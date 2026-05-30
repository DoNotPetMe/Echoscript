#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace Memory {

    // Safe read — returns false if the address is not readable
    template<typename T>
    inline bool SafeRead(uintptr_t addr, T& out) {
        __try {
            out = *reinterpret_cast<T*>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Safe write — returns false if the address is not writable
    template<typename T>
    inline bool SafeWrite(uintptr_t addr, const T& val) {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T),
                            PAGE_EXECUTE_READWRITE, &old))
            return false;
        *reinterpret_cast<T*>(addr) = val;
        VirtualProtect(reinterpret_cast<void*>(addr), sizeof(T), old, &old);
        return true;
    }

    // Follow a chain of relative offsets (pointer chase)
    // e.g. Chase(base, {0x10, 0x4, 0x60}) == *(*(*(base+0x10)+0x4)+0x60)
    inline uintptr_t Chase(uintptr_t base, std::initializer_list<uintptr_t> offsets) {
        uintptr_t addr = base;
        for (auto off : offsets) {
            if (!addr) return 0;
            uintptr_t next = 0;
            if (!SafeRead(addr + off, next)) return 0;
            addr = next;
        }
        return addr;
    }

    // VMT hook helpers
    inline void** GetVTable(void* obj) {
        return *reinterpret_cast<void***>(obj);
    }

    inline void* HookVMT(void** vtable, size_t index, void* newFn) {
        DWORD old;
        VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        void* orig = vtable[index];
        vtable[index] = newFn;
        VirtualProtect(&vtable[index], sizeof(void*), old, &old);
        return orig;
    }

    inline void UnhookVMT(void** vtable, size_t index, void* origFn) {
        DWORD old;
        VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        vtable[index] = origFn;
        VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    }

} // namespace Memory
