#pragma once
#include <Windows.h>
#include <DirectXMath.h>

// -----------------------------------------------------------------------
// Free Camera for Splinter Cell: Blacklist (UE3-based, 64-bit build)
//
// The camera matrix pointer is located at runtime via pattern scanning.
// If the pattern breaks on a patch, update CAM_MATRIX_SIG below.
//
// Memory layout of the in-game camera struct we care about:
//   struct CameraState {
//       float    viewMatrix[16];   // offset 0x00  (row-major 4x4)
//       float    fov;              // offset 0x40
//       float    position[3];      // offset 0x44
//       float    rotation[3];      // offset 0x50  (pitch, yaw, roll in radians)
//   };
// -----------------------------------------------------------------------

namespace FreeCam {

    // ----------------------------------------------------------------
    // Signatures (IDA-style, update if the game patches)
    // ----------------------------------------------------------------

    // Signature that leads to the camera state pointer, plus the offset of the
    // 4-byte address field inside the matched instruction and (x64 only) the
    // total instruction size. See REVERSE_ENGINEERING.md for how to find these.
#ifdef _WIN64
    // ---- 64-bit build (RIP-relative; e.g. `48 8B 0D <rel32>` mov rcx,[rip+x]) ----
    static constexpr const char* CAM_MATRIX_SIG =
        "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? F3 0F 11 41";
    static constexpr int CAM_SIG_REL32_OFFSET = 3;   // offset of the rel32 inside the instruction
    static constexpr int CAM_SIG_INSTR_SIZE   = 7;   // total instruction size
#else
    // ---- 32-bit build (absolute; e.g. `8B 0D <imm32>` mov ecx,[imm32]) ----
    // PLACEHOLDER — replace with a real x86 pattern from your game. The bytes
    // here will NOT match anything (freecam stays disabled until you fill it).
    // REL32_OFFSET here means "offset of the 4-byte absolute address field".
    static constexpr const char* CAM_MATRIX_SIG =
        "00 00 00 00 00 00 00 00 00 00 00 00";
    static constexpr int CAM_SIG_REL32_OFFSET = 2;   // operand offset (e.g. after `8B 0D`)
    static constexpr int CAM_SIG_INSTR_SIZE   = 6;   // unused on x86, kept for the API
#endif

    // ----------------------------------------------------------------
    // Config knobs exposed to the mod menu
    // ----------------------------------------------------------------
    struct Config {
        bool  enabled        = false;
        float moveSpeed      = 5.0f;    // units/second
        float lookSensitivity = 0.15f; // degrees per pixel
        bool  freezeTime     = false;
    };

    extern Config g_config;

    // ----------------------------------------------------------------
    // Camera state written each frame when free-cam is active
    // ----------------------------------------------------------------
    struct State {
        DirectX::XMFLOAT3 position  {0, 0, 0};
        float             pitch     = 0.0f;  // degrees
        float             yaw       = 0.0f;  // degrees
        float             roll      = 0.0f;  // degrees (usually kept 0)
    };

    extern State g_state;

    // ----------------------------------------------------------------
    // Lifecycle
    // ----------------------------------------------------------------
    bool Init();            // called once after the game DLL is ready; scans patterns
    void Update(float dt);  // called every frame; applies camera when enabled
    void Toggle();
    void Shutdown();

    // Directly set the camera struct base (for testing cam_finder results).
    // If the freecam position tracks your in-game position after calling this,
    // the address is correct.  Use the 'Force Cam Base' box in the menu.
    void ForceBase(uintptr_t addr);

    // ----------------------------------------------------------------
    //  Accessors used by the level editor's gizmo (world-to-screen).
    //  All return false if the camera state pointer is not yet resolved.
    // ----------------------------------------------------------------
    bool GetViewMatrix(DirectX::XMMATRIX& out);
    bool GetCameraPosition(DirectX::XMFLOAT3& out);
    bool GetFov(float& outRadians);

    // Builds view * projection. aspect comes from the caller (ImGui display
    // size); near/far are reasonable defaults overridable here.
    bool GetViewProjection(DirectX::XMMATRIX& out, float aspect,
                           float nearZ = 1.0f, float farZ = 100000.0f);

} // namespace FreeCam
