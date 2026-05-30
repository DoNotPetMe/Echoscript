#include "d3d11_hook.h"
#include "../menu/menu.h"
#include "../features/leveleditor.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>

// ImGui headers — supplied by the thirdparty/imgui directory
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// imgui_impl_win32.h keeps this declaration behind a `#if 0`, so declare it
// here at global scope. It must stay outside namespace D3D11Hook so it refers
// to the handler the ImGui backend actually defines.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace D3D11Hook {

// -----------------------------------------------------------------------
//  Globals
// -----------------------------------------------------------------------

bool                 g_imguiReady = false;
ID3D11Device*        g_device     = nullptr;
ID3D11DeviceContext* g_context    = nullptr;

static IDXGISwapChain*          s_swapChain      = nullptr;
static ID3D11RenderTargetView*  s_mainRTV        = nullptr;
static HWND                     s_hwnd           = nullptr;

// Original Present pointer saved before VMT hook
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
static PresentFn s_origPresent = nullptr;

// VMT index of IDXGISwapChain::Present
static constexpr size_t VMT_PRESENT_INDEX = 8;

// Frame timing
static std::chrono::high_resolution_clock::time_point s_lastFrame{};

// -----------------------------------------------------------------------
//  WndProc hook (forward raw input to ImGui)
// -----------------------------------------------------------------------

static WNDPROC s_origWndProc = nullptr;

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_imguiReady) {
        // Let ImGui consume the event first. Qualify with :: so this resolves
        // to the global handler declared by imgui_impl_win32.h, not a symbol
        // inside namespace D3D11Hook.
        if (::ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return 1;

        // Block keyboard/mouse from reaching the game when the menu is open
        if (Menu::g_menuOpen) {
            switch (msg) {
                case WM_KEYDOWN: case WM_KEYUP:
                case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                case WM_CHAR:
                case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_MOUSEWHEEL:
                    return 0;
            }
        }
    }
    return CallWindowProcW(s_origWndProc, hwnd, msg, wp, lp);
}

// -----------------------------------------------------------------------
//  Hooked Present
// -----------------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSwapChain,
                                                UINT SyncInterval, UINT Flags) {
    // ---- One-time ImGui init ----
    if (!g_imguiReady) {
        // Grab the device from the swap chain
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device),
                                         reinterpret_cast<void**>(&g_device)))) {
            return s_origPresent(pSwapChain, SyncInterval, Flags);
        }
        g_device->GetImmediateContext(&g_context);

        // Get back-buffer RTV
        ID3D11Texture2D* bb = nullptr;
        pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
        g_device->CreateRenderTargetView(bb, nullptr, &s_mainRTV);
        bb->Release();

        // Find the game window from the swap-chain description
        DXGI_SWAP_CHAIN_DESC desc{};
        pSwapChain->GetDesc(&desc);
        s_hwnd = desc.OutputWindow;

        // Init ImGui
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename  = nullptr;  // don't write imgui.ini

        ImGui::StyleColorsDark();

        // Tweak style
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding    = 6.f;
        style.FrameRounding     = 4.f;
        style.ScrollbarRounding = 4.f;
        style.GrabRounding      = 3.f;

        ImGui_ImplWin32_Init(s_hwnd);
        ImGui_ImplDX11_Init(g_device, g_context);

        // Hook the window procedure so ImGui gets raw messages
        s_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(HookedWndProc)));

        s_swapChain  = pSwapChain;
        g_imguiReady = true;
        Logger::Info("D3D11Hook: ImGui initialised (hwnd=0x%p)", s_hwnd);
    }

    // ---- Per-frame timing ----
    auto now = std::chrono::high_resolution_clock::now();
    float dt = std::chrono::duration<float>(now - s_lastFrame).count();
    if (dt > 0.1f) dt = 0.1f;   // clamp to avoid huge jumps after a pause
    s_lastFrame = now;

    // ---- Tick mod features ----
    Menu::Tick(dt);

    // ---- Render ImGui overlay ----
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    Menu::Render();

    // Editor gizmo draws to the background draw list every frame (self-guards
    // on the editor being enabled), so handles overlay the game with the menu
    // open or closed.
    LevelEditor::RenderGizmo();

    ImGui::Render();
    g_context->OMSetRenderTargets(1, &s_mainRTV, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    return s_origPresent(pSwapChain, SyncInterval, Flags);
}

// -----------------------------------------------------------------------
//  Install / Uninstall
// -----------------------------------------------------------------------

bool Install() {
    // Create a minimal temporary device + swap chain to read the vtable.
    // We immediately destroy it; we only need the vtable layout.
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount        = 1;
    scd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.Width   = 100;
    scd.BufferDesc.Height  = 100;
    scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SampleDesc.Count   = 1;
    scd.Windowed           = TRUE;

    // Create a dummy invisible window for the temp swap chain
    HWND dummyWnd = CreateWindowExW(0, L"STATIC", L"DX11Dummy",
                                    WS_POPUP, 0, 0, 1, 1,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    scd.OutputWindow = dummyWnd;

    ID3D11Device*        dummyDevice  = nullptr;
    ID3D11DeviceContext* dummyCtx     = nullptr;
    IDXGISwapChain*      dummySC      = nullptr;
    D3D_FEATURE_LEVEL    featureLevel = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &featureLevel, 1, D3D11_SDK_VERSION,
        &scd, &dummySC, &dummyDevice, nullptr, &dummyCtx);

    if (FAILED(hr)) {
        Logger::Error("D3D11Hook: D3D11CreateDeviceAndSwapChain failed (0x%08X)", hr);
        DestroyWindow(dummyWnd);
        return false;
    }

    // Grab the vtable and hook Present
    void** vtable = Memory::GetVTable(dummySC);
    s_origPresent = reinterpret_cast<PresentFn>(
        Memory::HookVMT(vtable, VMT_PRESENT_INDEX,
                        reinterpret_cast<void*>(HookedPresent)));

    Logger::Info("D3D11Hook: Present hooked (orig=0x%p)", reinterpret_cast<void*>(s_origPresent));

    // Clean up temp objects — the hook is in the vtable, which is shared
    dummySC->Release();
    dummyDevice->Release();
    dummyCtx->Release();
    DestroyWindow(dummyWnd);

    s_lastFrame = std::chrono::high_resolution_clock::now();
    return true;
}

void Uninstall() {
    if (!s_origPresent) return;

    // Restore WndProc
    if (s_hwnd && s_origWndProc)
        SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(s_origWndProc));

    // Restore Present VMT slot using the dummy swap-chain path again.
    // Because the vtable is global for all IDXGISwapChain instances,
    // we can use any live swap-chain pointer — or re-read from s_swapChain.
    if (s_swapChain) {
        void** vtable = Memory::GetVTable(s_swapChain);
        Memory::UnhookVMT(vtable, VMT_PRESENT_INDEX,
                          reinterpret_cast<void*>(s_origPresent));
    }

    if (g_imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }

    if (s_mainRTV) { s_mainRTV->Release(); s_mainRTV = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device  = nullptr; }

    s_origPresent = nullptr;
    Logger::Info("D3D11Hook: uninstalled");
}

} // namespace D3D11Hook
