#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>
#include <psapi.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>

// Target process name for Splinter Cell: Blacklist
static constexpr std::wstring_view TARGET_PROCESS = L"Blacklist_game.exe";
static constexpr int MAX_WAIT_SECONDS = 60;

static DWORD FindProcessId(std::wstring_view processName) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (processName == entry.szExeFile) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

static bool InjectDLL(DWORD pid, const std::wstring& dllPath) {
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!proc) {
        wprintf(L"[-] OpenProcess failed: %lu\n", GetLastError());
        return false;
    }

    // Allocate memory in target process for the DLL path
    const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(proc, nullptr, pathBytes,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        wprintf(L"[-] VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remotePath, dllPath.c_str(), pathBytes, nullptr)) {
        wprintf(L"[-] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remotePath, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    // Resolve LoadLibraryW in kernel32 (same address in all processes on Windows)
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadLib =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(k32, "LoadLibraryW"));

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLib, remotePath, 0, nullptr);
    if (!thread) {
        wprintf(L"[-] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remotePath, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    // Wait for injection thread to complete
    WaitForSingleObject(thread, 8000);

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);

    CloseHandle(thread);
    VirtualFreeEx(proc, remotePath, 0, MEM_RELEASE);
    CloseHandle(proc);

    // LoadLibraryW returns the module handle; 0 means failure
    return exitCode != 0;
}

int wmain(int argc, wchar_t* argv[]) {
    wprintf(L"=================================================\n");
    wprintf(L"  Splinter Cell: Blacklist — Mod Injector v1.0\n");
    wprintf(L"=================================================\n\n");

    // Resolve mod DLL path
    std::wstring dllPath;
    if (argc >= 2) {
        dllPath = argv[1];
    } else {
        wchar_t selfPath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        std::filesystem::path p(selfPath);
        p = p.parent_path() / L"BlacklistMod.dll";
        dllPath = p.wstring();
    }

    if (!std::filesystem::exists(dllPath)) {
        wprintf(L"[-] DLL not found: %s\n", dllPath.c_str());
        wprintf(L"    Place BlacklistMod.dll next to the injector, or pass its path as an argument.\n");
        return 1;
    }

    wprintf(L"[*] DLL path : %s\n", dllPath.c_str());
    wprintf(L"[*] Waiting for %s (up to %d seconds)...\n\n",
            TARGET_PROCESS.data(), MAX_WAIT_SECONDS);

    DWORD pid = 0;
    for (int i = 0; i < MAX_WAIT_SECONDS * 2; ++i) {
        pid = FindProcessId(TARGET_PROCESS);
        if (pid) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (i % 4 == 0) wprintf(L".");
    }
    wprintf(L"\n");

    if (!pid) {
        wprintf(L"[-] Game process not found. Launch the game first.\n");
        return 1;
    }

    wprintf(L"[+] Found %s  (PID %lu)\n", TARGET_PROCESS.data(), pid);

    // Brief delay so the game's D3D11 device has time to initialise
    wprintf(L"[*] Waiting 3 seconds for game to finish loading D3D11...\n");
    std::this_thread::sleep_for(std::chrono::seconds(3));

    wprintf(L"[*] Injecting mod DLL...\n");
    if (InjectDLL(pid, dllPath)) {
        wprintf(L"[+] Injection successful!\n");
        wprintf(L"\n    In-game controls:\n");
        wprintf(L"      INSERT      - Toggle mod menu\n");
        wprintf(L"      F5          - Toggle free camera\n");
        wprintf(L"      W/A/S/D     - Move camera (free-cam active)\n");
        wprintf(L"      Q / E       - Camera up / down\n");
        wprintf(L"      Mouse       - Look around\n");
        wprintf(L"      Scroll      - Adjust move speed\n");
        wprintf(L"      DELETE      - Unload mod\n");
    } else {
        wprintf(L"[-] Injection failed. Try running as Administrator.\n");
        return 1;
    }

    return 0;
}
