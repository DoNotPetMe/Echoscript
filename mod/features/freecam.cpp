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
#include <Xinput.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

using namespace DirectX;

namespace FreeCam {

Config g_config{};
State  g_state{};

// -----------------------------------------------------------------------
//  Two trampolines, both ported from Paul44's CE table:
//
//  1. COORD (read) hook @ Blacklist_DX11_game.exe+95D6A6
//        F3 0F 10 86 9C 00 00 00  movss xmm0,[esi+0x9C]
//     This runs in the *player* camera path, so ESI here is the player's
//     position struct. We copy ESI out to identify which struct is ours.
//
//  2. FREEROAM (write) hook @ Blacklist_DX11_game.exe+18880E
//        66 0F D6 83 9C 00 00 00  movq [ebx+0x9C],xmm0
//     This is a generic position-copy that runs for many objects. When EBX
//     matches the player struct captured above AND free-roam is on, we
//     substitute our desired X/Y/Z so the engine writes *our* position
//     instead of the source's -- making the position stick.
// -----------------------------------------------------------------------

namespace {
    // Captured player struct base (from the COORD read hook).
    volatile uintptr_t g_structBase = 0;
    // Manual override from the menu (0 = use auto-captured).
    uintptr_t          g_forcedBase = 0;

    // Shared with the FREEROAM asm stub:
    volatile uintptr_t g_activeBase      = 0;   // base the write hook should match
    volatile uint8_t   g_freeRoamActive  = 0;   // 1 = substitute our coords
    volatile float     g_desired[3]      = {0, 0, 0}; // X, Y, Z to force

    // COORD hook bookkeeping.
    uintptr_t g_coordInject = 0;
    uintptr_t g_coordReturn = 0;
    uint8_t   g_coordOrig[8] = {0};
    bool      g_coordHooked  = false;

    // FREEROAM hook bookkeeping.
    uintptr_t g_frInject = 0;
    uintptr_t g_frReturn = 0;
    uint8_t   g_frOrig[8] = {0};
    bool      g_frHooked  = false;

    // Raw input tracking for the on-screen pitch/yaw readout.
    POINT g_lastMouse{};
    bool  g_mouseInitialized = false;
}

// ----- Trampoline 1: capture the player's struct base from ESI -----
__declspec(naked) static void CoordHookStub() {
    __asm {
        mov  g_structBase, esi
        // original: movss xmm0, dword ptr [esi+0x9C]
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

// ----- Trampoline 2: substitute our coords on the engine's write -----
__declspec(naked) static void FreeRoamStub() {
    __asm {
        // Only substitute when free-roam is active...
        cmp  byte ptr g_freeRoamActive, 0
        je   do_original
        // ...and only for the player's struct (EBX == captured base).
        mov  eax, g_activeBase
        test eax, eax
        je   do_original
        cmp  ebx, eax
        jne  do_original

        // Load our X,Y into xmm0 (low 64 bits) so the store below writes them.
        movq xmm0, qword ptr [g_desired]
        // Overwrite the source Z at [esi+08] so the engine's follow-up
        //   mov eax,[esi+08] / mov [ebx+0xA4],eax
        // stores our Z instead of the source's.
        mov  eax, dword ptr [g_desired+8]
        mov  [esi+8], eax

    do_original:
        // original: movq qword ptr [ebx+0x9C], xmm0
        _emit 0x66
        _emit 0x0F
        _emit 0xD6
        _emit 0x83
        _emit 0x9C
        _emit 0x00
        _emit 0x00
        _emit 0x00
        jmp  g_frReturn
    }
}

// -----------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------
static bool IsKeyHeld(int vk)    { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static bool IsKeyPressed(int vk) { return (GetAsyncKeyState(vk) & 0x0001) != 0; }

// Per-frame gamepad movement axes, normalized to [-1, 1] after deadzones.
struct PadInput {
    bool  connected = false;
    float moveX     = 0.f;  // left stick X  -> world Y (strafe)
    float moveY     = 0.f;  // left stick Y  -> world X (forward/back)
    float upDown    = 0.f;  // triggers      -> world Z (RT up, LT down)
    bool  sprint    = false; // A button (or stick click)
};

// Read the first connected controller. The game also reads this controller,
// so we only *observe* it (no input grab) -- both move and fly work together.
static PadInput ReadPad() {
    PadInput out{};
    XINPUT_STATE st{};
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        out.connected = true;
        const auto& gp = st.Gamepad;

        // Radial deadzone on the left stick.
        float lx = static_cast<float>(gp.sThumbLX);
        float ly = static_cast<float>(gp.sThumbLY);
        constexpr float DZ = static_cast<float>(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        float mag = std::sqrt(lx * lx + ly * ly);
        if (mag > DZ) {
            float norm = std::min(mag, 32767.f);
            float scaled = (norm - DZ) / (32767.f - DZ);   // 0..1 past deadzone
            float inv = scaled / norm;
            out.moveX = lx * inv;
            out.moveY = ly * inv;
        }

        // Triggers (0..255) -> up/down, with a small threshold.
        constexpr float TT = static_cast<float>(XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
        if (gp.bRightTrigger > TT) out.upDown += (gp.bRightTrigger - TT) / (255.f - TT);
        if (gp.bLeftTrigger  > TT) out.upDown -= (gp.bLeftTrigger  - TT) / (255.f - TT);

        // A button or left-stick click = sprint.
        out.sprint = (gp.wButtons & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_LEFT_THUMB)) != 0;
        break;  // first active controller only
    }
    return out;
}

uintptr_t CurrentBase() {
    return g_forcedBase ? g_forcedBase : static_cast<uintptr_t>(g_structBase);
}

bool HasCapturedBase() { return g_structBase != 0; }

// Patch `inj` (8 bytes) with E9 <rel32> + NOPs to jump to `stub`. Saves the
// original bytes into `origOut`. Returns false on failure.
static bool InstallJmp(uintptr_t inj, const void* stub,
                       const uint8_t expect[], size_t expectLen,
                       uint8_t origOut[8], const char* name) {
    if (std::memcmp(reinterpret_cast<void*>(inj), expect, expectLen) != 0) {
        Logger::Warn("FreeCam: %s site 0x%08X bytes mismatch -- aborting hook.",
                     name, static_cast<unsigned>(inj));
        return false;
    }
    std::memcpy(origOut, reinterpret_cast<void*>(inj), 8);

    uint8_t patch[8];
    int32_t rel = static_cast<int32_t>(
        reinterpret_cast<uintptr_t>(stub) - (inj + 5));
    patch[0] = 0xE9;
    std::memcpy(patch + 1, &rel, 4);
    patch[5] = patch[6] = patch[7] = 0x90;

    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(inj), 8, PAGE_EXECUTE_READWRITE, &old)) {
        Logger::Warn("FreeCam: VirtualProtect failed for %s @ 0x%08X.",
                     name, static_cast<unsigned>(inj));
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(inj), patch, 8);
    VirtualProtect(reinterpret_cast<void*>(inj), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(inj), 8);
    return true;
}

static void RestoreBytes(uintptr_t inj, const uint8_t orig[8]) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(inj), 8, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(reinterpret_cast<void*>(inj), orig, 8);
    VirtualProtect(reinterpret_cast<void*>(inj), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(inj), 8);
}

// -----------------------------------------------------------------------
//  Init — install both trampolines
// -----------------------------------------------------------------------
bool Init() {
    Logger::Info("FreeCam: scanning for coordinate hook sites...");

    // ---- COORD read hook (struct-base capture) ----
    uintptr_t coord = PatternScan::ScanGame(COORD_SIG);
    if (!coord) {
        Logger::Warn("FreeCam: COORD signature not found -- free-roam disabled.");
        Logger::Warn("         If the game was patched, update COORD_SIG in freecam.h.");
        return false;
    }
    const uint8_t coordExpect[5] = { 0xF3, 0x0F, 0x10, 0x86, 0x9C };
    g_coordInject = coord;
    g_coordReturn = coord + 8;
    if (!InstallJmp(coord, reinterpret_cast<void*>(&CoordHookStub),
                    coordExpect, sizeof(coordExpect), g_coordOrig, "COORD")) {
        return false;
    }
    g_coordHooked = true;
    Logger::Info("FreeCam: COORD hook @ 0x%08X (captures player struct).",
                 static_cast<unsigned>(coord));

    // ---- FREEROAM write hook (sticky position) ----
    uintptr_t fr = PatternScan::ScanGame(FREEROAM_SIG);
    if (fr) {
        const uint8_t frExpect[5] = { 0x66, 0x0F, 0xD6, 0x83, 0x9C };
        g_frInject = fr;
        g_frReturn = fr + 8;
        if (InstallJmp(fr, reinterpret_cast<void*>(&FreeRoamStub),
                       frExpect, sizeof(frExpect), g_frOrig, "FREEROAM")) {
            g_frHooked = true;
            Logger::Info("FreeCam: FREEROAM hook @ 0x%08X (sticky writes ON).",
                         static_cast<unsigned>(fr));
        }
    } else {
        Logger::Warn("FreeCam: FREEROAM signature not found -- falling back to "
                     "per-frame writes (position may fight the engine).");
    }

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
        g_freeRoamActive  = 0;   // stop substituting -> engine resumes control
        g_mouseInitialized = false;
        return;
    }

    uintptr_t base = CurrentBase();
    g_activeBase = base;
    if (!base) { g_freeRoamActive = 0; return; }  // not in a level yet

    // ---- Mouse delta -> pitch/yaw readout (display only for now) ----
    POINT cur{};
    GetCursorPos(&cur);
    if (!g_mouseInitialized) { g_lastMouse = cur; g_mouseInitialized = true; }
    float dx = static_cast<float>(cur.x - g_lastMouse.x);
    float dy = static_cast<float>(cur.y - g_lastMouse.y);
    g_lastMouse = cur;
    g_state.yaw   += dx * g_config.lookSensitivity;
    g_state.pitch  = std::clamp(g_state.pitch + dy * g_config.lookSensitivity, -89.9f, 89.9f);

    // ---- World-axis fly (keyboard + gamepad) ----
    // X/Y/Z are world coordinates. WASD / left-stick move in the world XY
    // plane, Q/E / triggers go up and down. Camera-relative movement needs the
    // game yaw (static block, see CAMERA_OFFSETS.md) which isn't wired yet.
    PadInput pad = ReadPad();

    float speed = g_config.moveSpeed * dt;
    if (IsKeyHeld(VK_SHIFT) || pad.sprint) speed *= 3.0f;

    XMFLOAT3 p = g_state.position;

    // Keyboard (digital).
    if (IsKeyHeld('W')) p.x += speed;
    if (IsKeyHeld('S')) p.x -= speed;
    if (IsKeyHeld('D')) p.y += speed;
    if (IsKeyHeld('A')) p.y -= speed;
    if (IsKeyHeld('E')) p.z += speed;
    if (IsKeyHeld('Q')) p.z -= speed;

    // Gamepad (analog): left-stick Y = forward/back (world X), left-stick X =
    // strafe (world Y), triggers = up/down (world Z).
    p.x += pad.moveY  * speed;
    p.y += pad.moveX  * speed;
    p.z += pad.upDown * speed;

    g_state.position = p;

    g_config.moveSpeed = std::clamp(g_config.moveSpeed, 0.5f, 5000.0f);

    // Hand the desired coords to the write-hook (preferred), and if the hook
    // didn't install, fall back to a direct per-frame write.
    g_desired[0] = p.x;
    g_desired[1] = p.y;
    g_desired[2] = p.z;
    g_freeRoamActive = 1;

    if (!g_frHooked)
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
            XMFLOAT3 curPos{};
            if (Memory::SafeRead(base + OFF_POS_X, curPos)) {
                g_state.position = curPos;
                g_desired[0] = curPos.x;
                g_desired[1] = curPos.y;
                g_desired[2] = curPos.z;
            }
        }
        g_state.pitch = g_state.yaw = g_state.roll = 0.f;
        Logger::Info("FreeCam: enabled%s",
                     base ? "" : " (waiting for struct capture -- enter a level)");
    } else {
        g_freeRoamActive = 0;
        Logger::Info("FreeCam: disabled");
    }
    g_mouseInitialized = false;
}

void Shutdown() {
    g_config.enabled = false;
    g_freeRoamActive = 0;

    if (g_frHooked)    { RestoreBytes(g_frInject, g_frOrig);       g_frHooked = false; }
    if (g_coordHooked) { RestoreBytes(g_coordInject, g_coordOrig); g_coordHooked = false; }

    g_structBase = 0;
    g_forcedBase = 0;
    g_activeBase = 0;
}

void ForceBase(uintptr_t addr) {
    g_forcedBase = addr;
    if (addr)
        Logger::Info("FreeCam: base override set to 0x%08X.", static_cast<unsigned>(addr));
    else
        Logger::Info("FreeCam: base override cleared -- using auto-captured base.");
}

// -----------------------------------------------------------------------
//  Accessors for the level editor's gizmo. Only position is available.
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
