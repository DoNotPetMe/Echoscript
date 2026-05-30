#include "menu.h"
#include "../features/freecam.h"
#include "../features/leveleditor.h"
#include "../utils/logger.h"
#include "../utils/cam_finder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Xinput.h>

#include "imgui.h"
#include <cstdlib>

namespace Menu {

bool g_menuOpen = false;

// Toggle key
static constexpr int TOGGLE_KEY = VK_INSERT;

static bool s_wasToggleDown           = false;
static bool s_wasControllerToggleDown = false;

// Show/hide the cursor and ImGui software cursor when the menu state changes.
// Called whenever g_menuOpen is toggled.
static void ApplyCursorState(bool menuOpen) {
    ImGuiIO& io = ImGui::GetIO();
    if (menuOpen) {
        // Draw ImGui's built-in software cursor so the mouse is visible even
        // when the game has hidden the Windows hardware cursor.
        io.MouseDrawCursor = true;
        // Un-confine the cursor so it can reach any corner of the overlay.
        ClipCursor(nullptr);
    } else {
        io.MouseDrawCursor = false;
    }
}

// -----------------------------------------------------------------------
//  Tick — runs every frame
// -----------------------------------------------------------------------
void Tick(float dt) {
    // Edge-triggered INSERT toggle (keyboard)
    bool kbDown = (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;
    if (kbDown && !s_wasToggleDown) {
        g_menuOpen = !g_menuOpen;
        ApplyCursorState(g_menuOpen);
    }
    s_wasToggleDown = kbDown;

    // Edge-triggered L3+R3 toggle (XInput controller)
    XINPUT_STATE xi{};
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (XInputGetState(i, &xi) == ERROR_SUCCESS) {
            constexpr WORD kChord = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
            bool ctrlDown = (xi.Gamepad.wButtons & kChord) == kChord;
            if (ctrlDown && !s_wasControllerToggleDown) {
                g_menuOpen = !g_menuOpen;
                ApplyCursorState(g_menuOpen);
            }
            s_wasControllerToggleDown = ctrlDown;
            break; // use first active controller only
        }
    }

    // Mouse wheel changes free-cam speed when camera is active and menu is closed
    if (FreeCam::g_config.enabled && !g_menuOpen) {
        // Scroll state is polled via raw GetAsyncKeyState trick; actual wheel
        // delta is passed through the WndProc hook.  Speed changes in real-time.
    }

    // Run feature updates
    FreeCam::Update(dt);
    LevelEditor::Update(dt);
}

// -----------------------------------------------------------------------
//  Render — ImGui draw calls
// -----------------------------------------------------------------------
void Render() {
    if (!g_menuOpen) return;

    ImGui::SetNextWindowSize(ImVec2(420, 340), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(30, 30),    ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar;

    if (!ImGui::Begin("Splinter Cell: Blacklist — Mod Menu  [INSERT to close]",
                      &g_menuOpen, flags)) {
        ImGui::End();
        return;
    }

    // ---- Header ----
    ImGui::TextDisabled("Controls: INSERT = toggle menu   F5 = toggle free-cam");
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Free Camera ----
    if (ImGui::CollapsingHeader("Free Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();

        bool& fc = FreeCam::g_config.enabled;
        if (ImGui::Checkbox("Enable (F5)", &fc)) {
            if (fc) FreeCam::Toggle(); else FreeCam::Toggle();
            // Toggle() flips the flag itself, so sync back
            fc = FreeCam::g_config.enabled;
        }

        ImGui::SliderFloat("Move Speed",    &FreeCam::g_config.moveSpeed,      0.5f, 200.f, "%.1f u/s");
        ImGui::SliderFloat("Mouse Sensitivity", &FreeCam::g_config.lookSensitivity, 0.01f, 1.f, "%.3f");

        ImGui::Spacing();
        ImGui::TextDisabled("W/S/A/D — move   Q/E — up/down   Shift — sprint");
        ImGui::TextDisabled("Mouse — look around");

        if (FreeCam::g_config.enabled) {
            ImGui::Spacing();
            auto& st = FreeCam::g_state;
            ImGui::Text("Pos:  %.1f  %.1f  %.1f", st.position.x, st.position.y, st.position.z);
            ImGui::Text("Rot:  pitch %.1f  yaw %.1f", st.pitch, st.yaw);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Camera finder (if freecam shows 'pattern not found')");
        ImGui::Spacing();

        // --- Scan button ---
        if (CamFinder::IsScanning()) {
            ImGui::BeginDisabled();
            ImGui::Button("Scanning... (check console)");
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Scan Memory for Camera")) {
                CamFinder::Scan();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(~3 sec, logs to console)");
        }

        // --- Force-base input ---
        static char s_forceAddr[12] = "";
        ImGui::SetNextItemWidth(130.f);
        ImGui::InputText("##forcebase", s_forceAddr, sizeof(s_forceAddr),
                         ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button("Force Cam Base")) {
            uintptr_t addr = static_cast<uintptr_t>(
                std::strtoul(s_forceAddr, nullptr, 16));
            if (addr)
                FreeCam::ForceBase(addr);
            else
                Logger::Warn("FreeCam: invalid address -- enter hex without 0x prefix.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("hex, no 0x");

        ImGui::Unindent();
    }

    ImGui::Spacing();

    // ---- Level Editor (prop spawner) ----
    LevelEditor::RenderMenu();

    ImGui::Spacing();
    ImGui::Separator();

    // ---- Unload ----
    ImGui::Spacing();
    if (ImGui::Button("Unload Mod (DELETE)", ImVec2(-1, 0))) {
        // Signal main thread to eject the DLL
        PostThreadMessageW(GetCurrentThreadId(), WM_QUIT, 0, 0);
    }

    ImGui::End();
}

} // namespace Menu
