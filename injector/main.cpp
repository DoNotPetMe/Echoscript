// -----------------------------------------------------------------------
//  Blacklist DLL Injector
//  Finds the running Blacklist process and injects BlacklistMod.dll via
//  CreateRemoteThread + LoadLibraryW.
// -----------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <iostream>

// The mod hooks Direct3D 11, so it must attach to the DX11 build of the game.
// Blacklist ships two executables: Blacklist_game.exe (DX9) and
// Blacklist_DX11_game.exe (DX11). We prefer DX11 and warn if only DX9 is found.
static const wchar_t* kProcessNamesDX11[] = {
    L"Blacklist_DX11_game.exe",
};
static const wchar_t* kProcessNamesDX9[] = {
    L"Blacklist_game.exe",
};
static const wchar_t* kDllName = L"BlacklistMod.dll";

// Search the snapshot for any of the given process names.
// Returns the PID, or 0 if none are running.
static DWORD FindProcessId(const wchar_t* const names[], size_t count) {
    PROCESSENTRY32W entry{ sizeof(entry) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    DWORD pid = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            for (size_t i = 0; i < count; ++i) {
                if (_wcsicmp(entry.szExeName, names[i]) == 0) {
                    pid = entry.th32ProcessID;
                    break;
                }
            }
        } while (pid == 0 && Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

// Wait up to timeoutSec for the game to appear. Sets *isDX9 if only the DX9
// build was found (the overlay will not work in that case). Returns PID or 0.
static DWORD WaitForGame(int timeoutSec, bool* isDX9) {
    *isDX9 = false;
    for (int elapsed = 0; elapsed < timeoutSec; ++elapsed) {
        DWORD pid = FindProcessId(kProcessNamesDX11,
                                  _countof(kProcessNamesDX11));
        if (pid) return pid;

        pid = FindProcessId(kProcessNamesDX9, _countof(kProcessNamesDX9));
        if (pid) { *isDX9 = true; return pid; }

        if (elapsed == 0)
            std::wcout << L"Waiting for Blacklist to start...\n";
        Sleep(1000);
    }
    return 0;
}

// Build the absolute path to BlacklistMod.dll next to this executable.
static std::wstring DllPathNextToInjector() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring path(exePath);
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        path.resize(slash + 1);
    path += kDllName;
    return path;
}

// Inject dllPath into the process via CreateRemoteThread + LoadLibraryW.
static bool InjectDll(DWORD pid, const std::wstring& dllPath) {
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!proc) {
        std::wcerr << L"OpenProcess failed (" << GetLastError()
                   << L"). Run the injector as Administrator.\n";
        return false;
    }

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        std::wcerr << L"VirtualAllocEx failed (" << GetLastError() << L").\n";
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        std::wcerr << L"WriteProcessMemory failed (" << GetLastError() << L").\n";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(k32, "LoadLibraryW"));
    if (!loadLib) {
        std::wcerr << L"GetProcAddress(LoadLibraryW) failed.\n";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!thread) {
        std::wcerr << L"CreateRemoteThread failed (" << GetLastError() << L").\n";
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, INFINITE);

    DWORD remoteModule = 0;
    GetExitCodeThread(thread, &remoteModule);

    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (remoteModule == 0) {
        std::wcerr << L"LoadLibraryW returned NULL inside the game — the DLL "
                      L"failed to load (check it sits next to the injector).\n";
        return false;
    }
    return true;
}

static void PrintControls() {
    std::wcout <<
        L"\n=== BlacklistMod loaded ===\n"
        L"  INSERT      open / close the mod menu\n"
        L"  F5          toggle free camera\n"
        L"  W A S D     move (free-cam)        Q / E  up / down\n"
        L"  Shift       3x speed boost\n"
        L"  F6          toggle level editor\n"
        L"  DELETE      unload the mod\n\n";
}

int wmain() {
    std::wcout << L"Blacklist Mod Injector\n";

    std::wstring dllPath = DllPathNextToInjector();
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"ERROR: " << kDllName
                   << L" not found next to the injector.\n"
                   << L"Place BlacklistMod.dll in the same folder as this .exe.\n";
        std::wcout << L"\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    bool isDX9 = false;
    DWORD pid = WaitForGame(120, &isDX9);
    if (!pid) {
        std::wcerr << L"Timed out waiting for the game. Start Blacklist first.\n";
        std::wcout << L"\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    if (isDX9) {
        std::wcerr <<
            L"\nWARNING: The DX9 build (Blacklist_game.exe) was detected.\n"
            L"This mod hooks Direct3D 11 and the overlay will NOT appear.\n"
            L"Launch the DX11 build (Blacklist_DX11_game.exe) instead.\n\n";
    }

    std::wcout << L"Found game (PID " << pid << L"). Injecting "
               << kDllName << L"...\n";

    if (!InjectDll(pid, dllPath)) {
        std::wcout << L"\nPress Enter to exit...";
        std::cin.get();
        return 1;
    }

    std::wcout << L"Injection succeeded.\n";
    PrintControls();
    std::wcout << L"You can close this window; the mod runs inside the game.\n";
    return 0;
}
