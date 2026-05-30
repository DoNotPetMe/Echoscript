#include "prop_database.h"
#include "../utils/logger.h"

namespace PropDatabase {

static std::vector<PropEntry> g_entries;

// -----------------------------------------------------------------------
//  Seed catalog — pulled from the auto-generated include.  The X-macro
//  PROP(name, category) expands once here to build the table.
// -----------------------------------------------------------------------
void Init() {
    g_entries.clear();

    uint32_t id = 0;
    auto add = [&](const char* name, Category cat) {
        g_entries.push_back(PropEntry{ id++, name, cat, 0 });
    };

    #define PROP(NAME, CAT) add(NAME, Category::CAT);
    #include "prop_catalog.inc"
    #undef PROP

    Logger::Info("PropDatabase: seeded %zu spawnable types from LEAD catalog.",
                 g_entries.size());
}

const std::vector<PropEntry>& Entries() {
    return g_entries;
}

const char* CategoryName(Category c) {
    switch (c) {
        case Category::StaticVisual:  return "Static / Visual";
        case Category::CharacterAI:   return "Characters / AI";
        case Category::Camera:        return "Cameras";
        case Category::GameplayItem:  return "Gameplay Items";
        case Category::VolumeTrigger: return "Volumes / Triggers";
        case Category::NavMarker:     return "Navigation / Markers";
        case Category::ManagerLogic:  return "Managers / Logic";
        default:                      return "Unknown";
    }
}

uint32_t ResolveByName(const std::string& name) {
    for (const auto& e : g_entries)
        if (e.name == name) return e.classId;
    return UINT32_MAX;
}

PropEntry* Get(uint32_t classId) {
    if (classId >= g_entries.size()) return nullptr;
    return &g_entries[classId];
}

void SetTypeNode(uint32_t classId, uintptr_t node) {
    if (PropEntry* e = Get(classId)) e->typeNode = node;
}

size_t ResolvedCount() {
    size_t n = 0;
    for (const auto& e : g_entries) if (e.typeNode) ++n;
    return n;
}

} // namespace PropDatabase
