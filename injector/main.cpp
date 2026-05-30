// -----------------------------------------------------------------------
//  Blacklist DLL Injector
//  Finds the running Blacklist process and injects BlacklistMod.dll via
//  CreateRemoteThread + LoadLibraryW.
// -----------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
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

// Enumerate running processes and return the PID of the first whose executable
// name matches any of the given names (case-insensitive), or 0 if none match.
// Uses EnumProcesses + QueryFullProcessImageNameW rather than the Toolhelp
// snapshot API for broader toolchain/SDK compatibility.
static DWORD FindProcessId(const wchar_t* const names[], size_t count) {
    DWORD pids[2048];
    DWORD bytesReturned = 0;
    if (!EnumProcesses(pids, sizeof(pids), &bytesReturned))
        return 0;

    const DWORD numProcs = bytesReturned / sizeof(DWORD);
    for (DWORD i = 0; i < numProcs; ++i) {
        if (pids[i] == 0)
            continue;

        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (!proc)
            continue;

        wchar_t imagePath[MAX_PATH]{};
        DWORD pathLen = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, imagePath, &pathLen)) {
            // Reduce the full path to just the file name for comparison.
            std::wstring path(imagePath, pathLen);
            size_t slash = path.find_last_of(L"\\/");
            const wchar_t* exeName =
                (slash == std::wstring::npos) ? path.c_str()
                                              : path.c_str() + slash + 1;
            for (size_t n = 0; n < count; ++n) {
                if (_wcsicmp(exeName, names[n]) == 0) {
                    CloseHandle(proc);
                    return pids[i];
                }
            }
        }
        CloseHandle(proc);
    }
    return 0;
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

// Returns true if the target process is 32-bit (running under WOW64). The
// injector and BlacklistMod.dll are built x64, so a 32-bit target is a hard
// incompatibility: a 64-bit DLL cannot be loaded into a 32-bit process.
static bool TargetIsWow64(HANDLE proc) {
    BOOL wow64 = FALSE;
    IsWow64Process(proc, &wow64);
    return wow64 != FALSE;
}

// Returns true if a module with the given base name is loaded in the target.
static bool IsModuleLoaded(HANDLE proc, const wchar_t* moduleName) {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(proc, mods, sizeof(mods), &needed, LIST_MODULES_ALL))
        return false;

    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleBaseNameW(proc, mods[i], name, MAX_PATH) &&
            _wcsicmp(name, moduleName) == 0)
            return true;
    }
    return false;
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

    // Bail early on an architecture mismatch — it would otherwise look like a
    // mysterious "LoadLibrary returned NULL" failure.
    if (TargetIsWow64(proc)) {
        std::wcerr <<
            L"\nERROR: the game is a 32-bit process, but this injector and\n"
            L"BlacklistMod.dll are 64-bit. A 64-bit DLL cannot be injected into\n"
            L"a 32-bit game. The mod would have to be rebuilt for 32-bit (x86)\n"
            L"to attach to this game.\n";
        CloseHandle(proc);
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
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);

    // Don't trust GetExitCodeThread here: it only returns the low 32 bits of
    // LoadLibraryW's 64-bit HMODULE, so a successful load can look like NULL.
    // Verify by checking whether the module is actually present in the target.
    bool loaded = IsModuleLoaded(proc, kDllName);
    CloseHandle(proc);

    if (!loaded) {
        std::wcerr << L"The DLL did not load into the game. Common causes:\n"
                      L"  - architecture mismatch (game vs DLL bitness)\n"
                      L"  - a missing dependency next to BlacklistMod.dll\n";
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
