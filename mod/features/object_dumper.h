#pragma once
#include <cstdint>
#include <string>

// -----------------------------------------------------------------------
//  ObjectDumper — runtime discovery tool for the LEAD type registry.
//
//  Strategy (LEAD-specific, NOT a generic UE GObjects walk):
//  the type-name strings ("Actor", "Mesh", ...) are baked into the game
//  binary because data-defined types are registered by name at startup.
//  We scan for those strings, find the registry node that references each,
//  and walk the registry to recover live `typeNode` pointers — which we
//  hand to PropDatabase so the spawn factory can be called by type.
//
//  All structural offsets are config-driven and editable LIVE in the menu,
//  because the exact node layout is unknown until confirmed against the
//  running game.  This tool is read-only and the lowest-risk bring-up step.
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

    bool   Init();          // locate registry base via string anchors
    size_t DumpAll();       // walk registry, log + optional file, returns node count
    size_t ResolveProps();  // match registry names to PropDatabase, fill typeNodes
    bool   IsRegistryFound();

} // namespace ObjectDumper
