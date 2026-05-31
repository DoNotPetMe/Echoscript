#include "engine_bridge.h"
#include "object_dumper.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace EngineBridge {

Bridge g_bridge{};

static uintptr_t s_manualWorldPtr   = 0;
static uintptr_t s_manualSpawnFn    = 0;
static uintptr_t s_manualSetTransFn = 0;
static uintptr_t s_manualDestroyFn  = 0;
static uintptr_t s_gworldGlobalAddr = 0;  // address OF the GWorld pointer in .data

void SetManualWorldPtr(uintptr_t addr)       { s_manualWorldPtr   = addr; }
void SetManualSpawnFn(uintptr_t addr)        { s_manualSpawnFn    = addr; }
void SetManualSetTransformFn(uintptr_t addr) { s_manualSetTransFn = addr; }
void SetManualDestroyFn(uintptr_t addr)      { s_manualDestroyFn  = addr; }

uintptr_t ManualWorldPtr()       { return s_manualWorldPtr; }
uintptr_t ManualSpawnFn()        { return s_manualSpawnFn; }
uintptr_t ManualSetTransformFn() { return s_manualSetTransFn; }
uintptr_t ManualDestroyFn()      { return s_manualDestroyFn; }

bool ApplyManualOverrides() {
    if (!s_manualWorldPtr || !s_manualSpawnFn) {
        Logger::Warn("EngineBridge: manual override needs GWorld + SpawnActor at minimum "
                     "(world=0x%08X spawn=0x%08X).",
                     static_cast<unsigned>(s_manualWorldPtr),
                     static_cast<unsigned>(s_manualSpawnFn));
        return false;
    }
    g_bridge = Bridge{};
    g_bridge.worldPtr    = s_manualWorldPtr;
    g_bridge.spawnByType = reinterpret_cast<SpawnByTypeFn>(s_manualSpawnFn);
    if (s_manualSetTransFn)
        g_bridge.setTransform = reinterpret_cast<SetTransformFn>(s_manualSetTransFn);
    if (s_manualDestroyFn)
        g_bridge.destroyActor = reinterpret_cast<DestroyActorFn>(s_manualDestroyFn);
    g_bridge.ready = true;
    Logger::Info("EngineBridge: MANUAL OVERRIDE active "
                 "(world=0x%08X spawn=0x%08X setT=0x%08X dest=0x%08X).",
                 static_cast<unsigned>(s_manualWorldPtr),
                 static_cast<unsigned>(s_manualSpawnFn),
                 static_cast<unsigned>(s_manualSetTransFn),
                 static_cast<unsigned>(s_manualDestroyFn));
    return true;
}

uintptr_t GWorldGlobalAddr() { return s_gworldGlobalAddr; }

void RefreshWorldPtr() {
    if (!s_gworldGlobalAddr) return;
    uintptr_t val = 0;
    if (Memory::SafeRead(s_gworldGlobalAddr, val) && val) {
        g_bridge.worldPtr = val;
        s_manualWorldPtr  = val;
    }
}

// Scan EXE writable .data sections for a 4-byte value equal to targetValue.
// Returns the address of the first slot found, or 0.
static uintptr_t ScanForPointerValue(uintptr_t targetValue) {
    if (!targetValue) return 0;
    HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) return 0;
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    const auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uintptr_t base = reinterpret_cast<uintptr_t>(mod) + sec->VirtualAddress;
        size_t    size = sec->Misc.VirtualSize;
        for (size_t off = 0; off + 4 <= size; off += 4) {
            uintptr_t val = 0;
            if (Memory::SafeRead(base + off, val) && val == targetValue)
                return base + off;
        }
    }
    return 0;
}

// -----------------------------------------------------------------------
//  AutoFindGWorld — derive GWorld from the live GObjects array.
//
//  Step 1: walk GObjects looking for an object whose UClass FName == "World"
//          (i.e., the live UWorld instance for the current level).
//  Step 2: scan EXE .data for a global slot that stores the UWorld* value
//          — that slot is the GWorld global, needed to survive level reloads.
//  Step 3: log UWorld's vtable to help identify SpawnActor in IDA/x64dbg.
// -----------------------------------------------------------------------
bool AutoFindGWorld() {
    if (!ObjectDumper::IsUE3GObjectsActive()) {
        Logger::Warn("EngineBridge: AutoFindGWorld: GObjects not active — run ObjectDumper first.");
        return false;
    }

    uintptr_t uworldPtr = ObjectDumper::FindObjectByClassName("World");
    if (!uworldPtr) {
        Logger::Warn("EngineBridge: AutoFindGWorld: no UWorld instance found in GObjects "
                     "(no object with UClass.Name==\"World\"). Is a level loaded?");
        return false;
    }
    Logger::Info("EngineBridge: UWorld instance @ 0x%08X", static_cast<unsigned>(uworldPtr));

    // Try to locate the GWorld global (a UWorld** in EXE .data).
    uintptr_t gworldGlobal = ScanForPointerValue(uworldPtr);
    if (gworldGlobal) {
        s_gworldGlobalAddr = gworldGlobal;
        Logger::Info("EngineBridge: GWorld global @ 0x%08X (holds UWorld*=0x%08X)",
                     static_cast<unsigned>(gworldGlobal), static_cast<unsigned>(uworldPtr));
    } else {
        Logger::Warn("EngineBridge: GWorld global not found in EXE .data; "
                     "using direct UWorld* (reload-unsafe).");
    }

    s_manualWorldPtr = uworldPtr;

    // Log UWorld vtable — helps locate SpawnActor in IDA/x64dbg.
    // In UE3 single-inheritance C++, vtable ptr is at object offset 0.
    uintptr_t vtbl = 0;
    if (Memory::SafeRead(uworldPtr, vtbl) && vtbl > 0x10000u && vtbl < 0x80000000u) {
        Logger::Info("EngineBridge: UWorld vtable @ 0x%08X -- cross-reference in IDA to find SpawnActor:",
                     static_cast<unsigned>(vtbl));
        for (int i = 0; i < 120; ++i) {
            uintptr_t fn = 0;
            if (!Memory::SafeRead(vtbl + static_cast<uintptr_t>(i) * 4u, fn)) break;
            if (!fn) continue;
            Logger::Info("EngineBridge:   vt[%3d] = 0x%08X", i, static_cast<unsigned>(fn));
        }
    }

    if (s_manualWorldPtr && s_manualSpawnFn)
        return ApplyManualOverrides();

    Logger::Info("EngineBridge: GWorld=0x%08X set. Enter SpawnActor address to go ONLINE.",
                 static_cast<unsigned>(uworldPtr));
    return true;
}

// -----------------------------------------------------------------------
//  Rotation-unit conversion — the ONE place that knows how our degrees
//  map to the engine's rotation format.  LEAD most likely uses Euler
//  float degrees, but this could be radians or a packed int; isolate it
//  here so a single edit fixes everything once RE confirms the format.
// -----------------------------------------------------------------------
static Rot3 ToEngineRotation(const Rot3& degrees) {
    // Hypothesis: engine takes degrees as-is.  Adjust after RE.
    return degrees;
}

// -----------------------------------------------------------------------
//  Init — bring up the bridge.  Priority order:
//    1. Manual overrides (both WorldPtr + SpawnFn set) → immediate go-online.
//    2. AutoFindGWorld() from GObjects (sets WorldPtr automatically if a level
//       is loaded; SpawnActor still needs to be entered manually or found via RE).
//    3. Signature path — placeholder wildcards until RE fills them; always fails.
// -----------------------------------------------------------------------
bool Init() {
    g_bridge = Bridge{};   // reset

    // Manual overrides take full priority (entered via Discovery panel or
    // carried over from AutoFindGWorld on a previous Init call).
    if (s_manualWorldPtr && s_manualSpawnFn)
        return ApplyManualOverrides();

    // Auto-discover GWorld from ObjectDumper's live GObjects array.
    // Requires ObjectDumper::Init() to have run first (done in LevelEditor::Init).
    if (ObjectDumper::IsUE3GObjectsActive() && !s_manualWorldPtr)
        AutoFindGWorld();

    // If AutoFindGWorld also provided SpawnFn (from a previous manual set), go online.
    if (s_manualWorldPtr && s_manualSpawnFn)
        return ApplyManualOverrides();

    // Signature path: all *_SIG values are placeholder wildcards until RE provides
    // real byte patterns. GWORLD_SIG is also a 64-bit pattern that cannot match
    // this 32-bit process. Skipped — just report the offline status.
    Logger::Warn("EngineBridge: offline%s. "
                 "Enter SpawnActor address in Level Editor > Discovery to go ONLINE.",
                 s_manualWorldPtr ? " (GWorld found, SpawnActor missing)"
                                  : " (GWorld not found -- load a level and re-init)");
    return false;
}

void Shutdown() {
    g_bridge = Bridge{};
}

bool IsReady() {
    return g_bridge.ready;
}

// -----------------------------------------------------------------------
//  Facade — guarded + crash-contained raw calls.
// -----------------------------------------------------------------------
ActorHandle SpawnActor(uintptr_t typeNode, const Vec3& pos,
                       const Rot3& rotDeg, const Vec3S& scale) {
    if (!g_bridge.ready || !g_bridge.spawnByType || !typeNode) return 0;

    const Rot3 rot = ToEngineRotation(rotDeg);
    ActorHandle handle = 0;
    __try {
        handle = g_bridge.spawnByType(g_bridge.worldPtr, typeNode, &pos, &rot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logger::Error("EngineBridge: SpawnActor faulted — ABI/signature is wrong.");
        return 0;
    }

    // The spawn factory may not apply scale; push the full transform afterwards.
    if (handle) SetActorTransform(handle, pos, rotDeg, scale);
    return handle;
}

bool SetActorTransform(ActorHandle h, const Vec3& pos,
                       const Rot3& rotDeg, const Vec3S& scale) {
    if (!g_bridge.ready || !g_bridge.setTransform || !h) return false;

    const Rot3 rot = ToEngineRotation(rotDeg);
    __try {
        g_bridge.setTransform(h, &pos, &rot, &scale);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logger::Error("EngineBridge: SetActorTransform faulted.");
        return false;
    }
}

bool DestroyActor(ActorHandle h) {
    if (!g_bridge.ready || !g_bridge.destroyActor || !h) return false;
    __try {
        return g_bridge.destroyActor(h);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logger::Error("EngineBridge: DestroyActor faulted.");
        return false;
    }
}

} // namespace EngineBridge
