#pragma once
#include <d3d11.h>

namespace D3D11Hook {

    // Sets up the VMT hook on IDXGISwapChain::Present.
    // Must be called from a thread running inside the game process after D3D11 init.
    bool Install();
    void Uninstall();

    // True once ImGui has been initialised (safe to call ImGui:: APIs)
    extern bool g_imguiReady;

    // The hooked device and context (available after Install succeeds)
    extern ID3D11Device*        g_device;
    extern ID3D11DeviceContext* g_context;

} // namespace D3D11Hook
