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

    if (g_config.layout.gnamesIsChunked) {
        // Chunked layout: g_gnamesPtr → { NumElements(4), NumChunks(4), Chunks[0..127](4 each) }
        // Chunks[chunkIdx][entryIdx] = FNameEntry*
        static constexpr int kChunkSize = 16384;
        int chunkIdx = index / kChunkSize;
        int entryIdx = index % kChunkSize;
        uintptr_t chunkPtr = 0;
        if (!Memory::SafeRead(g_gnamesPtr + 8u + static_cast<uintptr_t>(chunkIdx) * 4u, chunkPtr)
            || !chunkPtr)
            return false;
        uintptr_t entry = 0;
        if (!Memory::SafeRead(chunkPtr + static_cast<uintptr_t>(entryIdx) * 4u, entry) || !entry)
            return false;
        return ReadAnsiAt(entry + g_config.layout.fname_str_off, out);
    }

    // Flat TArray<FNameEntry*> layout.
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

    // ---- Flat check: Data[0..4] are FNameEntry* with "None" at some offset ----
    for (int idx = 0; idx <= 4; ++idx) {
        uintptr_t entryN = 0;
        if (!Memory::SafeRead(data + static_cast<uintptr_t>(idx) * 4, entryN)) break;
        if (!LooksLikeReadablePtr(entryN)) continue;
        for (size_t fo : kStrOffs) {
            std::string s;
            if (ReadAnsiAt(entryN + fo, s, 32) && (s == "None" || s == "none")) {
                g_config.layout.fname_str_off   = fo;
                g_config.layout.gnamesIsChunked = false;
                (void)count;
                return true;
            }
        }
    }

    // ---- Chunked check: data is Chunks[0] (first chunk), Chunks[0][0] = FNameEntry* ----
    // Applies when the sliding window landed on G+8 inside a chunked GNames struct
    // (so `data` is actually Chunk0, not a flat FNameEntry** array).
    {
        uintptr_t e0 = 0;
        if (Memory::SafeRead(data, e0) && LooksLikeReadablePtr(e0)) {
            for (size_t fo : kStrOffs) {
                std::string s;
                if (ReadAnsiAt(e0 + fo, s, 32) && (s == "None" || s == "none")) {
                    // Verify G = arr-8 has plausible NumElements/NumChunks.
                    if (arr >= 8u) {
                        int numElem = 0, numC = 0;
                        if (Memory::SafeRead(arr - 8u, numElem) && numElem >= 100 && numElem <= 2'000'000 &&
                            Memory::SafeRead(arr - 4u, numC)    && numC    >= 1   && numC    <= 128) {
                            g_config.layout.fname_str_off   = fo;
                            g_config.layout.gnamesIsChunked = true;
                            (void)count;
                            return true;
                        }
                    }
                }
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

// Generic SEH-guarded scan for a NUL-terminated ANSI string in a memory region.
// Kept C-style (no unwinding objects) so __try is legal.
static int ScanRegionForString(const char* p, const char* pEnd,
                               const char* needle, int needleLen,
                               uintptr_t* out, int outMax) {
    int found = 0;
    __try {
        while (p < pEnd && found < outMax) {
            const char* f = static_cast<const char*>(
                std::memchr(p, needle[0], static_cast<size_t>(pEnd - p)));
            if (!f) break;
            if (f + needleLen + 1 <= pEnd &&
                std::memcmp(f, needle, static_cast<size_t>(needleLen)) == 0 &&
                f[needleLen] == '\0')
                out[found++] = reinterpret_cast<uintptr_t>(f);
            p = f + 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}
// -----------------------------------------------------------------------
//  FindGNamesViaDoubleHop v3 — three root-causes addressed vs v2:
//
//  FIX 1 — "None\0" 256 cap: "None\0" appears >256 times in heap (FStrings,
//    config, object properties), so the real FNameEntry may be hit #300+.
//    Use "ByteProperty\0" (GNames[1]) as primary anchor — essentially unique
//    in the process (1–3 hits). When found, H1 = &Data[1], H0 = H1 − 4.
//    Fall back to "None\0" with a 4096 cap if ByteProperty is absent.
//
//  FIX 2 — EXE-only revMap: Phase 2 used GetModuleHandleW(nullptr) sections
//    only.  GNames TArray may live in a DLL (Engine.dll, Core.dll etc.).
//    Use VirtualQuery with mbi.Type == MEM_IMAGE to cover every loaded DLL.
//
//  FIX 3 — value floor too high: revMap dropped values < 0x100000.  GNames.Data
//    may be an early heap allocation below 1 MB.  Floor lowered to 0x10000.
//
//  Phase 4 drops the mandatory count/max TArray shape check — "None" + score
//  is a stronger discriminator, and it also handles raw-pointer GNames.
// -----------------------------------------------------------------------
static bool FindGNamesViaDoubleHop() {
    Logger::Info("ObjectDumper: GNames double-hop v3 (ByteProperty anchor, all-DLL revMap)...");

    // ---- Phase 1: prefer "ByteProperty\0" (GNames[1]), fall back to "None\0" ----
    static const char kBP[]   = "ByteProperty";
    static const char kNone[] = "None";

    const char* anchorStr = kBP;
    int         anchorLen = 12;
    int         anchorIdx = 1;   // GNames index this anchor corresponds to
    int         anchorCap = 32;

    std::vector<uintptr_t> anchorAddrs;
    for (int pass = 0; pass < 2; ++pass) {
        anchorAddrs.clear();
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0x10000u;
        while ((int)anchorAddrs.size() < anchorCap &&
               VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
            uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                mbi.RegionSize <= 128u * 1024u * 1024u) {
                const char* p    = reinterpret_cast<const char*>(base);
                const char* pEnd = p + mbi.RegionSize;
                uintptr_t hits[128];
                int n = ScanRegionForString(p, pEnd, anchorStr, anchorLen, hits, 128);
                for (int i = 0; i < n && (int)anchorAddrs.size() < anchorCap; ++i)
                    anchorAddrs.push_back(hits[i]);
            }
            uintptr_t next = base + mbi.RegionSize;
            if (next <= addr) break;
            addr = next;
        }
        if (!anchorAddrs.empty()) break;
        // Retry with "None\0" and a higher cap.
        anchorStr = kNone; anchorLen = 4; anchorIdx = 0; anchorCap = 4096;
        Logger::Info("ObjectDumper: 'ByteProperty' absent in MEM_PRIVATE heap; retrying with 'None' cap=4096.");
    }
    Logger::Info("ObjectDumper: phase-1: %zu '%s' occurrence(s) (gnames_idx=%d).",
                 anchorAddrs.size(), anchorStr, anchorIdx);
    if (anchorAddrs.empty()) return false;

    // Compute candidate FNameEntry* for each anchor address.
    // Try header sizes 0, 4, 6, 8, 10, 12, 16 bytes — covers all known UE3 variants.
    static const uintptr_t kHdrSizes[] = { 4u, 8u, 0u, 6u, 10u, 12u, 16u };
    std::vector<uintptr_t> entryPtrs;
    entryPtrs.reserve(anchorAddrs.size() * 7);
    for (uintptr_t aa : anchorAddrs)
        for (uintptr_t h : kHdrSizes)
            if (aa >= h) entryPtrs.push_back(aa - h);
    std::sort(entryPtrs.begin(), entryPtrs.end());
    entryPtrs.erase(std::unique(entryPtrs.begin(), entryPtrs.end()), entryPtrs.end());

    // ---- Phase 2: revMap from ALL writable MEM_IMAGE pages (EXE + every DLL) ----
    // Value floor 0x10000 (was 0x100000) catches early-heap GNames.Data addresses.
    std::vector<std::pair<uintptr_t, uintptr_t>> revMap;
    revMap.reserve(2'000'000);
    {
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0x10000u;
        while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
            uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
                mbi.RegionSize <= 64u * 1024u * 1024u) {
                size_t slots = mbi.RegionSize / 4;
                for (size_t s = 0; s < slots; ++s) {
                    uint32_t v = 0;
                    if (!Memory::SafeRead(base + s * 4u, v)) continue;
                    if (v >= 0x10000u && v < 0x80000000u)
                        revMap.push_back({ static_cast<uintptr_t>(v), base + s * 4u });
                }
            }
            uintptr_t next = base + mbi.RegionSize;
            if (next <= addr) break;
            addr = next;
        }
    }
    std::sort(revMap.begin(), revMap.end());
    Logger::Info("ObjectDumper: phase-2 revMap: %zu entries (all DLL+EXE writable sections).",
                 revMap.size());

    auto LookupAll = [&](uintptr_t target, std::vector<uintptr_t>& out) {
        auto lo = std::lower_bound(revMap.begin(), revMap.end(),
                                   std::make_pair(target, uintptr_t{0}));
        for (auto it = lo; it != revMap.end() && it->first == target; ++it)
            out.push_back(it->second);
    };

    // ---- Phase 3: scan committed pages for entryPtr values → Hi candidate ----
    // anchorIdx==1 (ByteProperty): Hi = &Data[1], H0 = Hi − 4 = &Data[0].
    // anchorIdx==0 (None):         Hi = &Data[0] = H0.
    std::vector<uintptr_t> h0Candidates;
    h0Candidates.reserve(4096);
    {
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0x10000u;
        while (h0Candidates.size() < 16384u &&
               VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
            uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                mbi.RegionSize <= 128u * 1024u * 1024u) {
                size_t slots = mbi.RegionSize / 4;
                for (size_t s = 0; s < slots && h0Candidates.size() < 16384u; ++s) {
                    uint32_t v = 0;
                    if (!Memory::SafeRead(base + s * 4u, v)) continue;
                    auto it = std::lower_bound(entryPtrs.begin(), entryPtrs.end(),
                                               static_cast<uintptr_t>(v));
                    if (it != entryPtrs.end() && *it == v) {
                        uintptr_t Hi = base + s * 4u;
                        uintptr_t H0 = (anchorIdx == 1 && Hi >= 4u) ? Hi - 4u : Hi;
                        h0Candidates.push_back(H0);
                    }
                }
            }
            uintptr_t next = base + mbi.RegionSize;
            if (next <= addr) break;
            addr = next;
        }
        std::sort(h0Candidates.begin(), h0Candidates.end());
        h0Candidates.erase(std::unique(h0Candidates.begin(), h0Candidates.end()),
                           h0Candidates.end());
    }
    Logger::Info("ObjectDumper: phase-3: %zu H0 candidates (anchor='%s').",
                 h0Candidates.size(), anchorStr);
    if (h0Candidates.empty()) return false;

    // ---- Phase 4: for each H0, look in revMap for .data slot storing H0. ----
    // Validate with "None" + 8-entry score; no mandatory count/max TArray check.
    // Three layout cases: flat TArray, chunked struct, raw pointer (no wrapper).
    static const size_t kStrOffsCheck[] = { 0x04, 0x08, 0x00, 0x06, 0x0A, 0x0C, 0x10 };

    for (uintptr_t H0 : h0Candidates) {
        std::vector<uintptr_t> cands;
        LookupAll(H0, cands);
        for (uintptr_t cand : cands) {

            // Read H0[0] = FNameEntry* for GNames[0].
            uintptr_t e0 = 0;
            if (!Memory::SafeRead(H0, e0) || !LooksLikeReadablePtr(e0)) continue;
            size_t goodSoff = SIZE_MAX;
            for (size_t soff : kStrOffsCheck) {
                std::string s;
                if (ReadAnsiAt(e0 + soff, s, 32) && (s == "None" || s == "none"))
                    { goodSoff = soff; break; }
            }
            if (goodSoff == SIZE_MAX) continue;

            // Score first 8 entries of H0 as FNameEntry*'s.
            int score = 0;
            for (int k = 0; k < 8; ++k) {
                uintptr_t eN = 0;
                if (!Memory::SafeRead(H0 + static_cast<uintptr_t>(k) * 4u, eN)) break;
                if (!LooksLikeReadablePtr(eN)) continue;
                std::string s;
                if (ReadAnsiAt(eN + goodSoff, s, 64) && !s.empty()) ++score;
            }
            if (score < 4) continue;

            // Identify flat / chunked / raw.
            bool     isFlat    = false;
            bool     isChunked = false;
            uintptr_t gnamesBase = cand;
            int cnt = 0, maxv = 0;
            if (Memory::SafeRead(cand + 4, cnt) && cnt >= 100 && cnt <= 5'000'000 &&
                Memory::SafeRead(cand + 8, maxv) && maxv >= cnt && maxv <= 5'000'000)
                isFlat = true;
            if (!isFlat && cand >= 8u) {
                int numElem = 0, numC = 0;
                if (Memory::SafeRead(cand - 8u, numElem) && numElem >= 100 && numElem <= 2'000'000 &&
                    Memory::SafeRead(cand - 4u, numC)    && numC    >= 1   && numC    <= 128) {
                    isChunked = true; gnamesBase = cand - 8u;
                }
            }

            g_gnamesPtr = gnamesBase;
            g_config.layout.fname_str_off   = goodSoff;
            g_config.layout.gnamesIsChunked = isChunked;
            Logger::Info("ObjectDumper: GNames @ 0x%08X via double-hop %s "
                         "(anchor='%s' str_off=0x%zX score=%d/8 H0=0x%08X e0=0x%08X)",
                         static_cast<unsigned>(gnamesBase),
                         isChunked ? "CHUNKED" : (isFlat ? "FLAT" : "RAW-PTR"),
                         anchorStr, goodSoff, score,
                         static_cast<unsigned>(H0), static_cast<unsigned>(e0));
            return true;
        }
    }

    Logger::Warn("ObjectDumper: double-hop v3: %zu H0 candidates, none validated. "
                 "anchor='%s' revMap=%zu entries — GNames may be in a non-standard region.",
                 h0Candidates.size(), anchorStr, revMap.size());
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

    // Phase 2 — sort hits, then sweep .data TArray candidates.
    // For each TArray { dataV, cnt, max } in writable sections, check if any
    // hit H falls in [dataV, dataV + cnt*4). This eliminates the k-index limit
    // entirely: the player pawn can be at any index without bound.
    std::sort(hits, hits + nHits);

    {
        HMODULE mod = GetModuleHandleW(nullptr);
        const auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
        const auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
        const auto* sec2 = IMAGE_FIRST_SECTION(nt);

        for (WORD si = 0; si < nt->FileHeader.NumberOfSections; ++si, ++sec2) {
            if (!(sec2->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            uintptr_t start = reinterpret_cast<uintptr_t>(mod) + sec2->VirtualAddress;
            size_t    sz    = sec2->Misc.VirtualSize;

            for (size_t off = 0; off + 12 <= sz; off += 4) {
                uintptr_t cand  = start + off;
                uintptr_t dataV = 0; int cnt = 0, maxv = 0;
                if (!Memory::SafeRead(cand, dataV) || !LooksLikeReadablePtr(dataV)) continue;
                if (!Memory::SafeRead(cand + 4, cnt)  || cnt  < 5000 || cnt  > 2'000'000) continue;
                if (!Memory::SafeRead(cand + 8, maxv) || maxv < cnt  || maxv > 2'000'000) continue;
                if (cand == g_gnamesPtr) continue;

                // Binary search: is any hit H in [dataV, dataV + cnt*4)?
                uintptr_t rangeEnd = dataV + static_cast<uintptr_t>(cnt) * 4u;
                auto it = std::lower_bound(hits, hits + nHits, dataV);
                if (it == hits + nHits || *it >= rangeEnd) continue;

                int k = static_cast<int>((*it - dataV) / 4u);

                // Validate neighbours around the player entry.
                int score = 0;
                for (int dk = -4; dk <= 4; ++dk) {
                    int idx = k + dk;
                    if (idx < 0 || idx >= cnt) continue;
                    uintptr_t obj = 0;
                    if (Memory::SafeRead(dataV + static_cast<uintptr_t>(idx) * 4u, obj) &&
                        LooksLikeReadablePtr(obj))
                        ++score;
                }
                if (score < 5) continue;

                g_gobjectsPtr = cand;
                Logger::Info("ObjectDumper: GObjects @ 0x%08X via player-ptr bootstrap "
                             "(playerIdx=%d cnt=%d score=%d/9 data=0x%08X).",
                             static_cast<unsigned>(cand), k, cnt, score,
                             static_cast<unsigned>(dataV));
                return true;
            }
        }
    }

    Logger::Warn("ObjectDumper: player-ptr scan exhausted (%d hit slot(s)) — "
                 "no GObjects TArray found. The player struct may not be in GObjects, "
                 "or GObjects uses a non-TArray layout. Try entering GObjects address "
                 "manually from CE.",
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

// Probe a GObjects TArray candidate: sample object slots and, for each
// candidate uobj_name_off, count how many resolve to a clean name via the
// (already-verified) GNames. Returns best score and writes the offset.
// Requires g_gnamesPtr to be valid.
static int ProbeGObjectsNameOff(uintptr_t arr, size_t& outNameOff) {
    static const size_t kNameOffs[] = { 0x2C, 0x28, 0x30, 0x34, 0x38, 0x24, 0x40, 0x44, 0x48 };
    uintptr_t data = 0; int count = 0;
    if (!Memory::SafeRead(arr, data) || !LooksLikeReadablePtr(data)) return 0;
    if (!Memory::SafeRead(arr + 4, count) || count <= 0) return 0;

    int bestScore = 0; size_t bestOff = outNameOff;
    for (size_t no : kNameOffs) {
        int score = 0, sampled = 0;
        for (int k = 0; k < 64 && k < count; ++k) {
            uintptr_t obj = 0;
            if (!Memory::SafeRead(data + static_cast<uintptr_t>(k) * 4, obj)) break;
            if (!LooksLikeReadablePtr(obj)) continue;
            ++sampled;
            int nameIdx = 0;
            if (!Memory::SafeRead(obj + no, nameIdx)) continue;
            std::string s;
            if (ResolveFName(nameIdx, s) && !s.empty()) ++score;
        }
        if (sampled >= 8 && score > bestScore) { bestScore = score; bestOff = no; }
    }
    outNameOff = bestOff;
    return bestScore;
}

bool AutoFindGlobals() {
    g_gnamesPtr   = 0;
    g_gobjectsPtr = 0;

    // ---- GObjects pass 0: player-ptr bootstrap (requires freecam in-level) ----
    if (FreeCam::HasCapturedBase())
        FindGObjectsViaPlayerScan();

    // ---- GNames ----
    // VerifyGNames: accept a candidate ONLY if GNames[0] actually resolves to
    // "None" (UE3 invariant). This rejects the flat-scan false positives that
    // plagued earlier builds (random .data slots whose shape looks like a TArray
    // but whose Data[0] is garbage — they reported GNames[0]="").
    auto VerifyGNames = [&](uintptr_t cand) -> bool {
        if (!cand) return false;
        uintptr_t saved = g_gnamesPtr;
        g_gnamesPtr = cand;
        std::string s0, s1;
        bool ok0 = ResolveFName(0, s0) && (s0 == "None" || s0 == "none");
        // Index 1 should also be a clean non-empty identifier in a real table.
        bool ok1 = ResolveFName(1, s1) && !s1.empty();
        if (!(ok0 && ok1)) { g_gnamesPtr = saved; return false; }
        return true;
    };

    // Pass 1: double-hop heap scan FIRST — it anchors on a real "None\0" byte
    // sequence in the heap and walks back to the global, so it does not suffer
    // the false positives of the blind .data sliding-window scan.
    uintptr_t gnames = 0;
    if (FindGNamesViaDoubleHop() && VerifyGNames(g_gnamesPtr)) {
        gnames = g_gnamesPtr;
    } else {
        g_gnamesPtr = 0;
        g_config.layout.gnamesIsChunked = false;
    }

    // Pass 2/3: fall back to the PE-section TArray sliding-window scan, but only
    // accept a candidate that passes VerifyGNames (GNames[0]=="None").
    if (!gnames) {
        Logger::Info("ObjectDumper: double-hop did not yield a verified GNames; "
                     "trying PE-section TArray scan as fallback.");
        g_diagCandidates = 0;
        uintptr_t cand = FindTArray(&ValidateGNames, /*requireWrite=*/true);
        if (!cand) {
            g_diagCandidates = 0;
            cand = FindTArray(&ValidateGNames, /*requireWrite=*/false);
        }
        if (cand && g_config.layout.gnamesIsChunked && cand >= 8u)
            cand -= 8u;
        if (VerifyGNames(cand)) {
            gnames = cand;
        } else if (cand) {
            Logger::Warn("ObjectDumper: flat-scan candidate 0x%08X REJECTED "
                         "(GNames[0] did not resolve to \"None\" — false positive).",
                         static_cast<unsigned>(cand));
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
    {
        std::string s0;
        ResolveFName(0, s0);
        Logger::Info("ObjectDumper: GNames @ 0x%08X VERIFIED (fname_str_off=0x%zX, "
                     "chunked=%d, GNames[0]=\"%s\").",
                     static_cast<unsigned>(gnames), g_config.layout.fname_str_off,
                     g_config.layout.gnamesIsChunked ? 1 : 0, s0.c_str());
    }

    // ---- GObjects ----
    // If the player-ptr scan already found a candidate, VERIFY it now against
    // the verified GNames by probing for a uobj_name_off that resolves names.
    // (The player-ptr scan only checked "neighbours are readable pointers", so
    // the candidate could be a large non-GObjects buffer that happens to hold
    // the player pointer. Name resolution is the real test.)
    if (g_gobjectsPtr) {
        size_t nameOff = g_config.layout.uobj_name_off;
        int score = ProbeGObjectsNameOff(g_gobjectsPtr, nameOff);
        if (score >= 8) {
            g_config.layout.uobj_name_off = nameOff;
            // Lock a class offset that yields a readable pointer on object 0.
            uintptr_t data = 0; Memory::SafeRead(g_gobjectsPtr, data);
            uintptr_t obj0 = 0; Memory::SafeRead(data, obj0);
            for (size_t co : { 0x34u, 0x30u, 0x38u, 0x3Cu, 0x40u, 0x44u }) {
                uintptr_t cls = 0;
                if (Memory::SafeRead(obj0 + co, cls) && LooksLikeReadablePtr(cls)) {
                    g_config.layout.uobj_class_off = co; break;
                }
            }
            Logger::Info("ObjectDumper: GObjects @ 0x%08X (player-ptr path) VERIFIED via "
                         "GNames (uobj_name_off=0x%zX score=%d/64).",
                         static_cast<unsigned>(g_gobjectsPtr),
                         g_config.layout.uobj_name_off, score);
            return true;
        }
        Logger::Warn("ObjectDumper: player-ptr GObjects 0x%08X did NOT verify against "
                     "GNames (best name score=%d) — discarding and running section scan.",
                     static_cast<unsigned>(g_gobjectsPtr), score);
        g_gobjectsPtr = 0;
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
