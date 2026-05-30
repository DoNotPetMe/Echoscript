#include "object_dumper.h"
#include "prop_database.h"
#include "../utils/logger.h"
#include "../utils/memory.h"
#include "../utils/pattern_scan.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cstring>

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

// Known UObject layout offsets in Blacklist (UE3 — tune these live).
// These are the standard UE3 UObject fields:
//   +0x00  VTable ptr
//   +0x08  ObjectFlags (uint64 in 64-bit UE3)
//   +0x10  Index (int32 in GObjects)
//   +0x18  UClass* Class     ← the class node pointer we want
//   +0x28  FName Name        ← {Index:int32, Number:int32}
//   +0x30  UObject* Outer
// All of the above are HYPOTHESES for Blacklist; tune live in the menu.
struct UObjectOffsets {
    size_t classPtr = 0x18;   // UClass* Class at this offset within UObject
    size_t nameIdx  = 0x28;   // FName.Index (int32)
};
static UObjectOffsets s_ue3Off{};

namespace ObjectDumper {

Config g_config{};

// Resolved registry walk root (first node), discovered in Init().
static uintptr_t g_registryHead = 0;
// Resolved UE3 GObjects base pointer (TArray<FObjectItem>*), or 0.
static uintptr_t g_gobjectsPtr  = 0;

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

    // Path 1: UE3 GObjects (preferred — standard UE3 structure).
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
size_t DumpAll() {
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
