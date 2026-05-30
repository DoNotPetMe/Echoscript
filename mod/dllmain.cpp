#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "hooks/d3d11_hook.h"
#include "features/freecam.h"
#include "features/leveleditor.h"
#include "utils/logger.h"

// -----------------------------------------------------------------------
//  Main mod thread — runs inside the game process
// -----------------------------------------------------------------------
static DWORD WINAPI ModThread(LPVOID param) {
    HMODULE selfModule = reinterpret_cast<HMODULE>(param);

    Logger::Init();
    Logger::Info("BlacklistMod loaded  (thread 0x%lX)", GetCurrentThreadId());

    // Install D3D11 hook (blocks until D3D is available)
    if (!D3D11Hook::Install()) {
        Logger::Error("D3D11 hook failed — mod will unload.");
        Logger::Shutdown();
        FreeLibraryAndExitThread(selfModule, 1);
        return 1;
    }

    // Scan for camera patterns
    FreeCam::Init();

    // Bring up the level editor (seeds prop catalog; best-effort engine bridge)
    LevelEditor::Init();

    // Pump a minimal message loop so we can receive WM_QUIT (from the unload button)
    // and also handle the DELETE key unload shortcut.
    MSG msg{};
    while (true) {
        // Check for unload key
        if (GetAsyncKeyState(VK_DELETE) & 0x0001)
            break;

        // Check for queued messages (e.g. WM_QUIT from menu unload button)
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto unload;
        }

        Sleep(16);  // ~60 Hz polling; actual frame logic runs in Present hook
    }

unload:
    Logger::Info("BlacklistMod: unloading...");
    LevelEditor::Shutdown();
    FreeCam::Shutdown();
    D3D11Hook::Uninstall();
    Logger::Shutdown();

    FreeLibraryAndExitThread(selfModule, 0);
    return 0;
}

// -----------------------------------------------------------------------
//  DLL entry point
// -----------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(nullptr, 0, ModThread, hModule, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
