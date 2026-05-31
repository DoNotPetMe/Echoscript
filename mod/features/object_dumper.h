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
        // (Used only by the legacy string-anchor fallback path.)
        size_t node_name_ptr_off   = 0x00;  // char* / FString to the type name
        size_t node_parent_ptr_off = 0x08;  // pointer to parent type node
        size_t node_next_ptr_off   = 0x10;  // next node (if registry is a list)
        size_t node_ctor_ptr_off   = 0x18;  // constructor/factory fn (if present)
        bool   name_is_wide        = false; // wide vs ansi string in the node

        // ---- UE3 32-bit object model (used by the GObjects path) ----
        // Standard UDK/UE3 32-bit layout; Blacklist's LEAD build may differ
        // slightly, so these are tunable live in the menu. Defaults are the
        // common UDK values.
        size_t uobj_name_off  = 0x2C;  // FName.Index (int32) inside a UObject
        size_t uobj_class_off = 0x34;  // UClass* inside a UObject
        size_t fname_str_off  = 0x10;  // ANSI name chars inside an FNameEntry
    };

    struct Config {
        Layout      layout;
        bool        dumpToFile = true;
        std::string dumpPath   = "blacklist_typedump.txt";
        int         maxNodes   = 8192;       // safety cap on the legacy walk
    };
    extern Config g_config;

    // Live discovered globals (0 if not found). Exposed for the menu readout.
    uintptr_t GNamesPtr();
    uintptr_t GObjectsPtr();

    // Resolve a UE3 FName index to its string via the discovered GNames.
    // Returns false if GNames isn't located or the entry is unreadable.
    bool ResolveFName(int index, std::string& out);

    // Heuristic auto-finder: scans the module's data for the global GNames and
    // GObjects arrays without a hardcoded byte signature, validating GNames[0]
    // == "None" and that object names resolve. Returns true if both found.
    bool AutoFindGlobals();

    // Registry-anchor signature: located by searching data sections for the
    // literal type-name strings, then finding a node that points at one.
    // These are filled at runtime by the anchor scan, not by a static SIG.

    bool   Init();               // locate registry: UE3 GObjects or LEAD anchor
    size_t DumpAll();            // walk registry, log + optional file, returns node count
    size_t ResolveProps();       // match registry names to PropDatabase, fill typeNodes
    bool   IsRegistryFound();    // true if either path succeeded
    bool   IsUE3GObjectsActive(); // true if the UE3 GObjects path is active

} // namespace ObjectDumper
