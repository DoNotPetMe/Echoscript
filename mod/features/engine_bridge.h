#pragma once
#include <cstdint>
#include <string>
#include <vector>

// -----------------------------------------------------------------------
//  EngineBridge — the reverse-engineering plug-in seam.
//
//  Abstracts the LEAD-engine operations the level editor needs:
//    - spawn an actor of a given type
//    - set an actor's transform
//    - destroy an actor
//    - resolve the global "world"/level pointer
//
//  Everything game-specific lives here behind a POD function-pointer
//  struct that Init() fills via pattern scanning.  The signatures below
//  are PLACEHOLDERS that must be discovered against your game build
//  (see object_dumper for the discovery tooling).  Until they resolve,
//  g_bridge.ready stays false and the facade functions are safe no-ops.
// -----------------------------------------------------------------------

namespace EngineBridge {

    using ActorHandle = uintptr_t;   // raw in-game actor pointer; 0 == none

    struct Vec3  { float x = 0.f, y = 0.f, z = 0.f; };
    struct Rot3  { float pitch = 0.f, yaw = 0.f, roll = 0.f; }; // degrees in OUR model
    struct Vec3S { float x = 1.f, y = 1.f, z = 1.f; };          // scale

    // ------------------------------------------------------------------
    //  Raw game function ABIs — REVERSE-ENGINEER THESE.
    //  The exact calling convention/argument order is unknown until RE;
    //  treat each typedef as a hypothesis confirmed with a disassembler.
    // ------------------------------------------------------------------
    using SpawnByTypeFn   = ActorHandle (*)(uintptr_t world, uintptr_t typeNode,
                                            const Vec3* loc, const Rot3* rot);  // <<< RE
    using SetTransformFn  = void        (*)(ActorHandle actor, const Vec3* loc,
                                            const Rot3* rot, const Vec3S* scale);// <<< RE
    using DestroyActorFn  = bool        (*)(ActorHandle actor);                 // <<< RE

    struct Bridge {
        SpawnByTypeFn  spawnByType  = nullptr;
        SetTransformFn setTransform = nullptr;
        DestroyActorFn destroyActor = nullptr;
        uintptr_t      worldPtr     = 0;
        bool           ready        = false;
    };

    extern Bridge g_bridge;

    // ------------------------------------------------------------------
    //  Signatures (IDA-style stubs; FILL via RE).  Resolved in Init().
    //  Because LEAD actor types are data-defined (not native C++ classes),
    //  the spawn path is expected to be a SINGLE factory parameterised by
    //  the type-registry node — not 62 separate constructors.
    // ------------------------------------------------------------------
    static constexpr const char* SPAWN_BY_TYPE_SIG  = "?? ?? ?? ?? ?? ?? ?? ?? ?? ??"; // <<< FILL
    static constexpr const char* SET_TRANSFORM_SIG  = "?? ?? ?? ?? ?? ?? ?? ?? ?? ??"; // <<< FILL
    static constexpr const char* DESTROY_ACTOR_SIG  = "?? ?? ?? ?? ?? ?? ?? ?? ?? ??"; // <<< FILL
    static constexpr const char* GWORLD_SIG         = "48 8B 05 ?? ?? ?? ?? ?? ?? ?? ??"; // <<< FILL
    static constexpr int GWORLD_REL32_OFFSET = 3;
    static constexpr int GWORLD_INSTR_SIZE   = 7;

    // ------------------------------------------------------------------
    //  Lifecycle
    // ------------------------------------------------------------------
    bool Init();      // scan signatures, fill g_bridge, set ready
    void Shutdown();
    bool IsReady();

    // ------------------------------------------------------------------
    //  Manual address overrides — paste from Cheat Engine / IDA when
    //  the *_SIG values are still wildcards.
    //  worldPtr + spawnFn are the minimum required to go ONLINE.
    //  setTransform and destroyFn may be 0 (those operations become no-ops).
    //  Call ApplyManualOverrides() after setting, or call Init() —
    //  it will detect non-zero overrides and skip signature scanning.
    // ------------------------------------------------------------------
    void      SetManualWorldPtr(uintptr_t addr);
    void      SetManualSpawnFn(uintptr_t addr);
    void      SetManualSetTransformFn(uintptr_t addr);
    void      SetManualDestroyFn(uintptr_t addr);
    bool      ApplyManualOverrides();

    uintptr_t ManualWorldPtr();
    uintptr_t ManualSpawnFn();
    uintptr_t ManualSetTransformFn();
    uintptr_t ManualDestroyFn();

    // ------------------------------------------------------------------
    //  Facade — the editor calls these.  Each guards on g_bridge.ready
    //  and wraps the raw call in __try/__except so a wrong-ABI call is
    //  contained instead of crashing the game.  Rotation-unit conversion
    //  (our degrees -> engine format) lives in ONE place inside the .cpp.
    // ------------------------------------------------------------------
    ActorHandle SpawnActor(uintptr_t typeNode, const Vec3& pos,
                           const Rot3& rotDeg, const Vec3S& scale);
    bool        SetActorTransform(ActorHandle h, const Vec3& pos,
                                  const Rot3& rotDeg, const Vec3S& scale);
    bool        DestroyActor(ActorHandle h);

} // namespace EngineBridge
