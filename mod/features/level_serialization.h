#pragma once
#include <string>
#include <vector>
#include "engine_bridge.h"

// -----------------------------------------------------------------------
//  LevelIO — JSON save/load of placed props (nlohmann/json, header-only).
//
//  Saved files deliberately EXCLUDE volatile runtime data (actor handles,
//  type-node pointers); the persistence key is the LEAD type name string.
//  On load, the editor re-resolves names -> classId -> typeNode and respawns.
// -----------------------------------------------------------------------

namespace LevelIO {

    // Mirror of LevelEditor::PlacedProp's serializable fields (kept here to
    // avoid a circular include with leveleditor.h).
    struct SavedProp {
        uint32_t              id = 0;
        std::string           className;
        EngineBridge::Vec3    position;
        EngineBridge::Rot3    rotation;   // degrees
        EngineBridge::Vec3S   scale;
    };

    struct LevelFile {
        int                    version = 1;
        std::string            map;       // optional checkpoint/map id
        std::vector<SavedProp> props;
    };

    bool Save(const std::string& path, const LevelFile& lvl, std::string& err);
    bool Load(const std::string& path, LevelFile& out,       std::string& err);

} // namespace LevelIO
