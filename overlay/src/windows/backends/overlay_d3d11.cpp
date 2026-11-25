/******************************************************************************
    at-overlay - in-game overlay for AurexTranslator

    Copyright (C) 2026 by Daniil Nabiulin
    Copyright (C) by Lain Bailey and the OBS Studio contributors (obs-studio)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <d3d11.h>
#include <dxgi.h>

#include "win_common.h"
#include "../capture/d3d/obs_capture_bridge.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

#include "overlay_d3d11.h"
#include "overlay_d3d10.h"
#include "overlay_d3d12.h"
#include "overlay_text.h"
#include "overlay_ui.h"

namespace at {
namespace {
struct OverlayD3D11 {
    IDXGISwapChain*         swap    = nullptr;
    ID3D11Device*           dev     = nullptr;
    ID3D11DeviceContext*    ctx     = nullptr;
    ID3D11RenderTargetView* rtv     = nullptr;

    UINT        width  = 0;
    UINT        height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT        sample_count = 1;

    ImGuiContext* imgui     = nullptr;
    bool          imgui_ok  = false;
};

OverlayD3D11 g_ov;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

/* (plugins/win-capture/graphics-hook/dxgi-capture.cpp) obs*/
int g_swap_mismatch = 0;
constexpr int kSwapMismatchLimit = 16;

struct Lock {
    Lock()  { if (g_lock_ready) EnterCriticalSection(&g_lock); }
    ~Lock() { if (g_lock_ready) LeaveCriticalSection(&g_lock); }
};



// ===============================================================
// Teardown
// ===============================================================

void release_targets()
{
    safe_release(g_ov.rtv);
}

void shutdown_overlay()
{
    if (g_ov.imgui_ok) {
        ImGui::SetCurrentContext(g_ov.imgui);
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(g_ov.imgui);
        g_ov.imgui    = nullptr;
        g_ov.imgui_ok = false;
    }
    release_targets();
    safe_release(g_ov.ctx);
    safe_release(g_ov.dev);
    g_ov.swap   = nullptr;
    g_ov.width  = 0;
    g_ov.height = 0;
    g_ov.format = DXGI_FORMAT_UNKNOWN;
    g_swap_mismatch = 0;
}

// ===============================================================
// Setup
// ===============================================================

bool ensure_device(IDXGISwapChain* swap)
{
    if (g_ov.swap == swap && g_ov.dev) return true;

    shutdown_overlay();

    ID3D11Device* dev = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) {
        return false;
    }

    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) {
        dev->Release();
        return false;
    }

    g_ov.swap = swap;
    g_ov.dev  = dev;
    g_ov.ctx  = ctx;
    return true;
}

bool ensure_targets(IDXGISwapChain* swap)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swap->GetDesc(&desc))) return false;

    const bool changed = (desc.BufferDesc.Width  != g_ov.width) ||
                         (desc.BufferDesc.Height != g_ov.height) ||
                         (desc.BufferDesc.Format != g_ov.format) ||
                         (desc.SampleDesc.Count  != g_ov.sample_count) ||
                         (g_ov.rtv == nullptr);
    if (!changed) return true;

    release_targets();

    g_ov.width        = desc.BufferDesc.Width;
    g_ov.height       = desc.BufferDesc.Height;
    g_ov.format       = desc.BufferDesc.Format;
    g_ov.sample_count = desc.SampleDesc.Count;

    if (g_ov.width == 0 || g_ov.height == 0) return false;

    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer)) ||
        !backbuffer) {
        hlog("GetBuffer(0) failed");
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
    switch (g_ov.format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: rtvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: rtvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default:                              rtvd.Format = g_ov.format;                break;
    }
    rtvd.ViewDimension = (g_ov.sample_count > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                                 : D3D11_RTV_DIMENSION_TEXTURE2D;

    HRESULT hr = g_ov.dev->CreateRenderTargetView(backbuffer, &rtvd, &g_ov.rtv);
    backbuffer->Release();
    if (FAILED(hr)) {
        hlog("CreateRenderTargetView failed: 0x%08lx", (unsigned long)hr);
        return false;
    }

    return true;
}

// ===============================================================
// ImGui
// ===============================================================

bool ensure_imgui()
{
    if (g_ov.imgui_ok) return true;

    IMGUI_CHECKVERSION();
    g_ov.imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ov.imgui);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;

    at_text_setup_font();

    if (!ImGui_ImplDX11_Init(g_ov.dev, g_ov.ctx)) {
        hlog("ImGui_ImplDX11_Init failed");
        ImGui::DestroyContext(g_ov.imgui);
        g_ov.imgui = nullptr;
        return false;
    }
    g_ov.imgui_ok = true;
    return true;
}

void build_ui()
{
    ImGuiIO& io = ImGui::GetIO();
    at_overlay_draw_ui("D3D11", (int)io.DisplaySize.x, (int)io.DisplaySize.y,
                       d3d11_capture::uses_gpu() ? "GPU" :
                       d3d11_capture::uses_cpu() ? "CPU" : "Waiting");
}

void draw_overlay()
{
    static int no_draw = -1;
    if (env_flag_cached("AT_OVERLAY_DISABLE_DRAW", &no_draw)) return;
    if (!g_ov.rtv || !ensure_imgui()) return;

    ImGui::SetCurrentContext(g_ov.imgui);
    if (at_text_font_dirty()) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        at_text_font_rebuilt();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_ov.width, (float)g_ov.height);
    io.DeltaTime   = frame_delta();

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    build_ui();
    ImGui::Render();

    ImDrawData* draw = ImGui::GetDrawData();
    if (!draw || draw->CmdListsCount == 0) return;
    g_ov.ctx->OMSetRenderTargets(1, &g_ov.rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(draw);
}

// ===============================================================
// The actual per-frame work, shared by Present and Present1
// ===============================================================

void on_present(IDXGISwapChain* swap)
{
    if (vk_overlay_is_active()) return;
    if (d3d12_try_present(swap)) return;
    if (d3d10_try_present(swap)) return;

    Lock lk;

    if (g_ov.swap) {
        if (swap == g_ov.swap) {
            g_swap_mismatch = 0;
        } else if (++g_swap_mismatch < kSwapMismatchLimit) {
            return;
        } else {
            shutdown_overlay();
        }
    }

    if (!ensure_device(swap))  return;
    if (!ensure_targets(swap)) return;

    draw_overlay();
}


} // namespace
void d3d11_dispatch_init() { InitializeCriticalSection(&g_lock); g_lock_ready = true; }
void d3d11_dispatch_present(IDXGISwapChain* swap) { on_present(swap); }
void d3d11_dispatch_resize() { Lock lk; safe_release(g_ov.rtv); g_ov.width = g_ov.height = 0; d3d12_on_resize(); d3d10_on_resize(); }
void d3d11_dispatch_shutdown() { if (g_lock_ready) { EnterCriticalSection(&g_lock); shutdown_overlay(); LeaveCriticalSection(&g_lock); DeleteCriticalSection(&g_lock); g_lock_ready = false; } }
} // namespace at
