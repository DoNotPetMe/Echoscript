#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdio>
#include <string_view>

namespace Logger {

    inline FILE* g_log = nullptr;

    inline void Init() {
        AllocConsole();
        freopen_s(&g_log, "CONOUT$", "w", stdout);
        SetConsoleTitleA("Blacklist Mod — Debug Console");
        printf("[BlacklistMod] Console initialised\n");
    }

    inline void Shutdown() {
        if (g_log) fclose(g_log);
        FreeConsole();
    }

    template<typename... Args>
    inline void Info(std::string_view fmt, Args&&... args) {
        printf("[+] ");
        printf(fmt.data(), std::forward<Args>(args)...);
        printf("\n");
    }

    template<typename... Args>
    inline void Warn(std::string_view fmt, Args&&... args) {
        printf("[!] ");
        printf(fmt.data(), std::forward<Args>(args)...);
        printf("\n");
    }

    template<typename... Args>
    inline void Error(std::string_view fmt, Args&&... args) {
        printf("[-] ");
        printf(fmt.data(), std::forward<Args>(args)...);
        printf("\n");
    }

} // namespace Logger
