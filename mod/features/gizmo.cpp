#include "gizmo.h"
#include "imgui.h"

#include <DirectXMath.h>
#include <cmath>

using namespace DirectX;

namespace Gizmo {

// Length of the axis handles in world units, scaled so they stay a usable
// size on screen regardless of distance (approximate — good enough for an
// editor handle).
static constexpr float AXIS_WORLD_LEN = 60.0f;
static constexpr float HANDLE_PICK_PX = 8.0f;   // click tolerance in pixels

// Per-axis colors (X=red, Y=green, Z=blue), with a highlight when hovered.
static ImU32 AxisColor(int axis, bool hot) {
    const ImU32 base[3] = {
        IM_COL32(230,  60,  60, 255),   // X
        IM_COL32( 60, 220,  60, 255),   // Y
        IM_COL32( 70, 120, 240, 255),   // Z
    };
    if (hot) return IM_COL32(255, 235, 120, 255);
    return base[axis];
}

bool WorldToScreen(const XMFLOAT3& world, const XMMATRIX& viewProj,
                   float screenW, float screenH, XMFLOAT2& outScreen) {
    XMVECTOR p    = XMVectorSet(world.x, world.y, world.z, 1.0f);
    XMVECTOR clip = XMVector4Transform(p, viewProj);  // row-vector convention
    float w = XMVectorGetW(clip);
    if (w < 0.001f) return false;                     // at/behind camera plane

    float ndcX = XMVectorGetX(clip) / w;
    float ndcY = XMVectorGetY(clip) / w;
    outScreen.x = (ndcX * 0.5f + 0.5f) * screenW;
    outScreen.y = (1.0f - (ndcY * 0.5f + 0.5f)) * screenH;  // flip Y for screen space
    return true;
}

// Distance from point p to segment ab, in pixels.
static float DistToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    float vx = b.x - a.x, vy = b.y - a.y;
    float wx = p.x - a.x, wy = p.y - a.y;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0.f ? (wx * vx + wy * vy) / len2 : 0.f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    float dx = a.x + t * vx - p.x;
    float dy = a.y + t * vy - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

// Which axis (if any) is the mouse hovering, given the projected origin and
// the three projected axis tips.
static int HoveredAxis(const ImVec2& mouse, const ImVec2& origin,
                       const ImVec2 tips[3]) {
    int best = -1;
    float bestD = HANDLE_PICK_PX;
    for (int i = 0; i < 3; ++i) {
        float d = DistToSegment(mouse, origin, tips[i]);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// Drag state persists across frames while the mouse button is held.
static int   s_dragAxis = -1;

bool Manipulate(Mode mode, const XMMATRIX& viewProj,
                float screenW, float screenH,
                EngineBridge::Vec3& pos,
                EngineBridge::Rot3& rot,
                EngineBridge::Vec3S& scale) {
    (void)rot; (void)scale;  // rotate/scale are driven via numeric fields

    // Project the prop origin.
    XMFLOAT2 originScr{};
    XMFLOAT3 originWorld{ pos.x, pos.y, pos.z };
    if (!WorldToScreen(originWorld, viewProj, screenW, screenH, originScr))
        return false;

    // Project the three world-axis tips.
    XMFLOAT3 axisWorld[3] = {
        { pos.x + AXIS_WORLD_LEN, pos.y, pos.z },
        { pos.x, pos.y + AXIS_WORLD_LEN, pos.z },
        { pos.x, pos.y, pos.z + AXIS_WORLD_LEN },
    };
    ImVec2 origin(originScr.x, originScr.y);
    ImVec2 tips[3];
    bool   tipVisible[3];
    for (int i = 0; i < 3; ++i) {
        XMFLOAT2 s{};
        tipVisible[i] = WorldToScreen(axisWorld[i], viewProj, screenW, screenH, s);
        tips[i] = ImVec2(s.x, s.y);
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImGuiIO&    io = ImGui::GetIO();
    ImVec2      mouse = io.MousePos;

    int hovered = (mode == Mode::Translate) ? HoveredAxis(mouse, origin, tips) : -1;

    // Begin / continue / end a drag.
    bool modified = false;
    if (mode == Mode::Translate) {
        if (s_dragAxis < 0 && hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            s_dragAxis = hovered;

        if (s_dragAxis >= 0) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // Map mouse delta onto the screen-space axis direction, then
                // scale back into world units along that axis.
                ImVec2 axisDir(tips[s_dragAxis].x - origin.x,
                               tips[s_dragAxis].y - origin.y);
                float axisLenPx = std::sqrt(axisDir.x * axisDir.x + axisDir.y * axisDir.y);
                if (axisLenPx > 1.f) {
                    float inv = 1.0f / axisLenPx;
                    float nx = axisDir.x * inv, ny = axisDir.y * inv;
                    // Projection of mouse delta onto the axis direction (pixels).
                    float deltaPx = io.MouseDelta.x * nx + io.MouseDelta.y * ny;
                    // Pixels -> world units: AXIS_WORLD_LEN spans axisLenPx pixels.
                    float worldDelta = deltaPx * (AXIS_WORLD_LEN / axisLenPx);
                    if (s_dragAxis == 0) pos.x += worldDelta;
                    if (s_dragAxis == 1) pos.y += worldDelta;
                    if (s_dragAxis == 2) pos.z += worldDelta;
                    modified = (deltaPx != 0.f);
                }
            } else {
                s_dragAxis = -1;
            }
        }
    }

    // Draw the handles.
    const char* labels[3] = { "X", "Y", "Z" };
    for (int i = 0; i < 3; ++i) {
        if (!tipVisible[i]) continue;
        bool hot = (i == hovered) || (i == s_dragAxis);
        dl->AddLine(origin, tips[i], AxisColor(i, hot), hot ? 3.0f : 2.0f);
        dl->AddCircleFilled(tips[i], hot ? 5.0f : 4.0f, AxisColor(i, hot));
        dl->AddText(ImVec2(tips[i].x + 4, tips[i].y - 6), AxisColor(i, hot), labels[i]);
    }
    dl->AddCircleFilled(origin, 3.0f, IM_COL32(255, 255, 255, 255));

    return modified;
}

} // namespace Gizmo
