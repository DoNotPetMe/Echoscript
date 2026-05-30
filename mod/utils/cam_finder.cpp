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

// Safe read of three floats (position) at an in-process address.
static bool ReadPos(uintptr_t addr, float out[3]) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<LPCVOID>(addr),
                             out, sizeof(float) * 3, &got) && got == sizeof(float) * 3;
}

static bool StillMatrix(uintptr_t base) {
    float buf[16];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(base),
                           buf, sizeof(buf), &got) || got != sizeof(buf))
        return false;
    return IsViewMatrix(buf);
}

struct Cand { uintptr_t base; float pos[3]; };

static DWORD WINAPI ScanThread(LPVOID) {
    Logger::Info("CamFinder: phase 1 -- snapshotting candidate matrices...");

    std::vector<Cand> cands;
    cands.reserve(8192);

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
                    c.pos[0] = p[0]; c.pos[1] = p[1]; c.pos[2] = p[2];
                    cands.push_back(c);
                }
            }
        }
        addr += mbi.RegionSize;
        if (addr == 0u) break;
    }

    Logger::Info("CamFinder: %zu candidates captured.", cands.size());
    Logger::Info("CamFinder: ***WALK in a straight line now*** (4 seconds)...");
    for (int s = 4; s > 0; --s) {
        Logger::Info("   ...%d", s);
        Sleep(1000);
    }

    // ---- Phase 2: keep only candidates that MOVED a player-like distance ----
    // A walking player moves clearly but not teleport-far in 4 s.
    constexpr float kMinMove = 8.0f;      // must move at least this far
    constexpr float kMaxMove = 8000.0f;   // but not absurdly far (cuts garbage)

    std::vector<Cand> movers;
    for (auto& c : cands) {
        if (!StillMatrix(c.base)) continue;     // matrix vanished/changed shape -> not camera
        float now[3];
        if (!ReadPos(c.base + 0x44, now)) continue;
        float dx = now[0]-c.pos[0], dy = now[1]-c.pos[1], dz = now[2]-c.pos[2];
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dist < kMinMove || dist > kMaxMove) continue;
        Cand m; m.base = c.base; m.pos[0]=now[0]; m.pos[1]=now[1]; m.pos[2]=now[2];
        movers.push_back(m);
    }

    Logger::Info("CamFinder: done -- %zu candidate(s) MOVED while you walked:", movers.size());
    if (movers.empty()) {
        Logger::Warn("  None moved. Make sure you actually walked during the countdown,");
        Logger::Warn("  then run the scan again. (Walking a long, straight path helps.)");
    }
    for (auto& m : movers)
        Logger::Info("  base=0x%08X  pos=(%.1f, %.1f, %.1f)",
                     m.base, m.pos[0], m.pos[1], m.pos[2]);
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
