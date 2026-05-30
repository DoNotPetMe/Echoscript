#pragma once
#include <cstdint>
#include <string>

// -----------------------------------------------------------------------
//  ObjectDumper — runtime discovery for spawnable type pointers.
//
//  Architecture insight (from System/Blacklist.ini):
//  Splinter Cell: Blacklist is built on Unreal Engine 3.  The LEAD system
//  is a data-driven scripting / sequencing layer ON TOP of UE3, not a
//  replacement. Key evidence:
//    - Protocol=unreal, MapExt=unr
//    - GameEngine=Engine.GameEngine (standard UE3)
//    - RenderDevice=D3DDrv.D3DRenderDevice (standard UE3 DX9/DX11 path)
//    - EditPackages=Core, Engine, Echelon, EchelonAI, EchelonCharacter ...
//    - Class=EchelonCharacter.ESam  (standard UE3 Package.Class naming)
//
//  This means:
//    - GObjects / GNames arrays are standard UE3 structures.
//    - GWorld (UWorld*) → PersistentLevel → Actors gives all world actors.
//    - Actor class reflection uses UE3 class objects via GObjects.
//    - Spawning actors uses UWorld::SpawnActor (standard UE3 virtual).
//    - LEAD types in definitions.xml are UE3 objects registered in the
//      game's own package (EchelonGameObject, Echelon, ...) and accessible
//      via GObjects by their UE3 class object pointer.
//
//  Two-phase approach (lowest → highest risk):
//  Phase 1 (this file): scan for GObjects/GNames via pattern signatures
//    to find the live UClass* for each catalog type name.
//    - GOBJECTS_SIG: points to the global TArray<FObjectItem> GObjects.
//    - GNAMES_SIG: points to the global TNameEntryArray GNames.
//    - Fallback: LEAD string-anchor walk (find "Actor" literal → type node).
//  Phase 2 (engine_bridge.h): call UWorld::SpawnActor with the resolved
//    UClass* once signatures are confirmed in a disassembler.
//
//  All offsets are config-driven and editable live in the menu.
// -----------------------------------------------------------------------

namespace ObjectDumper {

    struct Layout {
        // Byte offsets within a LEAD type-registry node. HYPOTHESES — tune live.
        size_t node_name_ptr_off   = 0x00;  // char* / FString to the type name
        size_t node_parent_ptr_off = 0x08;  // pointer to parent type node
        size_t node_next_ptr_off   = 0x10;  // next node (if registry is a list)
        size_t node_ctor_ptr_off   = 0x18;  // constructor/factory fn (if present)
        bool   name_is_wide        = false; // wide vs ansi string in the node
    };

    struct Config {
        Layout      layout;
        bool        dumpToFile = true;
        std::string dumpPath   = "blacklist_typedump.txt";
        int         maxNodes   = 8192;       // safety cap on the walk
    };
    extern Config g_config;

    // Registry-anchor signature: located by searching data sections for the
    // literal type-name strings, then finding a node that points at one.
    // These are filled at runtime by the anchor scan, not by a static SIG.

    bool   Init();               // locate registry: UE3 GObjects or LEAD anchor
    size_t DumpAll();            // walk registry, log + optional file, returns node count
    size_t ResolveProps();       // match registry names to PropDatabase, fill typeNodes
    bool   IsRegistryFound();    // true if either path succeeded
    bool   IsUE3GObjectsActive(); // true if the UE3 GObjects path is active

} // namespace ObjectDumper
