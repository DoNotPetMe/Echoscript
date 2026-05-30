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

namespace CamFinder {

static std::atomic<bool> s_scanning{ false };

// -----------------------------------------------------------------------
//  Matrix validation
// -----------------------------------------------------------------------

static float Dot3(float ax, float ay, float az,
                  float bx, float by, float bz) {
    return ax*bx + ay*by + az*bz;
}
static float Len2(float x, float y, float z) { return x*x + y*y + z*z; }

// Returns true if the 16-float block looks like a valid 4x4 rotation matrix
// (row-major OR column-major — we check both because we don't know the game's
// convention yet).  Tolerance is loose (2 %) to account for floating-point
// accumulation in animated matrices.
static bool IsViewMatrix(const float* m) {
    // Every element in the rotation part must be finite and in [-1, 1]
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float v = m[r * 4 + c];
            if (!std::isfinite(v) || fabsf(v) > 1.001f) return false;
        }

    // ---- row-major: rows 0,1,2 are the three basis vectors ----
    {
        float ax=m[0], ay=m[1], az=m[2];
        float bx=m[4], by=m[5], bz=m[6];
        float cx=m[8], cy=m[9], cz=m[10];
        if (fabsf(Len2(ax,ay,az)-1.f) < 0.02f &&
            fabsf(Len2(bx,by,bz)-1.f) < 0.02f &&
            fabsf(Len2(cx,cy,cz)-1.f) < 0.02f &&
            fabsf(Dot3(ax,ay,az,bx,by,bz))   < 0.02f &&
            fabsf(Dot3(ax,ay,az,cx,cy,cz))   < 0.02f &&
            fabsf(Dot3(bx,by,bz,cx,cy,cz))   < 0.02f)
            return true;
    }

    // ---- column-major: columns 0,1,2 are the three basis vectors ----
    {
        float ax=m[0], ay=m[4], az=m[8];
        float bx=m[1], by=m[5], bz=m[9];
        float cx=m[2], cy=m[6], cz=m[10];
        if (fabsf(Len2(ax,ay,az)-1.f) < 0.02f &&
            fabsf(Len2(bx,by,bz)-1.f) < 0.02f &&
            fabsf(Len2(cx,cy,cz)-1.f) < 0.02f &&
            fabsf(Dot3(ax,ay,az,bx,by,bz))   < 0.02f &&
            fabsf(Dot3(ax,ay,az,cx,cy,cz))   < 0.02f &&
            fabsf(Dot3(bx,by,bz,cx,cy,cz))   < 0.02f)
            return true;
    }

    return false;
}

// -----------------------------------------------------------------------
//  Background scan thread
// -----------------------------------------------------------------------

static DWORD WINAPI ScanThread(LPVOID) {
    Logger::Info("CamFinder: walking memory — this takes a few seconds...");

    struct Hit { uintptr_t base; float pos[3]; };
    std::vector<Hit> hits;
    hits.reserve(256);

    uintptr_t addr = 0x10000u;

    while (addr < 0xFFF00000u) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
            break;

        bool readable =
            mbi.State   == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD) &&
            mbi.RegionSize >= 80;

        if (readable) {
            // Bulk-read the region into a local buffer for cache-friendly scanning
            std::vector<uint8_t> buf(mbi.RegionSize);
            SIZE_T got = 0;
            if (ReadProcessMemory(GetCurrentProcess(),
                                  mbi.BaseAddress, buf.data(),
                                  mbi.RegionSize, &got) && got >= 80) {

                // Scan at 16-byte alignment (view matrices are always aligned)
                for (size_t i = 0; i + 80 <= got; i += 16) {
                    const float* m = reinterpret_cast<const float*>(buf.data() + i);
                    if (!IsViewMatrix(m)) continue;

                    // Read potential position at offset +0x44 into the struct
                    if (i + 0x44 + 12 > got) continue;
                    const float* p = reinterpret_cast<const float*>(buf.data() + i + 0x44);

                    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
                        continue;

                    float mag2 = p[0]*p[0] + p[1]*p[1] + p[2]*p[2];
                    // Skip all-zero (uninitialised) and astronomically large positions
                    if (mag2 < 1.f || mag2 > 1e12f) continue;

                    Hit h;
                    h.base   = addr + i;
                    h.pos[0] = p[0];
                    h.pos[1] = p[1];
                    h.pos[2] = p[2];
                    hits.push_back(h);
                }
            }
        }

        addr += mbi.RegionSize;
        if (addr == 0u) break; // 32-bit wrap-around guard
    }

    // ---- Report ----
    Logger::Info("CamFinder: done — %zu candidate(s):", hits.size());
    Logger::Info("  (look at pos X/Y/Z and find the one that matches where you are standing)");
    for (auto& h : hits)
        Logger::Info("  base=0x%08X  pos=(%.1f, %.1f, %.1f)",
                     h.base, h.pos[0], h.pos[1], h.pos[2]);

    Logger::Info("CamFinder: paste a base address into the 'Force Cam Base' box in the menu.");
    Logger::Info("  Move around — if freecam position tracks you, that is the camera struct.");

    s_scanning.store(false);
    return 0;
}

// -----------------------------------------------------------------------
//  Public API
// -----------------------------------------------------------------------

void Scan() {
    if (s_scanning.exchange(true)) {
        Logger::Warn("CamFinder: scan already in progress, please wait...");
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else   s_scanning.store(false);
}

bool IsScanning() {
    return s_scanning.load();
}

} // namespace CamFinder
