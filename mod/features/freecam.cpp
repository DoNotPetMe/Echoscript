#include "freecam.h"
#include "../utils/logger.h"
#include "../utils/memory.h"
#include "../utils/pattern_scan.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

using namespace DirectX;

namespace FreeCam {

Config g_config{};
State  g_state{};

// -----------------------------------------------------------------------
//  ESI-capture trampoline
//
//  We patch a 5-byte JMP over the 8-byte `movss xmm0,[esi+0x9C]` at the
//  injection site. Our stub copies ESI into g_structBase, re-executes the
//  original instruction, then jumps back to the byte after it. This mirrors
//  Paul44's CE script (`newmem: mov [pCoord],esi`).
// -----------------------------------------------------------------------

namespace {
    volatile uintptr_t g_structBase   = 0;   // updated every time the hook runs
    uintptr_t          g_forcedBase   = 0;   // manual override from the menu
    uintptr_t          g_coordInject  = 0;   // patched instruction address
    uintptr_t          g_coordReturn  = 0;   // resume address (inject + 8)
    uint8_t            g_coordOrig[8]  = {0}; // saved original bytes
    bool               g_coordHooked   = false;

    // Raw input tracking for the on-screen pitch/yaw readout.
    POINT g_lastMouse{};
    bool  g_mouseInitialized = false;
}

// The naked stub jumped to from the patched site. Keep it free-standing so
// its address is stable; it references the file-scope statics above.
__declspec(naked) static void CoordHookStub() {
    __asm {
        mov  g_structBase, esi
        // original instruction: movss xmm0, dword ptr [esi+0x9C]
        _emit 0xF3
        _emit 0x0F
        _emit 0x10
        _emit 0x86
        _emit 0x9C
        _emit 0x00
        _emit 0x00
        _emit 0x00
        jmp  g_coordReturn
    }
}

// -----------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------
static bool IsKeyHeld(int vk)    { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static bool IsKeyPressed(int vk) { return (GetAsyncKeyState(vk) & 0x0001) != 0; }

uintptr_t CurrentBase() {
    return g_forcedBase ? g_forcedBase : static_cast<uintptr_t>(g_structBase);
}

bool HasCapturedBase() { return g_structBase != 0; }

// -----------------------------------------------------------------------
//  Init — install the trampoline
// -----------------------------------------------------------------------
bool Init() {
    Logger::Info("FreeCam: scanning for coordinate injection site...");

    uintptr_t inj = PatternScan::ScanGame(COORD_SIG);
    if (!inj) {
        Logger::Warn("FreeCam: coordinate signature not found -- free-cam disabled.");
        Logger::Warn("         If the game was patched, update COORD_SIG in freecam.h.");
        return false;
    }

    // Sanity-check the bytes we are about to overwrite before patching.
    const uint8_t expect[5] = { 0xF3, 0x0F, 0x10, 0x86, 0x9C };
    if (std::memcmp(reinterpret_cast<void*>(inj), expect, sizeof(expect)) != 0) {
        Logger::Warn("FreeCam: site 0x%08X did not match expected bytes -- aborting hook.",
                     static_cast<unsigned>(inj));
        return false;
    }

    g_coordInject = inj;
    g_coordReturn = inj + 8;
    std::memcpy(g_coordOrig, reinterpret_cast<void*>(inj), 8);

    // Build: E9 <rel32> + 3x NOP (the original instruction is 8 bytes).
    uint8_t patch[8];
    int32_t rel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(&CoordHookStub) - (inj + 5));
    patch[0] = 0xE9;
    std::memcpy(patch + 1, &rel, 4);
    patch[5] = patch[6] = patch[7] = 0x90;

    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(inj), 8, PAGE_EXECUTE_READWRITE, &old)) {
        Logger::Warn("FreeCam: VirtualProtect failed at 0x%08X.", static_cast<unsigned>(inj));
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(inj), patch, 8);
    VirtualProtect(reinterpret_cast<void*>(inj), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(inj), 8);

    g_coordHooked = true;
    Logger::Info("FreeCam: coordinate hook installed @ 0x%08X.", static_cast<unsigned>(inj));
    Logger::Info("FreeCam: load into a level and move once to capture the struct base.");
    return true;
}

// -----------------------------------------------------------------------
//  Update (called every frame from the D3D Present hook)
// -----------------------------------------------------------------------
void Update(float dt) {
    if (IsKeyPressed(VK_F5))
        Toggle();

    if (!g_config.enabled) {
        g_mouseInitialized = false;
        return;
    }

    uintptr_t base = CurrentBase();
    if (!base) return;  // not captured yet (not in a level) and no override set

    // ---- Mouse delta -> pitch/yaw readout (display only for now) ----
    POINT cur{};
    GetCursorPos(&cur);
    if (!g_mouseInitialized) { g_lastMouse = cur; g_mouseInitialized = true; }
    float dx = static_cast<float>(cur.x - g_lastMouse.x);
    float dy = static_cast<float>(cur.y - g_lastMouse.y);
    g_lastMouse = cur;
    g_state.yaw   += dx * g_config.lookSensitivity;
    g_state.pitch  = std::clamp(g_state.pitch + dy * g_config.lookSensitivity, -89.9f, 89.9f);

    // ---- World-axis fly ----
    // X/Y/Z are world coordinates (Z is up). WASD moves in the world XY plane,
    // Q/E move down/up. Camera-relative movement needs the game's yaw, which
    // this struct doesn't expose -- world axes are predictable and verifiable.
    float speed = g_config.moveSpeed * dt;
    if (IsKeyHeld(VK_SHIFT)) speed *= 3.0f;

    XMFLOAT3 p = g_state.position;
    if (IsKeyHeld('W')) p.x += speed;
    if (IsKeyHeld('S')) p.x -= speed;
    if (IsKeyHeld('D')) p.y += speed;
    if (IsKeyHeld('A')) p.y -= speed;
    if (IsKeyHeld('E')) p.z += speed;
    if (IsKeyHeld('Q')) p.z -= speed;
    g_state.position = p;

    g_config.moveSpeed = std::clamp(g_config.moveSpeed, 0.5f, 5000.0f);

    // Write X/Y/Z as one contiguous 12-byte block at +0x9C.
    Memory::SafeWrite(base + OFF_POS_X, p);
}

// -----------------------------------------------------------------------
//  Toggle
// -----------------------------------------------------------------------
void Toggle() {
    g_config.enabled = !g_config.enabled;

    if (g_config.enabled) {
        uintptr_t base = CurrentBase();
        if (base) {
            // Snapshot the live coordinates so we start exactly where we are.
            XMFLOAT3 cur{};
            if (Memory::SafeRead(base + OFF_POS_X, cur))
                g_state.position = cur;
        }
        g_state.pitch = g_state.yaw = g_state.roll = 0.f;
        Logger::Info("FreeCam: enabled%s", base ? "" : " (waiting for struct capture -- enter a level)");
    } else {
        Logger::Info("FreeCam: disabled");
    }
    g_mouseInitialized = false;
}

void Shutdown() {
    g_config.enabled = false;
    if (g_coordHooked) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(g_coordInject), 8, PAGE_EXECUTE_READWRITE, &old);
        std::memcpy(reinterpret_cast<void*>(g_coordInject), g_coordOrig, 8);
        VirtualProtect(reinterpret_cast<void*>(g_coordInject), 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_coordInject), 8);
        g_coordHooked = false;
    }
    g_structBase = 0;
    g_forcedBase = 0;
}

void ForceBase(uintptr_t addr) {
    g_forcedBase = addr;
    if (addr)
        Logger::Info("FreeCam: base override set to 0x%08X.", static_cast<unsigned>(addr));
    else
        Logger::Info("FreeCam: base override cleared -- using auto-captured base.");
}

// -----------------------------------------------------------------------
//  Accessors for the level editor's gizmo
//  Only position is available from this struct.
// -----------------------------------------------------------------------
bool GetCameraPosition(XMFLOAT3& out) {
    uintptr_t base = CurrentBase();
    if (!base) return false;
    return Memory::SafeRead(base + OFF_POS_X, out);
}

bool GetViewMatrix(XMMATRIX&)                          { return false; }
bool GetFov(float&)                                    { return false; }
bool GetViewProjection(XMMATRIX&, float, float, float) { return false; }

} // namespace FreeCam
