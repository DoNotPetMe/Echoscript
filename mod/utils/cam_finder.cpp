#include "cam_finder.h"
#include "logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cmath>
#include <vector>
#include <atomic>
#include <cstring>

// -----------------------------------------------------------------------
//  Camera finder — motion-diff strategy
//
//  A static "is this an orthonormal matrix?" test matches thousands of
//  bone/mesh transforms.  The camera is distinguished by MOTION: when the
//  player walks, the camera's position changes by a large amount, while
//  bone/mesh matrices barely translate.  So we:
//    1. Snapshot every candidate matrix + its position.
//    2. Ask the player to WALK for a few seconds.
//    3. Re-read each candidate; keep only those whose position moved within
//       a "player-like" range (not zero, not teleport-sized).
//  That collapses the bone arrays and leaves a tiny list.
// -----------------------------------------------------------------------

namespace CamFinder {

static std::atomic<bool> s_scanning{ false };

static float Dot3(float ax, float ay, float az, float bx, float by, float bz) {
    return ax*bx + ay*by + az*bz;
}
static float Len2(float x, float y, float z) { return x*x + y*y + z*z; }

// True if the 16-float block is an orthonormal 3x3 rotation (row or column major).
static bool IsViewMatrix(const float* m) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float v = m[r * 4 + c];
            if (!std::isfinite(v) || fabsf(v) > 1.001f) return false;
        }
    {   // row-major
        float ax=m[0], ay=m[1], az=m[2];
        float bx=m[4], by=m[5], bz=m[6];
        float cx=m[8], cy=m[9], cz=m[10];
        if (fabsf(Len2(ax,ay,az)-1.f) < 0.02f && fabsf(Len2(bx,by,bz)-1.f) < 0.02f &&
            fabsf(Len2(cx,cy,cz)-1.f) < 0.02f && fabsf(Dot3(ax,ay,az,bx,by,bz)) < 0.02f &&
            fabsf(Dot3(ax,ay,az,cx,cy,cz)) < 0.02f && fabsf(Dot3(bx,by,bz,cx,cy,cz)) < 0.02f)
            return true;
    }
    {   // column-major
        float ax=m[0], ay=m[4], az=m[8];
        float bx=m[1], by=m[5], bz=m[9];
        float cx=m[2], cy=m[6], cz=m[10];
        if (fabsf(Len2(ax,ay,az)-1.f) < 0.02f && fabsf(Len2(bx,by,bz)-1.f) < 0.02f &&
            fabsf(Len2(cx,cy,cz)-1.f) < 0.02f && fabsf(Dot3(ax,ay,az,bx,by,bz)) < 0.02f &&
            fabsf(Dot3(ax,ay,az,cx,cy,cz)) < 0.02f && fabsf(Dot3(bx,by,bz,cx,cy,cz)) < 0.02f)
            return true;
    }
    return false;
}

// Read the 16-float matrix block at base. Returns false if unreadable.
static bool ReadMat(uintptr_t base, float out[16]) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(base),
                             out, sizeof(float) * 16, &got) && got == sizeof(float) * 16;
}

// Sum of absolute differences across the 9 rotation elements.
static float RotDelta(const float* a, const float* b) {
    float d = 0.f;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            d += fabsf(a[r * 4 + c] - b[r * 4 + c]);
    return d;
}

static float PosDist(const float* a, const float* b) {
    float dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

struct Cand { uintptr_t base; float mat[16]; };  // mat[0x44/4..] = position

static void CountdownWalk(const char* what, int secs) {
    Logger::Info("CamFinder: >>> %s <<< (%d seconds)", what, secs);
    for (int s = secs; s > 0; --s) { Logger::Info("   ...%d", s); Sleep(1000); }
}

static DWORD WINAPI ScanThread(LPVOID) {
    Logger::Info("CamFinder: phase 1 -- snapshotting candidate matrices...");

    std::vector<Cand> cands;
    cands.reserve(16384);

    uintptr_t addr = 0x10000u;
    while (addr < 0xFFF00000u) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
            break;

        bool readable = mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD) && mbi.RegionSize >= 80;

        if (readable) {
            std::vector<uint8_t> buf(mbi.RegionSize);
            SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(), mbi.BaseAddress, buf.data(),
                                  mbi.RegionSize, &got) && got >= 80) {
                for (size_t i = 0; i + 0x50 <= got; i += 16) {
                    const float* m = reinterpret_cast<const float*>(buf.data() + i);
                    if (!IsViewMatrix(m)) continue;
                    const float* p = reinterpret_cast<const float*>(buf.data() + i + 0x44);
                    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
                        continue;
                    float mag2 = p[0]*p[0] + p[1]*p[1] + p[2]*p[2];
                    if (mag2 < 1.f || mag2 > 1e12f) continue;
                    Cand c; c.base = addr + i;
                    std::memcpy(c.mat, m, sizeof(c.mat));
                    cands.push_back(c);
                }
            }
        }
        addr += mbi.RegionSize;
        if (addr == 0u) break;
    }
    Logger::Info("CamFinder: %zu candidates captured.", cands.size());

    // ---- Phase 2: LOOK AROUND without moving ----
    // The camera's rotation sweeps hugely while its position stays put. Bones
    // and world objects don't do this. This is the strongest discriminator.
    CountdownWalk("STAND STILL and LOOK AROUND with the mouse (turn a lot)", 4);

    constexpr float kRotChanged   = 0.40f;  // rotation must change clearly
    constexpr float kPosStayedPut = 30.0f;  // but position must stay roughly fixed

    std::vector<Cand> turners;
    for (auto& c : cands) {
        float now[16];
        if (!ReadMat(c.base, now) || !IsViewMatrix(now)) continue;
        if (RotDelta(c.mat, now) < kRotChanged)       continue;  // didn't rotate -> not camera
        if (PosDist(c.mat + 0x44/4, now + 0x44/4) > kPosStayedPut) continue; // moved -> not a still look
        Cand t; t.base = c.base; std::memcpy(t.mat, now, sizeof(t.mat));
        turners.push_back(t);
    }
    Logger::Info("CamFinder: %zu survived the look-around test.", turners.size());

    // ---- Phase 3: WALK to confirm the position tracks you ----
    CountdownWalk("now WALK forward in a straight line (don't turn)", 4);

    constexpr float kMinMove = 8.0f;
    constexpr float kMaxMove = 8000.0f;

    std::vector<Cand> finalists;
    for (auto& c : turners) {
        float now[16];
        if (!ReadMat(c.base, now) || !IsViewMatrix(now)) continue;
        float d = PosDist(c.mat + 0x44/4, now + 0x44/4);
        if (d < kMinMove || d > kMaxMove) continue;
        Cand f; f.base = c.base; std::memcpy(f.mat, now, sizeof(f.mat));
        finalists.push_back(f);
    }

    // If walking eliminated everything (e.g. you didn't move), fall back to the
    // look-around survivors -- those are already very likely the camera.
    std::vector<Cand>& result = finalists.empty() ? turners : finalists;
    const char* label = finalists.empty()
        ? "look-around survivors (walk phase found none -- did you move?)"
        : "FINAL candidates (rotated when you looked, moved when you walked)";

    Logger::Info("CamFinder: done -- %zu %s:", result.size(), label);
    if (result.empty()) {
        Logger::Warn("  Nothing matched. Re-run and be sure to LOOK AROUND a lot in phase 2,");
        Logger::Warn("  then WALK in phase 3. Big, deliberate movements work best.");
    }
    for (auto& m : result) {
        const float* p = m.mat + 0x44/4;
        Logger::Info("  base=0x%08X  pos=(%.1f, %.1f, %.1f)", m.base, p[0], p[1], p[2]);
    }
    Logger::Info("CamFinder: type a base into 'Force Cam Base', enable freecam, and verify.");

    s_scanning.store(false);
    return 0;
}

void Scan() {
    if (s_scanning.exchange(true)) {
        Logger::Warn("CamFinder: scan already in progress, please wait...");
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else   s_scanning.store(false);
}

bool IsScanning() { return s_scanning.load(); }

} // namespace CamFinder
