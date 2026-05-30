#pragma once
#include <cstdint>
#include <string>
#include <vector>

// -----------------------------------------------------------------------
//  PropDatabase — catalog of spawnable LEAD actor types.
//
//  Seeded statically from prop_catalog.inc (auto-generated from the game's
//  definitions.xml — the 62 placeable types descending from "Actor").
//  The runtime ObjectDumper fills in each entry's live `typeNode` pointer
//  by matching names against the in-memory LEAD type registry.
// -----------------------------------------------------------------------

namespace PropDatabase {

    enum class Category {
        StaticVisual,
        CharacterAI,
        Camera,
        GameplayItem,
        VolumeTrigger,
        NavMarker,
        ManagerLogic,
        Count
    };

    struct PropEntry {
        uint32_t    classId  = 0;   // stable index into the catalog (our id)
        std::string name;           // LEAD type name, e.g. "Mesh"
        Category    category = Category::StaticVisual;
        uintptr_t   typeNode = 0;   // live registry node ptr; 0 until dumper resolves it
    };

    void Init();   // build the seed catalog

    const std::vector<PropEntry>& Entries();
    const char* CategoryName(Category c);

    // Resolve by LEAD type name; returns classId or UINT32_MAX if absent.
    uint32_t ResolveByName(const std::string& name);

    PropEntry* Get(uint32_t classId);             // nullptr if out of range
    void       SetTypeNode(uint32_t classId, uintptr_t node);

    // Stats for the menu (e.g. "37 / 62 types resolved").
    size_t ResolvedCount();

} // namespace PropDatabase
