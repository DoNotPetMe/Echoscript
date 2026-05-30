#pragma once
#include <Windows.h>
#include <DirectXMath.h>
#include <cstdint>

// -----------------------------------------------------------------------
// Free Camera / Free Roam for Splinter Cell: Blacklist (32-bit DX11 build)
//
// Reverse-engineering credit: the coordinate struct layout and injection
// site below were derived from Paul44's Cheat Engine table (FearLess
// Revolution, "SC Blacklist v2.2"). See CAMERA_OFFSETS.md.
//
// There is NO static pointer to the player/camera position struct. Instead
// the game exposes it in the ESI register at a specific instruction. We
// AOB-scan for that instruction, install a tiny trampoline that copies ESI
// into g_structBase every time it runs, and then read/write the coordinates
// directly:
//
//   struct PositionBlock {            // base == captured ESI
//       ...
//       float x;   // base + 0x9C
//       float y;   // base + 0xA0
//       float z;   // base + 0xA4
//   };
//
// CE injection point (for reference):
//   Blacklist_DX11_game.exe+95D6A6: F3 0F 10 86 9C 00 00 00
//                                    movss xmm0,[esi+0000009C]
// -----------------------------------------------------------------------

namespace FreeCam {

    // ----------------------------------------------------------------
    // Signatures (IDA-style). Update if the game patches.
    // ----------------------------------------------------------------
    //
    // COORD_SIG matches the player camera read at +95D6A6:
    //   movss xmm0,[esi+0x9C]   (F3 0F 10 86 9C 00 00 00)
    //   movss xmm1,[esi+0x250]  (F3 0F 10 8E 50 ...)
    // The match address is the injection point; ESI there is the struct base.
    static constexpr const char* COORD_SIG =
        "F3 0F 10 86 9C 00 00 00 F3 0F 10 8E 50";

    // FREEROAM_SIG matches the engine's position-copy write at +18880E:
    //   movq [ebx+0x9C],xmm0    (66 0F D6 83 9C 00 00 00)
    //   mov  eax,[esi+08]       (8B 46 ...)
    // EBX there is the struct base; we substitute our coords for the player's.
    static constexpr const char* FREEROAM_SIG =
        "66 0F D6 83 9C 00 00 00 8B 46";

    // Offsets of the X/Y/Z position floats inside the captured struct.
    static constexpr unsigned OFF_POS_X = 0x9C;
    static constexpr unsigned OFF_POS_Y = 0xA0;
    static constexpr unsigned OFF_POS_Z = 0xA4;

    // Module-relative static camera block (from Paul44's CE table, camera
    // section @ +1AFDEEB). These are written for the "detached camera" mode
    // so the view moves without dragging Sam's body. X/Y/Z are inferred;
    // pitch (+30BACC4) is the one CE labelled. See CAMERA_OFFSETS.md.
    static constexpr unsigned CAM_BLOCK_X     = 0x30BACB8;
    static constexpr unsigned CAM_BLOCK_Y     = 0x30BACBC;
    static constexpr unsigned CAM_BLOCK_Z     = 0x30BACC0;
    static constexpr unsigned CAM_BLOCK_PITCH = 0x30BACC4;

    // ----------------------------------------------------------------
    // Config knobs exposed to the mod menu
    // ----------------------------------------------------------------
    struct Config {
        bool  enabled         = false;
        float moveSpeed       = 300.0f;  // units/second (UE units ~= cm)
        float lookSensitivity = 0.15f;   // degrees per pixel (HUD only for now)
        bool  freezeTime      = false;
        // When true (default) free-roam writes the player struct, so Sam's
        // body travels with the camera (reliable). When false it writes the
        // static camera block instead -- view detaches from Sam (experimental).
        bool  moveSam         = true;
    };

    extern Config g_config;

    // ----------------------------------------------------------------
    // Camera state mirrored each frame while free-cam is active
    // ----------------------------------------------------------------
    struct State {
        DirectX::XMFLOAT3 position {0, 0, 0};  // x, y, z in world units
        float             pitch    = 0.0f;     // degrees (HUD)
        float             yaw      = 0.0f;     // degrees (HUD)
        float             roll     = 0.0f;
    };

    extern State g_state;

    // ----------------------------------------------------------------
    // Lifecycle
    // ----------------------------------------------------------------
    bool Init();            // installs the ESI-capture trampoline
    void Update(float dt);  // called every frame; writes coords when enabled
    void Toggle();
    void Shutdown();        // removes the trampoline, restores original bytes

    // True once the trampoline has run at least once (i.e. the game executed
    // the hooked instruction and we captured a struct base). It only fires
    // while you're actually in a loaded level.
    bool HasCapturedBase();

    // The currently active struct base (forced override if set, else captured).
    uintptr_t CurrentBase();

    // Manually override the struct base (the 'Force Cam Base' menu box). Pass 0
    // to clear the override and fall back to the auto-captured base.
    void ForceBase(uintptr_t addr);

    // ----------------------------------------------------------------
    //  Accessors used by the level editor's gizmo. Only position is
    //  available from this struct; the view matrix / FOV are not, so those
    //  return false (world-to-screen gizmo is disabled for this title).
    // ----------------------------------------------------------------
    bool GetViewMatrix(DirectX::XMMATRIX& out);
    bool GetCameraPosition(DirectX::XMFLOAT3& out);
    bool GetFov(float& outRadians);
    bool GetViewProjection(DirectX::XMMATRIX& out, float aspect,
                           float nearZ = 1.0f, float farZ = 100000.0f);

} // namespace FreeCam
