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
#include <cmath>
#include <numbers>

using namespace DirectX;

namespace FreeCam {

Config g_config{};
State  g_state{};

// Pointer to the game's live camera state struct
static uintptr_t g_camStatePtr = 0;

// Raw input tracking for mouse delta
static POINT g_lastMouse{};
static bool  g_mouseInitialized = false;

// Offsets into the camera state struct (see freecam.h header comment)
static constexpr size_t OFF_VIEW_MATRIX = 0x00;
static constexpr size_t OFF_FOV         = 0x40;
static constexpr size_t OFF_POSITION    = 0x44;
static constexpr size_t OFF_ROTATION    = 0x50;

// -----------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------

static float ToRad(float deg) {
    return deg * (std::numbers::pi_v<float> / 180.f);
}

static bool IsKeyHeld(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static bool IsKeyPressed(int vk) {
    return (GetAsyncKeyState(vk) & 0x0001) != 0;
}

// Write a 4x4 row-major matrix to game memory
static void WriteViewMatrix(const XMMATRIX& mat) {
    if (!g_camStatePtr) return;
    uintptr_t dst = g_camStatePtr + OFF_VIEW_MATRIX;
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, mat);
    Memory::SafeWrite(dst, m);
}

static void WritePosition(const XMFLOAT3& pos) {
    if (!g_camStatePtr) return;
    Memory::SafeWrite(g_camStatePtr + OFF_POSITION, pos);
}

// -----------------------------------------------------------------------
//  Init
// -----------------------------------------------------------------------

bool Init() {
    Logger::Info("FreeCam: scanning for camera state pointer...");

    uintptr_t instr = PatternScan::ScanGame(CAM_MATRIX_SIG);
    if (!instr) {
        Logger::Warn("FreeCam: camera pattern not found — free camera will be disabled.");
        Logger::Warn("         If the game was updated, update CAM_MATRIX_SIG in freecam.h.");
        return false;
    }

    // Resolve the pointer baked into the instruction. x64 uses a RIP-relative
    // displacement; x86 embeds the absolute address directly. ResolvePtrOperand
    // picks the right mode for the build architecture.
    uintptr_t ptrAddr = PatternScan::ResolvePtrOperand(instr, CAM_SIG_REL32_OFFSET, CAM_SIG_INSTR_SIZE);
    uintptr_t camPtr  = 0;
    if (!Memory::SafeRead(ptrAddr, camPtr) || !camPtr) {
        Logger::Warn("FreeCam: camera pointer resolved to null — is the game fully loaded?");
        return false;
    }

    g_camStatePtr = camPtr;
    Logger::Info("FreeCam: camera state @ 0x%p", reinterpret_cast<void*>(g_camStatePtr));
    return true;
}

// -----------------------------------------------------------------------
//  Update (called every frame from the D3D Present hook)
// -----------------------------------------------------------------------

void Update(float dt) {
    // Allow toggle via F5
    if (IsKeyPressed(VK_F5))
        Toggle();

    if (!g_config.enabled) {
        g_mouseInitialized = false;
        return;
    }

    if (!g_camStatePtr) {
        // Retry pattern scan in case the game wasn't fully loaded at Init time
        Init();
        if (!g_camStatePtr) return;
    }

    // ---- Mouse look ----
    POINT curMouse{};
    GetCursorPos(&curMouse);

    if (!g_mouseInitialized) {
        g_lastMouse       = curMouse;
        g_mouseInitialized = true;
    }

    float dx = static_cast<float>(curMouse.x - g_lastMouse.x);
    float dy = static_cast<float>(curMouse.y - g_lastMouse.y);
    g_lastMouse = curMouse;

    g_state.yaw   += dx * g_config.lookSensitivity;
    g_state.pitch += dy * g_config.lookSensitivity;

    // Clamp pitch to avoid gimbal lock at exactly ±90°
    g_state.pitch = std::clamp(g_state.pitch, -89.9f, 89.9f);

    // ---- Build rotation quaternion ----
    XMVECTOR qPitch = XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), ToRad(g_state.pitch));
    XMVECTOR qYaw   = XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), ToRad(g_state.yaw));
    XMVECTOR qRot   = XMQuaternionMultiply(qPitch, qYaw);
    XMMATRIX rotMat = XMMatrixRotationQuaternion(qRot);

    // ---- Extract axes from rotation matrix ----
    XMVECTOR right   = XMVector3Normalize(rotMat.r[0]);
    XMVECTOR up      = XMVectorSet(0, 1, 0, 0);   // world up for vertical strafe
    XMVECTOR forward = XMVector3Normalize(rotMat.r[2]);

    // ---- Keyboard movement ----
    float speed = g_config.moveSpeed * dt;

    // Shift = 3x speed boost
    if (IsKeyHeld(VK_SHIFT)) speed *= 3.0f;

    XMVECTOR pos = XMLoadFloat3(&g_state.position);

    if (IsKeyHeld('W')) pos = XMVectorAdd(pos, XMVectorScale(forward, speed));
    if (IsKeyHeld('S')) pos = XMVectorSubtract(pos, XMVectorScale(forward, speed));
    if (IsKeyHeld('A')) pos = XMVectorSubtract(pos, XMVectorScale(right, speed));
    if (IsKeyHeld('D')) pos = XMVectorAdd(pos, XMVectorScale(right, speed));
    if (IsKeyHeld('Q')) pos = XMVectorAdd(pos, XMVectorScale(up, speed));
    if (IsKeyHeld('E')) pos = XMVectorSubtract(pos, XMVectorScale(up, speed));

    XMStoreFloat3(&g_state.position, pos);

    // ---- Scroll wheel: adjust move speed ----
    // (handled in menu.cpp via WM_MOUSEWHEEL; just clamp here)
    g_config.moveSpeed = std::clamp(g_config.moveSpeed, 0.5f, 200.0f);

    // ---- Build and write view matrix ----
    XMVECTOR eyePos  = XMLoadFloat3(&g_state.position);
    XMVECTOR lookAt  = XMVectorAdd(eyePos, forward);
    XMMATRIX viewMat = XMMatrixLookAtLH(eyePos, lookAt, XMVectorSet(0, 1, 0, 0));

    WriteViewMatrix(viewMat);
    XMFLOAT3 posF3;
    XMStoreFloat3(&posF3, eyePos);
    WritePosition(posF3);
}

// -----------------------------------------------------------------------
//  Toggle
// -----------------------------------------------------------------------

void Toggle() {
    g_config.enabled = !g_config.enabled;

    if (g_config.enabled) {
        // Snapshot current camera position from game memory as starting point
        if (g_camStatePtr) {
            XMFLOAT3 pos{};
            Memory::SafeRead(g_camStatePtr + OFF_POSITION, pos);
            g_state.position = pos;
            // Reset angles — the game writes rotation separately so we start neutral
            g_state.pitch = 0.f;
            g_state.yaw   = 0.f;
            g_state.roll  = 0.f;
        }
        Logger::Info("FreeCam: enabled");
    } else {
        Logger::Info("FreeCam: disabled");
    }
    g_mouseInitialized = false;
}

void Shutdown() {
    g_config.enabled  = false;
    g_camStatePtr     = 0;
}

// -----------------------------------------------------------------------
//  Accessors for the level editor's gizmo
// -----------------------------------------------------------------------
bool GetViewMatrix(XMMATRIX& out) {
    if (!g_camStatePtr) return false;
    XMFLOAT4X4 m{};
    if (!Memory::SafeRead(g_camStatePtr + OFF_VIEW_MATRIX, m)) return false;
    out = XMLoadFloat4x4(&m);
    return true;
}

bool GetCameraPosition(XMFLOAT3& out) {
    if (!g_camStatePtr) return false;
    return Memory::SafeRead(g_camStatePtr + OFF_POSITION, out);
}

bool GetFov(float& outRadians) {
    if (!g_camStatePtr) return false;
    float fov = 0.f;
    if (!Memory::SafeRead(g_camStatePtr + OFF_FOV, fov)) return false;
    // The cam struct stores FOV in degrees; convert to radians for projection.
    outRadians = ToRad(fov > 0.f ? fov : 75.0f);
    return true;
}

bool GetViewProjection(XMMATRIX& out, float aspect, float nearZ, float farZ) {
    XMMATRIX view;
    if (!GetViewMatrix(view)) return false;

    float fovRad = ToRad(75.0f);
    GetFov(fovRad);  // best-effort; falls back to 75° if unavailable

    if (aspect <= 0.f) aspect = 16.0f / 9.0f;
    XMMATRIX proj = XMMatrixPerspectiveFovLH(fovRad, aspect, nearZ, farZ);
    out = XMMatrixMultiply(view, proj);
    return true;
}

} // namespace FreeCam
