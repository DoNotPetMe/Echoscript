#pragma once
#include <DirectXMath.h>
#include "engine_bridge.h"

// -----------------------------------------------------------------------
//  Gizmo — on-screen 3D transform handles + world-to-screen projection.
//
//  Hand-rolled on top of ImGui draw lists so the only new third-party
//  dependency for the whole level editor is nlohmann/json.  (ImGuizmo is
//  a drop-in upgrade if richer rotate/scale handles are wanted later — the
//  Manipulate() interface below was kept close to ImGuizmo's shape.)
//
//  Translate is fully interactive (drag the axis handles).  Rotation and
//  scale are driven by the numeric fields in the menu; the gizmo renders
//  read-only orientation/scale indicators for them.
// -----------------------------------------------------------------------

namespace Gizmo {

    enum class Mode { Translate, Rotate, Scale };

    // Project a world point to screen pixels using a view-projection matrix.
    // Returns false if the point is behind the camera.
    bool WorldToScreen(const DirectX::XMFLOAT3& world,
                       const DirectX::XMMATRIX& viewProj,
                       float screenW, float screenH,
                       DirectX::XMFLOAT2& outScreen);

    // Draw + interact with the gizmo for one selected prop.
    // `pos` is read/written in place when the user drags a translate handle.
    // Returns true if the transform was modified this frame.
    bool Manipulate(Mode mode,
                    const DirectX::XMMATRIX& viewProj,
                    float screenW, float screenH,
                    EngineBridge::Vec3& pos,
                    EngineBridge::Rot3& rot,
                    EngineBridge::Vec3S& scale);

} // namespace Gizmo
