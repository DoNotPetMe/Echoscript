#include "engine_bridge.h"
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

namespace EngineBridge {

Bridge g_bridge{};

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
//  Init — resolve every signature.  All-or-nothing: if any required
//  function is unresolved we leave ready=false so the editor degrades
//  gracefully (browser still works; spawn buttons report "not ready").
// -----------------------------------------------------------------------
bool Init() {
    g_bridge = Bridge{};   // reset

    uintptr_t spawn = PatternScan::ScanGame(SPAWN_BY_TYPE_SIG);
    uintptr_t setT  = PatternScan::ScanGame(SET_TRANSFORM_SIG);
    uintptr_t dest  = PatternScan::ScanGame(DESTROY_ACTOR_SIG);
    uintptr_t gwIns = PatternScan::ScanGame(GWORLD_SIG);

    if (!spawn || !setT || !dest || !gwIns) {
        Logger::Warn("EngineBridge: signatures unresolved "
                     "(spawn=%d setT=%d dest=%d gworld=%d).",
                     spawn != 0, setT != 0, dest != 0, gwIns != 0);
        Logger::Warn("EngineBridge: spawning disabled. Use the Object Dumper to "
                     "rediscover offsets, then fill the *_SIG values in engine_bridge.h.");
        return false;
    }

    g_bridge.spawnByType  = reinterpret_cast<SpawnByTypeFn>(spawn);
    g_bridge.setTransform = reinterpret_cast<SetTransformFn>(setT);
    g_bridge.destroyActor = reinterpret_cast<DestroyActorFn>(dest);

    uintptr_t worldPtrAddr = PatternScan::ResolveRIPSimple(gwIns, GWORLD_REL32_OFFSET, GWORLD_INSTR_SIZE);
    if (!Memory::SafeRead(worldPtrAddr, g_bridge.worldPtr) || !g_bridge.worldPtr) {
        Logger::Warn("EngineBridge: world pointer null — is a level loaded yet?");
        return false;
    }

    g_bridge.ready = true;
    Logger::Info("EngineBridge: ready (world=0x%p)", reinterpret_cast<void*>(g_bridge.worldPtr));
    return true;
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
