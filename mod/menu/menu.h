#pragma once

namespace Menu {

    extern bool g_menuOpen;

    // Called every frame regardless of menu visibility (runs feature ticks)
    void Tick(float dt);

    // Called inside ImGui::NewFrame() / ImGui::Render() block
    void Render();

} // namespace Menu
