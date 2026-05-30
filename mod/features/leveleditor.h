#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "engine_bridge.h"
#include "gizmo.h"

// -----------------------------------------------------------------------
//  LevelEditor — the orchestrator feature.
//
//  Follows the standard feature contract (Init/Update/Toggle/Shutdown) and
//  is the only new feature dllmain/menu drive directly. It owns the list of
//  placed props, the current selection, and ties together EngineBridge
//  (spawn/transform/destroy), PropDatabase (catalog), the Gizmo (on-screen
//  manipulation), and LevelIO (save/load).
//
//  Toggle hotkey: F6.
// -----------------------------------------------------------------------

namespace LevelEditor {

    struct PlacedProp {
        uint32_t                 id          = 0;   // editor-local stable id
        std::string              className;          // serialized identity
        uint32_t                 classId     = UINT32_MAX; // index into PropDatabase
        EngineBridge::Vec3       position;
        EngineBridge::Rot3       rotation;           // degrees
        EngineBridge::Vec3S      scale;
        EngineBridge::ActorHandle runtimeActor = 0;  // live ptr; not serialized
        bool                     spawnFailed = false;
    };

    struct Config {
        bool        enabled      = false;
        Gizmo::Mode gizmoMode    = Gizmo::Mode::Translate;
        float       spawnDist    = 200.0f;          // units in front of camera
        std::string levelPath    = "blacklist_level.json";
        std::string mapName;                         // optional checkpoint id
    };
    extern Config g_config;

    struct State {
        std::vector<PlacedProp> props;
        int                     selected     = -1;   // index into props, -1 = none
        uint32_t                browserClass = UINT32_MAX; // selected catalog entry
        uint32_t                nextId       = 1;
        std::string             lastError;
    };
    extern State g_state;

    // ---- Feature contract ----
    bool Init();
    void Update(float dt);
    void Toggle();
    void Shutdown();

    // ---- Drawn inside the ImGui frame (called from Menu) ----
    void RenderMenu();    // the "Level Editor" collapsing header + controls
    void RenderGizmo();   // the on-screen 3D handles for the selection

    // ---- Actions (also invoked from the menu) ----
    void SpawnSelected();           // spawn browserClass in front of the camera
    void DeleteSelected();
    void ClearAll();
    bool SaveLevel();
    bool LoadLevel();               // clears current, loads + respawns

} // namespace LevelEditor
