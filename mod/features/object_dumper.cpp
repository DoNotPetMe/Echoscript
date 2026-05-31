#include "object_dumper.h"
#include "prop_database.h"
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
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// -----------------------------------------------------------------------
//  UE3 GObjects signature stubs — fill these via IDA/x64dbg once confirmed.
//
//  GObjects is a TArray<FObjectItem> (or FUObjectArray in UE4-style builds).
//  The pattern below should resolve to the global GObjects pointer.
//  This IS standard UE3; Blacklist uses Engine.GameEngine and Package.Class
//  naming (see System/Blacklist.ini: GameEngine=Engine.GameEngine).
//
//  ScanGame() returns a RIP-relative resolved address pointing at GObjects.
//  After resolving, GObjects[i].Object is a UObject*; read Object->Class
//  (at UObject+0x??) and Object->Name (FName at UObject+0x??) to find UClass*.
//
//  Recommended bring-up order:
//   1. Find GObjects via signature → confirm it walks recognisable class names.
//   2. Find the "Mesh" or "Actor" UClass* — that becomes the typeNode for spawn.
//   3. Locate UWorld::SpawnActor virtual at a known vtable slot (standard UE3).
// -----------------------------------------------------------------------

// <<< FILL via IDA/x64dbg — see engine_bridge.h for the same pattern style.
static constexpr const char* GOBJECTS_SIG =
    "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??";   // placeholder
static constexpr int GOBJECTS_REL32_OFF   = 3;
static constexpr int GOBJECTS_INSTR_SIZE  = 7;

static constexpr const char* GNAMES_SIG =
    "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??";   // placeholder
static constexpr int GNAMES_REL32_OFF     = 3;
static constexpr int GNAMES_INSTR_SIZE    = 7;

// -----------------------------------------------------------------------
//  UE3 32-bit object model (Blacklist is a 32-bit UE3/LEAD build).
//
//  GObjects is a TArray in UE3: { UObject** Data; int Count; int Max; }
//  Each UObject* has an FName at uobj_name_off (an int32 index into GNames)
//  and a UClass* at uobj_class_off.
//
//  GNames is a TArray of FNameEntry*. An FNameEntry holds flags + index,
//  then the inline ANSI name characters at fname_str_off. GNames[0] is
//  ALWAYS "None" in UE3 — we use that as the validation anchor.
//
//  Offsets live in g_config.layout so they can be tuned in the menu if this
//  particular build deviates from the common UDK layout.
// -----------------------------------------------------------------------

namespace ObjectDumper {

Config g_config{};

// Resolved registry walk root (first node), legacy LEAD path.
static uintptr_t g_registryHead = 0;
// Resolved UE3 GObjects TArray address, or 0.
static uintptr_t g_gobjectsPtr  = 0;
// Resolved UE3 GNames TArray address, or 0.
static uintptr_t g_gnamesPtr    = 0;
// Diagnostic: how many TArray candidates passed the {data/count/max} shape check.
static int g_diagCandidates = 0;

uintptr_t GNamesPtr()   { return g_gnamesPtr; }
uintptr_t GObjectsPtr() { return g_gobjectsPtr; }

void SetGNamesPtr(uintptr_t addr) {
    g_gnamesPtr = addr;
    if (addr) Logger::Info("ObjectDumper: GNames manually set to 0x%08X.", static_cast<unsigned>(addr));
}

void SetGObjectsPtr(uintptr_t addr) {
    g_gobjectsPtr = addr;
    if (addr) {
        g_registryHead = addr;
        Logger::Info("ObjectDumper: GObjects manually set to 0x%08X.", static_cast<unsigned>(addr));
    }
}

// Read an ANSI string from a fixed address into out (bounded).
static bool ReadAnsiAt(uintptr_t addr, std::string& out, int maxLen = 128) {
    char buf[256]{};
    int n = (maxLen < 255) ? maxLen : 255;
    for (int i = 0; i < n; ++i) {
        char c = 0;
        if (!Memory::SafeRead(addr + i, c)) return false;
        if (c == 0) { buf[i] = 0; break; }
        // Reject non-printable: name entries are clean ASCII identifiers.
        if (c < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;
        buf[i] = c;
    }
    if (buf[0] == '\0') return false;
    out = buf;
    return true;
}

// Resolve an FName index to its string via the discovered GNames array.
bool ResolveFName(int index, std::string& out) {
    if (!g_gnamesPtr || index < 0) return false;
    uintptr_t data = 0;   // FNameEntry** Data
    if (!Memory::SafeRead(g_gnamesPtr, data) || !data) return false;
    int count = 0;
    if (!Memory::SafeRead(g_gnamesPtr + sizeof(uintptr_t), count) || index >= count)
        return false;
    uintptr_t entry = 0;  // FNameEntry*
    if (!Memory::SafeRead(data + static_cast<uintptr_t>(index) * sizeof(uintptr_t), entry)
        || !entry)
        return false;
    return ReadAnsiAt(entry + g_config.layout.fname_str_off, out);
}

// Read a UObject*'s name (resolving its FName through GNames).
static bool ReadObjectName(uintptr_t obj, std::string& out) {
    if (!obj) return false;
    int nameIdx = 0;
    if (!Memory::SafeRead(obj + g_config.layout.uobj_name_off, nameIdx)) return false;
    return ResolveFName(nameIdx, out);
}

// -----------------------------------------------------------------------
//  Find the address of a literal ANSI string inside the game module's
//  data sections.  Used as an anchor: the registry node for a type holds
//  a pointer to its name string.
// -----------------------------------------------------------------------
static uintptr_t FindString(const char* str) {
    HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) return 0;

    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    const auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);

    const size_t len = std::strlen(str);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        // Search readable, initialised-data sections (skip code-only).
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)) continue;

        uintptr_t start = reinterpret_cast<uintptr_t>(mod) + sec->VirtualAddress;
        size_t    size  = sec->Misc.VirtualSize;
        if (size < len + 1) continue;

        const char* data = reinterpret_cast<const char*>(start);
        for (size_t off = 0; off + len + 1 <= size; ++off) {
            // Whole-string match, NUL-terminated (avoids matching substrings).
            if (data[off] == str[0] &&
                std::memcmp(data + off, str, len) == 0 &&
                data[off + len] == '\0') {
                return start + off;
            }
        }
    }
    return 0;
}

// -----------------------------------------------------------------------
//  Scan the module image for any pointer whose value equals targetPtr.
//  Returns the address holding that pointer (i.e. a candidate registry
//  node field) or 0.  Used to find the node that references a name string.
// -----------------------------------------------------------------------
static uintptr_t FindPointerTo(uintptr_t targetPtr) {
    HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) return 0;

    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    const auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)) continue;

        uintptr_t start = reinterpret_cast<uintptr_t>(mod) + sec->VirtualAddress;
        size_t    size  = sec->Misc.VirtualSize;

        for (size_t off = 0; off + sizeof(uintptr_t) <= size; off += sizeof(uintptr_t)) {
            uintptr_t val = 0;
            if (Memory::SafeRead(start + off, val) && val == targetPtr)
                return start + off;
        }
    }
    return 0;
}

// -----------------------------------------------------------------------
//  Read a type-node's name string into out.  Honors the wide/ansi flag.
// -----------------------------------------------------------------------
static bool ReadNodeName(uintptr_t node, std::string& out) {
    uintptr_t namePtr = 0;
    if (!Memory::SafeRead(node + g_config.layout.node_name_ptr_off, namePtr) || !namePtr)
        return false;

    char buf[128]{};
    if (g_config.layout.name_is_wide) {
        wchar_t wbuf[128]{};
        for (int i = 0; i < 127; ++i) {
            wchar_t c = 0;
            if (!Memory::SafeRead(namePtr + i * sizeof(wchar_t), c) || c == 0) break;
            wbuf[i] = c;
        }
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf), nullptr, nullptr);
    } else {
        for (int i = 0; i < 127; ++i) {
            char c = 0;
            if (!Memory::SafeRead(namePtr + i, c) || c == 0) break;
            buf[i] = c;
        }
    }
    if (buf[0] == '\0') return false;
    out = buf;
    return true;
}

// -----------------------------------------------------------------------
//  AutoFindGlobals — locate GNames and GObjects without a byte signature.
//
//  Strategy (works on 32-bit UE3 with ASLR):
//   1. Find the literal "None" string in the image (FNameEntry for index 0
//      stores it inline). Then find a writable global (GNames TArray) whose
//      Data[0] points at an FNameEntry whose inline string == "None".
//   2. Validate GNames by resolving a few known names ("None", "Core",
//      "Object", "Actor") to ensure the layout is right.
//   3. Find GObjects: a writable global TArray whose Data[k] are pointers to
//      objects whose name resolves and whose first object is named "Class" or
//      whose names look like a real object table.
//
//  Everything is bounds-checked and __try-guarded via SafeRead, so a wrong
//  guess just fails instead of crashing.
// -----------------------------------------------------------------------
static bool LooksLikeReadablePtr(uintptr_t p) {
    if (p < 0x10000u || p > 0x7FFFFFFFu) return false;
    int probe = 0;
    return Memory::SafeRead(p, probe);
}

// Scan PE sections for a TArray<T*> { Data, Count, Max } that satisfies
// `validate(Data, Count)`. Returns the TArray address or 0.
// requireWrite=true  → only writable sections (.data, .bss) — first pass
// requireWrite=false → all readable sections including .rdata — second pass fallback
static uintptr_t FindTArray(bool (*validate)(uintptr_t data, int count),
                             bool requireWrite = true) {
    HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) return 0;
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    const auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)) continue;
        if (requireWrite && !(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uintptr_t start = reinterpret_cast<uintptr_t>(mod) + sec->VirtualAddress;
        size_t    size  = sec->Misc.VirtualSize;

        for (size_t off = 0; off + 12 <= size; off += 4) {
            uintptr_t cand = start + off;
            uintptr_t data = 0; int count = 0, max = 0;
            if (!Memory::SafeRead(cand, data) || !LooksLikeReadablePtr(data)) continue;
            if (!Memory::SafeRead(cand + 4, count) || count <= 0 || count > 5'000'000) continue;
            if (!Memory::SafeRead(cand + 8, max)   || max < count || max > 5'000'000) continue;
            if (validate(cand, count)) return cand;
        }
    }
    return 0;
}

// GNames validator: check Data[0..4] for an FNameEntry whose inline string == "None".
// Some UE3 builds reserve Data[0] as a null sentinel; "None" may be at index 1.
static bool ValidateGNames(uintptr_t arr, int count) {
    uintptr_t data = 0;
    if (!Memory::SafeRead(arr, data) || !LooksLikeReadablePtr(data)) return false;

    ++g_diagCandidates;

    // Comprehensive FNameEntry inline-string offset candidates.
    static const size_t kStrOffs[] = {
        0x00, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1C, 0x20, 0x28
    };

    // Diagnostic: show the data pointer and the raw D[0..3] pointers + bytes at D[0].
    if (g_diagCandidates <= 8) {
        uintptr_t d[4] = {};
        for (int i = 0; i < 4; ++i) Memory::SafeRead(data + static_cast<uintptr_t>(i) * 4, d[i]);
        Logger::Info("ObjectDumper: GNamesCand#%d arr=0x%08X cnt=%d data=0x%08X "
                     "D[0..3]=[0x%08X 0x%08X 0x%08X 0x%08X]",
                     g_diagCandidates, static_cast<unsigned>(arr), count,
                     static_cast<unsigned>(data),
                     static_cast<unsigned>(d[0]), static_cast<unsigned>(d[1]),
                     static_cast<unsigned>(d[2]), static_cast<unsigned>(d[3]));
        // Dump bytes at each non-null D[i] to help identify the string offset.
        for (int i = 0; i < 4; ++i) {
            if (!LooksLikeReadablePtr(d[i])) continue;
            char byteStr[72] = {};
            int n = 0;
            for (int bi = 0; bi < 16 && n + 4 < (int)sizeof(byteStr); ++bi) {
                uint8_t b = 0; Memory::SafeRead(d[i] + bi, b);
                n += snprintf(byteStr + n, sizeof(byteStr) - n, "%02X ", b);
            }
            Logger::Info("ObjectDumper:   D[%d]=0x%08X bytes=[%s]", i, static_cast<unsigned>(d[i]), byteStr);
        }
    }

    // Try Data[0..4] with all candidate string offsets.
    for (int idx = 0; idx <= 4; ++idx) {
        uintptr_t entryN = 0;
        if (!Memory::SafeRead(data + static_cast<uintptr_t>(idx) * 4, entryN)) break;
        if (!LooksLikeReadablePtr(entryN)) continue;
        for (size_t fo : kStrOffs) {
            std::string s;
            if (ReadAnsiAt(entryN + fo, s, 32) && (s == "None" || s == "none")) {
                g_config.layout.fname_str_off = fo;
                (void)count;
                return true;
            }
        }
    }
    return false;
}

// SEH-guarded scan of a single memory region for "None\0".
// Writes found addresses into out[] (up to outMax); returns the count.
// Kept free of C++ objects so __try is legal (no object unwinding).
static int ScanRegionForNone(const char* p, const char* pEnd,
                             uintptr_t* out, int outMax) {
    int found = 0;
    __try {
        while (p < pEnd && found < outMax) {
            const char* f = static_cast<const char*>(
                std::memchr(p, 'N', static_cast<size_t>(pEnd - p)));
            if (!f) break;
            if (f + 5 <= pEnd &&
                f[1] == 'o' && f[2] == 'n' && f[3] == 'e' && f[4] == '\0')
                out[found++] = reinterpret_cast<uintptr_t>(f);
            p = f + 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

// -----------------------------------------------------------------------
//  FindGNamesViaMemScan — last-resort path that doesn't assume TArray layout.
//
//  Strategy:
//   1. Scan all MEM_PRIVATE committed heap pages with memchr for "None\0".
//   2. Build a sorted reverse-lookup of every writable .data section value
//      that looks like a pointer → its address.
//   3. For each "None" at address X and each candidate string offset S:
//        entryPtr = X - S  (start of the FNameEntry)
//        dataArr  = entryPtr - idx*4  (if entry is at Data[idx])
//      Find dataArr in the reverse map → get a TArray.Data field address.
//      Validate with Count/Max plausibility + name-score.
//
//  This works whether GNames is a TArray<FNameEntry*>, a raw C-array, or
//  any other structure — as long as something in .data holds the Data ptr.
// -----------------------------------------------------------------------
static bool FindGNamesViaMemScan() {
    Logger::Info("ObjectDumper: mem-scan fallback: scanning heap for 'None\\0'...");

    // ---- Phase 1: memchr scan of MEM_PRIVATE heap pages ----
    std::vector<uintptr_t> noneAddrs;
    {
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0x10000u;
        while (noneAddrs.size() < 256 &&
               VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
            uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                mbi.RegionSize <= 128u * 1024u * 1024u) {
                const char* p    = reinterpret_cast<const char*>(base);
                const char* pEnd = p + mbi.RegionSize;
                uintptr_t hits[64];
                int n = ScanRegionForNone(p, pEnd, hits, 64);
                for (int i = 0; i < n && noneAddrs.size() < 256; ++i)
                    noneAddrs.push_back(hits[i]);
            }
            uintptr_t next = base + mbi.RegionSize;
            if (next <= addr) break;
            addr = next;
        }
    }
    Logger::Info("ObjectDumper: heap scan found %zu 'None' occurrence(s).", noneAddrs.size());
    if (noneAddrs.empty()) return false;

    // ---- Phase 2: build sorted reverse-lookup of writable .data values ----
    // Maps (stored 32-bit value → address in .data section where it's stored).
    std::vector<std::pair<uintptr_t, uintptr_t>> revMap;
    revMap.reserve(2'000'000);
    {
        HMODULE mod = GetModuleHandleW(nullptr);
        const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
        const auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD si = 0; si < nt->FileHeader.NumberOfSections; ++si, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            uintptr_t start = reinterpret_cast<uintptr_t>(mod) + sec->VirtualAddress;
            size_t    sz    = sec->Misc.VirtualSize;
            for (size_t off = 0; off + 4 <= sz; off += 4) {
                uint32_t v = 0;
                if (!Memory::SafeRead(start + off, v)) continue;
                if (v >= 0x10000u && v < 0x80000000u)
                    revMap.push_back({ static_cast<uintptr_t>(v), start + off });
            }
        }
    }
    std::sort(revMap.begin(), revMap.end());
    Logger::Info("ObjectDumper: revmap %zu ptr-valued entries.", revMap.size());

    // Find all .data addresses that hold a given value.
    auto LookupAll = [&](uintptr_t target, std::vector<uintptr_t>& out) {
        auto lo = std::lower_bound(revMap.begin(), revMap.end(),
                                    std::make_pair(target, uintptr_t{0}));
        for (auto it = lo; it != revMap.end() && it->first == target; ++it)
            out.push_back(it->second);
    };

    // ---- Phase 3: trace None → FNameEntry → Data[idx] → TArray ----
    static const size_t kStrOffs[] = { 0x00, 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10 };

    for (uintptr_t noneAddr : noneAddrs) {
        for (size_t soff : kStrOffs) {
            if (noneAddr < soff) continue;
            uintptr_t entryPtr = noneAddr - soff;  // expected FNameEntry start

            // Try entry at Data[0], Data[1], Data[2] (handle null sentinel at index 0).
            for (int idx = 0; idx <= 2; ++idx) {
                if (entryPtr < static_cast<uintptr_t>(idx) * 4u) continue;
                uintptr_t dataArr = entryPtr - static_cast<uintptr_t>(idx) * 4u;

                std::vector<uintptr_t> tArrayCands;
                LookupAll(dataArr, tArrayCands);
                for (uintptr_t gnamesCand : tArrayCands) {
                    int cnt = 0, maxv = 0;
                    if (!Memory::SafeRead(gnamesCand + 4, cnt) || cnt < 100 || cnt > 5'000'000) continue;
                    if (!Memory::SafeRead(gnamesCand + 8, maxv) || maxv < cnt || maxv > 5'000'000) continue;

                    // Score: how many of Data[0..7] resolve to valid identifier strings.
                    int score = 0;
                    for (int k = 0; k < 8; ++k) {
                        uintptr_t eN = 0;
                        if (!Memory::SafeRead(dataArr + static_cast<uintptr_t>(k) * 4, eN)) break;
                        if (!LooksLikeReadablePtr(eN)) continue;
                        std::string s;
                        if (ReadAnsiAt(eN + soff, s, 64) && !s.empty()) ++score;
                    }
                    if (score < 3) continue;

                    g_gnamesPtr = gnamesCand;
                    g_config.layout.fname_str_off = soff;
                    Logger::Info("ObjectDumper: GNames @ 0x%08X via mem-scan "
                                 "(str_off=0x%zX cnt=%d score=%d/8 none@0x%08X entry@0x%08X)",
                                 static_cast<unsigned>(gnamesCand), soff, cnt, score,
                                 static_cast<unsigned>(noneAddr),
                                 static_cast<unsigned>(entryPtr));
                    return true;
                }
            }
        }
    }

    Logger::Warn("ObjectDumper: mem-scan traced %zu 'None' sites — GNames TArray not found. "
                 "GNames may use raw C-array or chunk-table storage, not TArray.",
                 noneAddrs.size());
    return false;
}

// -----------------------------------------------------------------------
//  FindGObjectsViaPlayerScan — bootstrap GObjects from a confirmed live
//  UObject pointer (the player/camera struct captured by FreeCam).
//
//  The captured ESI pointer is stored in GObjects.Data[k] for some k.
//  Strategy:
//   1. Scan all committed memory for the exact 4-byte value.
//   2. For each hit H (candidate &Data[k]), try k=1..511:
//      dataPtr = H - k*4 (candidate array start).
//   3. Search writable .data sections for a slot holding dataPtr —
//      that slot is the TArray.Data field of GObjects.
//   4. Validate Count/Max plausibility and that entries near [k] are
//      valid-looking pointers (score >= 5/9).
//
//  No GNames dependency — works independently as the first bootstrap step.
// -----------------------------------------------------------------------
static bool FindGObjectsViaPlayerScan() {
    uintptr_t playerPtr = FreeCam::CurrentBase();
    if (!playerPtr) {
        Logger::Info("ObjectDumper: player-ptr scan skipped (not yet in a level).");
        return false;
    }
    Logger::Info("ObjectDumper: player-ptr GObjects scan (ptr=0x%08X)...",
                 static_cast<unsigned>(playerPtr));

    // Phase 1 — scan committed pages for playerPtr value (4-byte aligned).
    static uintptr_t hits[1024];
    int nHits = 0;
    {
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0x10000u;
        while (nHits < 1024 &&
               VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
            uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            if (mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                mbi.RegionSize <= 128u * 1024u * 1024u) {
                size_t slots = mbi.RegionSize / 4;
                for (size_t s = 0; s < slots && nHits < 1024; ++s) {
                    uintptr_t v = 0;
                    if (Memory::SafeRead(base + s * 4u, v) && v == playerPtr)
                        hits[nHits++] = base + s * 4u;
                }
            }
            uintptr_t next = base + mbi.RegionSize;
            if (next <= addr) break;
            addr = next;
        }
    }
    Logger::Info("ObjectDumper: player-ptr scan: %d memory slot(s) hold 0x%08X.",
                 nHits, static_cast<unsigned>(playerPtr));
    if (!nHits) return false;

    // Phase 2 — build sorted reverse map of writable .data pointer values.
    // Maps stored_value → address_in_.data so we can ask "what .data slot
    // holds pointer X?" in O(log n).
    std::vector<std::pair<uintptr_t, uintptr_t>> revMap;
    revMap.reserve(500000);
    {
        HMODULE mod = GetModuleHandleW(nullptr);
        const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
        const auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD si = 0; si < nt->FileHeader.NumberOfSections; ++si, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            uintptr_t start = reinterpret_cast<uintptr_t>(mod) + sec->VirtualAddress;
            size_t    sz    = sec->Misc.VirtualSize;
            for (size_t off = 0; off + 4 <= sz; off += 4) {
                uint32_t v = 0;
                if (!Memory::SafeRead(start + off, v)) continue;
                if (v >= 0x10000u && v < 0x80000000u)
                    revMap.push_back({ static_cast<uintptr_t>(v), start + off });
            }
        }
    }
    std::sort(revMap.begin(), revMap.end());

    auto LookupAll = [&](uintptr_t target, std::vector<uintptr_t>& out) {
        auto lo = std::lower_bound(revMap.begin(), revMap.end(),
                                   std::make_pair(target, uintptr_t{0}));
        for (auto it = lo; it != revMap.end() && it->first == target; ++it)
            out.push_back(it->second);
    };

    // Phase 3 — trace hits back to GObjects TArray.
    for (int h = 0; h < nHits; ++h) {
        uintptr_t H = hits[h];
        for (int k = 1; k < 512; ++k) {
            if (H < static_cast<uintptr_t>(k) * 4u) break;
            uintptr_t dataPtr = H - static_cast<uintptr_t>(k) * 4u;

            std::vector<uintptr_t> cands;
            LookupAll(dataPtr, cands);
            for (uintptr_t cand : cands) {
                int cnt = 0, maxv = 0;
                if (!Memory::SafeRead(cand + 4, cnt)  || cnt  < 5000 || cnt  > 2'000'000) continue;
                if (!Memory::SafeRead(cand + 8, maxv) || maxv < cnt  || maxv > 2'000'000) continue;
                if (cand == g_gnamesPtr) continue;

                // Validate: entries surrounding playerIdx should be non-null pointers.
                int score = 0;
                for (int dk = -4; dk <= 4; ++dk) {
                    int idx = k + dk;
                    if (idx < 0 || idx >= cnt) continue;
                    uintptr_t obj = 0;
                    if (Memory::SafeRead(dataPtr + static_cast<uintptr_t>(idx) * 4u, obj) &&
                        LooksLikeReadablePtr(obj))
                        ++score;
                }
                if (score < 5) continue;

                g_gobjectsPtr = cand;
                Logger::Info("ObjectDumper: GObjects @ 0x%08X via player-ptr bootstrap "
                             "(playerIdx=%d cnt=%d score=%d/9 data=0x%08X).",
                             static_cast<unsigned>(cand), k, cnt, score,
                             static_cast<unsigned>(dataPtr));
                return true;
            }
        }
    }

    Logger::Warn("ObjectDumper: player-ptr scan exhausted (%d hit slot(s)) — "
                 "no GObjects TArray found. The player struct may not be directly "
                 "in GObjects, or it is at an unusual index (>511). "
                 "Try entering GObjects address manually from CE.",
                 nHits);
    return false;
}

bool ScanGObjectsFromPlayerPtr() {
    if (FindGObjectsViaPlayerScan()) {
        g_registryHead = g_gobjectsPtr;
        Logger::Info("ObjectDumper: ScanGObjectsFromPlayerPtr succeeded — "
                     "run 'Resolve catalog' to populate typeNodes.");
        return true;
    }
    return false;
}

bool AutoFindGlobals() {
    g_gnamesPtr   = 0;
    g_gobjectsPtr = 0;

    // ---- GObjects pass 0: player-ptr bootstrap (requires freecam in-level) ----
    if (FreeCam::HasCapturedBase())
        FindGObjectsViaPlayerScan();

    // ---- GNames ----
    // Pass 1: writable sections only (normal globals live in .data / .bss).
    g_diagCandidates = 0;
    uintptr_t gnames = FindTArray(&ValidateGNames, /*requireWrite=*/true);
    if (!gnames) {
        Logger::Info("ObjectDumper: GNames not in writable sections "
                     "(%d shape-ok candidates). Retrying all readable sections.",
                     g_diagCandidates);
        // Pass 2: also search read-only sections — handles unusual section flags.
        g_diagCandidates = 0;
        gnames = FindTArray(&ValidateGNames, /*requireWrite=*/false);
    }
    if (!gnames) {
        Logger::Info("ObjectDumper: PE section scans exhausted "
                     "(%d total shape-ok candidates). Trying heap mem-scan...",
                     g_diagCandidates);
        // Pass 3: scan heap memory directly for "None\0" and trace back to TArray.
        // Handles cases where GNames is a raw C-array or chunk-table, not a TArray.
        if (FindGNamesViaMemScan()) {
            gnames = g_gnamesPtr;
        }
    }
    if (!gnames) {
        Logger::Warn("ObjectDumper: GNames not auto-found after all passes. "
                     "GNames structure in this build is not yet understood — "
                     "needs manual RE (look for 'FName::Init' or 'GNames' in IDA).");
        // If GObjects was found via player-ptr scan, report partial success.
        if (g_gobjectsPtr) {
            Logger::Info("ObjectDumper: partial success — GObjects located, GNames missing. "
                         "Type names will not resolve until GNames is found.");
            return true;
        }
        return false;
    }
    g_gnamesPtr = gnames;
    Logger::Info("ObjectDumper: GNames @ 0x%08X (fname_str_off=0x%zX).",
                 static_cast<unsigned>(gnames), g_config.layout.fname_str_off);

    // Sanity: a handful of indices should resolve to clean identifiers.
    {
        std::string s0;
        ResolveFName(0, s0);
        Logger::Info("ObjectDumper: GNames[0] = \"%s\" (expect None).", s0.c_str());
    }

    // ---- GObjects ----
    // Skip if already found by player-ptr scan above.
    if (g_gobjectsPtr) {
        Logger::Info("ObjectDumper: GObjects already located (player-ptr path) — "
                     "skipping section scan.");
        return true;
    }

    // Validate by reading Data[0..N] as UObject* and resolving their names; a
    // real object table yields mostly-resolvable names. We also probe a couple
    // of uobj_name_off candidates and lock in the best.
    static const size_t kNameOffs[]  = { 0x2C, 0x28, 0x30, 0x34, 0x38 };
    static const size_t kClassOffs[] = { 0x34, 0x30, 0x38, 0x3C, 0x40 };

    HMODULE mod = GetModuleHandleW(nullptr);
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    const auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);

    uintptr_t bestArr = 0; int bestScore = 0;
    size_t bestNameOff = 0x2C;

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)) continue;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uintptr_t start = reinterpret_cast<uintptr_t>(mod) + sec->VirtualAddress;
        size_t    size  = sec->Misc.VirtualSize;

        for (size_t off = 0; off + 12 <= size; off += 4) {
            uintptr_t cand = start + off;
            uintptr_t data = 0; int count = 0, max = 0;
            if (!Memory::SafeRead(cand, data) || !LooksLikeReadablePtr(data)) continue;
            if (!Memory::SafeRead(cand + 4, count) || count < 1000 || count > 5'000'000) continue;
            if (!Memory::SafeRead(cand + 8, max) || max < count || max > 5'000'000) continue;
            if (cand == g_gnamesPtr) continue;

            // Sample the first 24 object slots; score resolvable names.
            for (size_t no : kNameOffs) {
                int score = 0, sampled = 0;
                for (int k = 0; k < 24 && k < count; ++k) {
                    uintptr_t obj = 0;
                    if (!Memory::SafeRead(data + static_cast<uintptr_t>(k) * 4, obj)) break;
                    if (!LooksLikeReadablePtr(obj)) continue;
                    ++sampled;
                    int nameIdx = 0;
                    if (!Memory::SafeRead(obj + no, nameIdx)) continue;
                    std::string s;
                    if (ResolveFName(nameIdx, s)) ++score;
                }
                if (sampled >= 8 && score > bestScore) {
                    bestScore = score; bestArr = cand; bestNameOff = no;
                }
            }
        }
    }

    if (bestArr && bestScore >= 8) {
        g_gobjectsPtr = bestArr;
        g_config.layout.uobj_name_off = bestNameOff;
        // Pick a class offset that yields a readable pointer on the first object.
        uintptr_t data = 0; Memory::SafeRead(bestArr, data);
        uintptr_t obj0 = 0; Memory::SafeRead(data, obj0);
        for (size_t co : kClassOffs) {
            uintptr_t cls = 0;
            if (Memory::SafeRead(obj0 + co, cls) && LooksLikeReadablePtr(cls)) {
                g_config.layout.uobj_class_off = co; break;
            }
        }
        Logger::Info("ObjectDumper: GObjects @ 0x%08X (count probe ok, "
                     "uobj_name_off=0x%zX, uobj_class_off=0x%zX, score=%d/24).",
                     static_cast<unsigned>(bestArr),
                     g_config.layout.uobj_name_off,
                     g_config.layout.uobj_class_off, bestScore);
        return true;
    }

    Logger::Warn("ObjectDumper: GObjects not auto-found (GNames ok). "
                 "Names resolve but no object table matched -- tune uobj offsets.");
    return false;
}

// -----------------------------------------------------------------------
//  TryScanUE3GObjects — attempt to locate GObjects via signature.
//  Since Blacklist is UE3, this is the preferred path; it gives UClass*
//  pointers directly.
// -----------------------------------------------------------------------
static bool TryScanUE3GObjects() {
    g_gobjectsPtr = 0;

    // GOBJECTS_SIG is a placeholder until the real signature is found via RE.
    // When the placeholder is "?? ?? ..." it will scan but never match real bytes
    // (all-wildcard scan results in no useful hit) — treated as "not found".
    // Fill GOBJECTS_SIG with a real IDA-style pattern to activate this path.
    bool allWild = true;
    {
        const char* p = GOBJECTS_SIG;
        while (*p) {
            if (*p != '?' && *p != ' ') { allWild = false; break; }
            ++p;
        }
    }
    if (allWild) {
        Logger::Info("ObjectDumper: GObjects sig is placeholder — UE3 path skipped. "
                     "Fill GOBJECTS_SIG in object_dumper.cpp after RE.");
        return false;
    }

    uintptr_t hit = PatternScan::ScanGame(GOBJECTS_SIG);
    if (!hit) {
        Logger::Warn("ObjectDumper: GOBJECTS_SIG not found in image.");
        return false;
    }
    g_gobjectsPtr = PatternScan::ResolveRIPSimple(hit, GOBJECTS_REL32_OFF, GOBJECTS_INSTR_SIZE);
    if (!g_gobjectsPtr) {
        Logger::Warn("ObjectDumper: GObjects RIP resolve failed.");
        return false;
    }
    Logger::Info("ObjectDumper: GObjects @ 0x%p (UE3 path active).",
                 reinterpret_cast<void*>(g_gobjectsPtr));
    return true;
}

// -----------------------------------------------------------------------
//  Init — try UE3 GObjects first, fall back to LEAD string-anchor.
// -----------------------------------------------------------------------
bool Init() {
    g_registryHead = 0;
    g_gobjectsPtr  = 0;
    g_gnamesPtr    = 0;

    // Path 0: heuristic auto-finder (no hardcoded signature needed — works on
    // your specific build). This is the preferred route for Blacklist.
    if (AutoFindGlobals()) {
        Logger::Info("ObjectDumper: using auto-found UE3 GObjects/GNames path.");
        return true;
    }

    // Path 1: UE3 GObjects via a hardcoded signature (if one is ever filled).
    if (TryScanUE3GObjects()) {
        g_registryHead = g_gobjectsPtr;  // reuse registryHead as the anchor
        Logger::Info("ObjectDumper: using UE3 GObjects path.");
        return true;
    }

    // Path 2: LEAD string-anchor fallback — scan data sections for the
    // literal "Actor" string that LEAD type-registration code bakes in,
    // then find the registry node that references it.
    uintptr_t actorStr = FindString("Actor");
    if (!actorStr) {
        Logger::Warn("ObjectDumper: 'Actor' string not found in image — "
                     "both UE3 and LEAD anchor paths failed.");
        return false;
    }
    Logger::Info("ObjectDumper: 'Actor' string @ 0x%p (LEAD anchor path).",
                 reinterpret_cast<void*>(actorStr));

    uintptr_t nameField = FindPointerTo(actorStr);
    if (!nameField) {
        Logger::Warn("ObjectDumper: no node references 'Actor' string. "
                     "Adjust layout offsets and retry.");
        return false;
    }

    g_registryHead = nameField - g_config.layout.node_name_ptr_off;
    Logger::Info("ObjectDumper: candidate LEAD registry node @ 0x%p",
                 reinterpret_cast<void*>(g_registryHead));
    Logger::Info("ObjectDumper: verify node layout offsets in menu, "
                 "then Dump to confirm names walk correctly.");
    return true;
}

bool IsRegistryFound() {
    return g_registryHead != 0;
}

bool IsUE3GObjectsActive() {
    return g_gobjectsPtr != 0;
}

// -----------------------------------------------------------------------
//  DumpAll — walk the registry list and log every node's name + ptr.
//  Falls back gracefully if the list link is invalid (logs what it got).
// -----------------------------------------------------------------------
// Walk the live GObjects array, invoking fn(objPtr, name) for each named
// object. Returns the number of objects whose name resolved.
static size_t ForEachObject(const std::function<void(uintptr_t, const std::string&)>& fn) {
    uintptr_t data = 0; int count = 0;
    if (!Memory::SafeRead(g_gobjectsPtr, data) ||
        !Memory::SafeRead(g_gobjectsPtr + sizeof(uintptr_t), count))
        return 0;
    if (count > g_config.maxNodes) count = g_config.maxNodes;

    size_t named = 0;
    for (int k = 0; k < count; ++k) {
        uintptr_t obj = 0;
        if (!Memory::SafeRead(data + static_cast<uintptr_t>(k) * sizeof(uintptr_t), obj) || !obj)
            continue;
        std::string name;
        if (ReadObjectName(obj, name)) { fn(obj, name); ++named; }
    }
    return named;
}

size_t DumpAll() {
    // Preferred: walk the live GObjects array.
    if (g_gobjectsPtr) {
        FILE* f = nullptr;
        if (g_config.dumpToFile) fopen_s(&f, g_config.dumpPath.c_str(), "w");
        size_t count = ForEachObject([&](uintptr_t obj, const std::string& name) {
            Logger::Info("[obj] 0x%08X  %s", static_cast<unsigned>(obj), name.c_str());
            if (f) fprintf(f, "0x%08X\t%s\n", static_cast<unsigned>(obj), name.c_str());
        });
        if (f) fclose(f);
        Logger::Info("ObjectDumper: dumped %zu named objects%s.", count,
                     g_config.dumpToFile ? " (written to dump file)" : "");
        return count;
    }

    // Legacy fallback: linked-list registry walk.
    if (!g_registryHead) {
        Logger::Warn("ObjectDumper: registry not located; run Init first.");
        return 0;
    }

    FILE* f = nullptr;
    if (g_config.dumpToFile)
        fopen_s(&f, g_config.dumpPath.c_str(), "w");

    size_t count = 0;
    uintptr_t node = g_registryHead;
    for (int i = 0; i < g_config.maxNodes && node; ++i) {
        std::string name;
        if (ReadNodeName(node, name)) {
            Logger::Info("[type] 0x%p  %s", reinterpret_cast<void*>(node), name.c_str());
            if (f) fprintf(f, "0x%p\t%s\n", reinterpret_cast<void*>(node), name.c_str());
            ++count;
        }
        uintptr_t next = 0;
        if (!Memory::SafeRead(node + g_config.layout.node_next_ptr_off, next) || next == node)
            break;
        node = next;
    }

    if (f) fclose(f);
    Logger::Info("ObjectDumper: walked %zu nodes%s.", count,
                 g_config.dumpToFile ? " (written to dump file)" : "");
    return count;
}

// -----------------------------------------------------------------------
//  ResolveProps — walk the registry and, for every name that matches a
//  catalog entry, record its live node pointer in PropDatabase.
// -----------------------------------------------------------------------
size_t ResolveProps() {
    // Preferred: match catalog names against live GObjects. For each catalog
    // type name we want the UClass* (an object literally named the same in the
    // 'Class' table), so we record the object whose own name matches.
    if (g_gobjectsPtr) {
        size_t resolved = 0;
        ForEachObject([&](uintptr_t obj, const std::string& name) {
            uint32_t id = PropDatabase::ResolveByName(name);
            if (id != UINT32_MAX) { PropDatabase::SetTypeNode(id, obj); ++resolved; }
        });
        Logger::Info("ObjectDumper: resolved %zu / %zu catalog types via GObjects.",
                     resolved, PropDatabase::Entries().size());
        return resolved;
    }

    if (!g_registryHead) return 0;

    size_t resolved = 0;
    uintptr_t node = g_registryHead;
    for (int i = 0; i < g_config.maxNodes && node; ++i) {
        std::string name;
        if (ReadNodeName(node, name)) {
            uint32_t id = PropDatabase::ResolveByName(name);
            if (id != UINT32_MAX) {
                PropDatabase::SetTypeNode(id, node);
                ++resolved;
            }
        }
        uintptr_t next = 0;
        if (!Memory::SafeRead(node + g_config.layout.node_next_ptr_off, next) || next == node)
            break;
        node = next;
    }

    Logger::Info("ObjectDumper: resolved %zu / %zu catalog types to live nodes.",
                 resolved, PropDatabase::Entries().size());
    return resolved;
}

} // namespace ObjectDumper
