#include "leveleditor.h"
#include "prop_database.h"
#include "object_dumper.h"
#include "engine_bridge.h"
#include "level_serialization.h"
#include "freecam.h"
#include "../hooks/d3d11_hook.h"
#include "../utils/logger.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <DirectXMath.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"

using namespace DirectX;

namespace LevelEditor {

Config g_config{};
State  g_state{};

static bool s_wasToggleDown = false;
static constexpr int TOGGLE_KEY = VK_F6;

// -----------------------------------------------------------------------
//  Init — bring up the sub-systems the editor depends on.
// -----------------------------------------------------------------------
bool Init() {
    PropDatabase::Init();          // seed the 62-type catalog (always works)
    ObjectDumper::Init();          // locate the LEAD type registry (best-effort)
    if (ObjectDumper::IsRegistryFound())
        ObjectDumper::ResolveProps();   // fill live typeNodes for catalog entries
    EngineBridge::Init();          // resolve spawn/transform/destroy (best-effort)

    Logger::Info("LevelEditor: ready (engine bridge %s).",
                 EngineBridge::IsReady() ? "ONLINE" : "offline — spawning disabled");
    return true;
}

// -----------------------------------------------------------------------
//  Update — F6 toggle. The actual rendering happens in RenderMenu/RenderGizmo.
// -----------------------------------------------------------------------
void Update(float /*dt*/) {
    bool down = (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;
    if (down && !s_wasToggleDown)
        Toggle();
    s_wasToggleDown = down;
}

void Toggle() {
    g_config.enabled = !g_config.enabled;
    Logger::Info("LevelEditor: %s", g_config.enabled ? "enabled" : "disabled");
}

void Shutdown() {
    // Best-effort: remove anything we spawned so we don't leave dangling actors.
    for (auto& p : g_state.props)
        if (p.runtimeActor) EngineBridge::DestroyActor(p.runtimeActor);
    g_state.props.clear();
    g_state.selected = -1;
    EngineBridge::Shutdown();
    g_config.enabled = false;
}

// -----------------------------------------------------------------------
//  Spawn / delete / clear
// -----------------------------------------------------------------------
static EngineBridge::Vec3 SpawnPointInFront() {
    XMFLOAT3 camPos{};
    FreeCam::GetCameraPosition(camPos);
    XMMATRIX view;
    EngineBridge::Vec3 out{ camPos.x, camPos.y, camPos.z };
    if (FreeCam::GetViewMatrix(view)) {
        // Forward vector is the 3rd row of the inverse-view; for a LookAtLH
        // view matrix the camera forward is row 2 of its rotation transpose.
        XMVECTOR fwd = XMVector3Normalize(XMVectorSet(
            XMVectorGetZ(view.r[0]), XMVectorGetZ(view.r[1]), XMVectorGetZ(view.r[2]), 0));
        out.x += XMVectorGetX(fwd) * g_config.spawnDist;
        out.y += XMVectorGetY(fwd) * g_config.spawnDist;
        out.z += XMVectorGetZ(fwd) * g_config.spawnDist;
    }
    return out;
}

void SpawnSelected() {
    PropDatabase::PropEntry* e = PropDatabase::Get(g_state.browserClass);
    if (!e) { g_state.lastError = "no prop type selected"; return; }

    PlacedProp prop;
    prop.id        = g_state.nextId++;
    prop.className = e->name;
    prop.classId   = e->classId;
    prop.position  = SpawnPointInFront();
    prop.scale     = EngineBridge::Vec3S{};

    if (e->typeNode && EngineBridge::IsReady()) {
        prop.runtimeActor = EngineBridge::SpawnActor(
            e->typeNode, prop.position, prop.rotation, prop.scale);
        prop.spawnFailed = (prop.runtimeActor == 0);
    } else {
        // No live type node / bridge offline: record the placement anyway so
        // it persists and can spawn once RE is complete.
        prop.spawnFailed = true;
    }

    g_state.props.push_back(prop);
    g_state.selected = static_cast<int>(g_state.props.size()) - 1;

    if (prop.spawnFailed)
        g_state.lastError = "placed '" + e->name +
            "' (not live: engine bridge offline or type unresolved)";
    else
        g_state.lastError.clear();
}

void DeleteSelected() {
    if (g_state.selected < 0 || g_state.selected >= (int)g_state.props.size()) return;
    auto& p = g_state.props[g_state.selected];
    if (p.runtimeActor) EngineBridge::DestroyActor(p.runtimeActor);
    g_state.props.erase(g_state.props.begin() + g_state.selected);
    g_state.selected = -1;
}

void ClearAll() {
    for (auto& p : g_state.props)
        if (p.runtimeActor) EngineBridge::DestroyActor(p.runtimeActor);
    g_state.props.clear();
    g_state.selected = -1;
}

// -----------------------------------------------------------------------
//  Save / load
// -----------------------------------------------------------------------
bool SaveLevel() {
    LevelIO::LevelFile lvl;
    lvl.version = 1;
    lvl.map     = g_config.mapName;
    for (const auto& p : g_state.props)
        lvl.props.push_back(LevelIO::SavedProp{ p.id, p.className, p.position, p.rotation, p.scale });

    std::string err;
    if (!LevelIO::Save(g_config.levelPath, lvl, err)) {
        g_state.lastError = "save failed: " + err;
        return false;
    }
    g_state.lastError = "saved " + std::to_string(lvl.props.size()) + " props";
    return true;
}

bool LoadLevel() {
    LevelIO::LevelFile lvl;
    std::string err;
    if (!LevelIO::Load(g_config.levelPath, lvl, err)) {
        g_state.lastError = "load failed: " + err;
        return false;
    }

    ClearAll();
    g_config.mapName = lvl.map;

    for (const auto& sp : lvl.props) {
        PlacedProp prop;
        prop.id        = sp.id;
        prop.className = sp.className;
        prop.position  = sp.position;
        prop.rotation  = sp.rotation;
        prop.scale     = sp.scale;
        prop.classId   = PropDatabase::ResolveByName(sp.className);

        PropDatabase::PropEntry* e = PropDatabase::Get(prop.classId);
        if (e && e->typeNode && EngineBridge::IsReady()) {
            prop.runtimeActor = EngineBridge::SpawnActor(
                e->typeNode, prop.position, prop.rotation, prop.scale);
            prop.spawnFailed = (prop.runtimeActor == 0);
        } else {
            prop.spawnFailed = true;   // kept in the list; not lost
        }

        g_state.nextId = (prop.id >= g_state.nextId) ? prop.id + 1 : g_state.nextId;
        g_state.props.push_back(prop);
    }

    int failed = 0;
    for (auto& p : g_state.props) if (p.spawnFailed) ++failed;
    g_state.lastError = "loaded " + std::to_string(g_state.props.size()) +
                        " props (" + std::to_string(failed) + " not live)";
    return true;
}

// -----------------------------------------------------------------------
//  Menu UI
// -----------------------------------------------------------------------
void RenderMenu() {
    if (!ImGui::CollapsingHeader("Level Editor"))
        return;

    ImGui::Indent();

    // Master enable + status
    ImGui::Checkbox("Enable Editor (F6)", &g_config.enabled);
    ImGui::SameLine();
    ImGui::TextColored(EngineBridge::IsReady() ? ImVec4(0.4f,1,0.4f,1) : ImVec4(1,0.6f,0.3f,1),
                       EngineBridge::IsReady() ? "bridge: ONLINE" : "bridge: offline");

    // ---- Discovery / dumper ----
    if (ImGui::TreeNode("Discovery (Object Dumper)")) {
        auto& L = ObjectDumper::g_config.layout;
        ImGui::TextDisabled("Tune offsets, then Dump until names print correctly.");
        ImGui::InputScalar("name ptr off",   ImGuiDataType_U64, &L.node_name_ptr_off);
        ImGui::InputScalar("parent ptr off", ImGuiDataType_U64, &L.node_parent_ptr_off);
        ImGui::InputScalar("next ptr off",   ImGuiDataType_U64, &L.node_next_ptr_off);
        ImGui::InputScalar("ctor ptr off",   ImGuiDataType_U64, &L.node_ctor_ptr_off);
        ImGui::Checkbox("name is wide (UTF-16)", &L.name_is_wide);

        if (ImGui::Button("Re-locate registry")) ObjectDumper::Init();
        ImGui::SameLine();
        if (ImGui::Button("Dump types")) ObjectDumper::DumpAll();
        ImGui::SameLine();
        if (ImGui::Button("Resolve catalog")) ObjectDumper::ResolveProps();

        ImGui::Text("Resolved: %zu / %zu types",
                    PropDatabase::ResolvedCount(), PropDatabase::Entries().size());
        ImGui::TreePop();
    }

    // ---- Prop browser ----
    if (ImGui::TreeNodeEx("Prop Browser", ImGuiTreeNodeFlags_DefaultOpen)) {
        static char filter[64] = "";
        static int  catFilter  = 0;   // 0 = All; 1..7 map to categories
        ImGui::InputText("search", filter, sizeof(filter));
        ImGui::Combo("category", &catFilter,
                     "All\0Static / Visual\0Characters / AI\0Cameras\0Gameplay Items\0"
                     "Volumes / Triggers\0Navigation / Markers\0Managers / Logic\0");

        ImGui::BeginChild("proplist", ImVec2(0, 140), true);
        for (const auto& e : PropDatabase::Entries()) {
            if (catFilter > 0 && (int)e.category != (catFilter - 1)) continue;
            if (filter[0] && e.name.find(filter) == std::string::npos) continue;

            bool sel = (e.classId == g_state.browserClass);
            std::string label = e.name + (e.typeNode ? "  [live]" : "");
            if (ImGui::Selectable(label.c_str(), sel))
                g_state.browserClass = e.classId;
        }
        ImGui::EndChild();

        ImGui::SliderFloat("spawn distance", &g_config.spawnDist, 20.f, 1000.f, "%.0f");
        if (ImGui::Button("Spawn at camera", ImVec2(-1, 0)))
            SpawnSelected();
        ImGui::TreePop();
    }

    // ---- Placed props ----
    if (ImGui::TreeNodeEx("Placed Props", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("placedlist", ImVec2(0, 120), true);
        for (int i = 0; i < (int)g_state.props.size(); ++i) {
            const auto& p = g_state.props[i];
            char label[128];
            snprintf(label, sizeof(label), "#%u  %s", p.id, p.className.c_str());
            if (p.spawnFailed)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.5f, 0.5f, 1));
            if (ImGui::Selectable(label, g_state.selected == i))
                g_state.selected = i;
            if (p.spawnFailed) ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        if (ImGui::Button("Delete selected")) DeleteSelected();
        ImGui::SameLine();
        if (ImGui::Button("Clear all")) ClearAll();
        ImGui::TreePop();
    }

    // ---- Transform panel ----
    if (g_state.selected >= 0 && g_state.selected < (int)g_state.props.size()) {
        auto& p = g_state.props[g_state.selected];
        if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            int mode = (int)g_config.gizmoMode;
            ImGui::RadioButton("Translate", &mode, 0); ImGui::SameLine();
            ImGui::RadioButton("Rotate", &mode, 1);    ImGui::SameLine();
            ImGui::RadioButton("Scale", &mode, 2);
            g_config.gizmoMode = (Gizmo::Mode)mode;

            bool changed = false;
            changed |= ImGui::DragFloat3("position", &p.position.x, 1.0f);
            changed |= ImGui::DragFloat3("rotation", &p.rotation.pitch, 0.5f);
            changed |= ImGui::DragFloat3("scale",    &p.scale.x, 0.01f, 0.01f, 100.f);

            if (changed && p.runtimeActor)
                EngineBridge::SetActorTransform(p.runtimeActor, p.position, p.rotation, p.scale);
            ImGui::TreePop();
        }
    }

    // ---- Persistence ----
    if (ImGui::TreeNode("Save / Load")) {
        static char pathBuf[260];
        // sync buffer from config once per open
        if (pathBuf[0] == '\0')
            strncpy_s(pathBuf, g_config.levelPath.c_str(), _TRUNCATE);
        if (ImGui::InputText("file", pathBuf, sizeof(pathBuf)))
            g_config.levelPath = pathBuf;
        if (ImGui::Button("Save")) SaveLevel();
        ImGui::SameLine();
        if (ImGui::Button("Load")) LoadLevel();
        ImGui::TreePop();
    }

    if (!g_state.lastError.empty())
        ImGui::TextWrapped("%s", g_state.lastError.c_str());

    ImGui::Unindent();
}

// -----------------------------------------------------------------------
//  Gizmo — drawn to the background draw list so handles overlay the game.
// -----------------------------------------------------------------------
void RenderGizmo() {
    if (!g_config.enabled) return;
    if (g_state.selected < 0 || g_state.selected >= (int)g_state.props.size()) return;
    if (!D3D11Hook::g_imguiReady) return;

    ImGuiIO& io = ImGui::GetIO();
    float aspect = (io.DisplaySize.y > 0.f) ? io.DisplaySize.x / io.DisplaySize.y : 16.f/9.f;

    XMMATRIX viewProj;
    if (!FreeCam::GetViewProjection(viewProj, aspect)) return;

    auto& p = g_state.props[g_state.selected];
    if (Gizmo::Manipulate(g_config.gizmoMode, viewProj,
                          io.DisplaySize.x, io.DisplaySize.y,
                          p.position, p.rotation, p.scale)) {
        if (p.runtimeActor)
            EngineBridge::SetActorTransform(p.runtimeActor, p.position, p.rotation, p.scale);
    }
}

} // namespace LevelEditor
