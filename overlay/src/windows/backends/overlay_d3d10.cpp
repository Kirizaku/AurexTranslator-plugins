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

/*
D3D10 swapchain overlay + capture

Structure parallels obs-studio/plugins/win-capture/graphics-hook
(d3d10-capture.c), rewritten for this project.
*/

#include "win_common.h"
#include "win_capture.h"
#include "overlay_d3d10.h"
#include "overlay_text.h"
#include "overlay_ui.h"

#include <d3d10.h>
#include <dxgi.h>

#include "imgui.h"
#include "backends/imgui_impl_dx10.h"

namespace at {
namespace {

struct OverlayD3D10 {
    IDXGISwapChain*         swap = nullptr; 
    ID3D10Device*           dev  = nullptr; /* device == immediate context */
    ID3D10RenderTargetView* rtv  = nullptr;

    /* Capture */
    ID3D10Texture2D* staging = nullptr;
    ID3D10Texture2D* resolve = nullptr;

    UINT        width  = 0;
    UINT        height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT        sample_count = 1;

    ImGuiContext* imgui    = nullptr;
    bool          imgui_ok = false;
};

OverlayD3D10 g_ov;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

struct Lock {
    Lock()  { if (g_lock_ready) EnterCriticalSection(&g_lock); }
    ~Lock() { if (g_lock_ready) LeaveCriticalSection(&g_lock); }
};

thread_local bool g_in_present = false;

DXGI_FORMAT rtv_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:                              return f;
    }
}

// ===============================================================
// Teardown
// ===============================================================

void release_targets()
{
    safe_release(g_ov.rtv);
    safe_release(g_ov.staging);
    safe_release(g_ov.resolve);
    capture_stop();
}

void shutdown_overlay()
{
    if (g_ov.imgui_ok) {
        ImGui::SetCurrentContext(g_ov.imgui);
        ImGui_ImplDX10_Shutdown();
        ImGui::DestroyContext(g_ov.imgui);
        g_ov.imgui    = nullptr;
        g_ov.imgui_ok = false;
    }
    release_targets();
    safe_release(g_ov.dev);
    g_ov.swap   = nullptr;
    g_ov.width  = 0;
    g_ov.height = 0;
    g_ov.format = DXGI_FORMAT_UNKNOWN;
}

// ===============================================================
// Setup
// ===============================================================

bool ensure_device(IDXGISwapChain* swap)
{
    if (g_ov.swap == swap && g_ov.dev) return true;
    shutdown_overlay();

    ID3D10Device* dev = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&dev)) || !dev)
        return false;

    g_ov.swap = swap;
    g_ov.dev  = dev;
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

    ID3D10Texture2D* backbuffer = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&backbuffer)) ||
        !backbuffer) {
        hlog("d3d10: GetBuffer(0) failed");
        return false;
    }

    D3D10_RENDER_TARGET_VIEW_DESC rtvd = {};
    rtvd.Format        = rtv_format(g_ov.format);
    rtvd.ViewDimension = (g_ov.sample_count > 1) ? D3D10_RTV_DIMENSION_TEXTURE2DMS
                                                 : D3D10_RTV_DIMENSION_TEXTURE2D;

    HRESULT hr = g_ov.dev->CreateRenderTargetView(backbuffer, &rtvd, &g_ov.rtv);
    backbuffer->Release();
    if (FAILED(hr)) {
        hlog("d3d10: CreateRenderTargetView failed: 0x%08lx", (unsigned long)hr);
        return false;
    }
    return true;
}

bool init_staging_texture()
{
    D3D10_TEXTURE2D_DESC td = {};
    td.Width            = g_ov.width;
    td.Height           = g_ov.height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = rtv_format(g_ov.format);
    td.SampleDesc.Count = 1;
    td.Usage            = D3D10_USAGE_STAGING;
    td.CPUAccessFlags   = D3D10_CPU_ACCESS_READ;

    HRESULT hr = g_ov.dev->CreateTexture2D(&td, nullptr, &g_ov.staging);
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) { logged = true; hlog("d3d10: CreateTexture2D(staging) failed: 0x%08lx", (unsigned long)hr); }
        return false;
    }

    if (g_ov.sample_count > 1) {
        D3D10_TEXTURE2D_DESC rtd = td;
        rtd.Usage          = D3D10_USAGE_DEFAULT;
        rtd.CPUAccessFlags = 0;
        rtd.BindFlags      = D3D10_BIND_SHADER_RESOURCE | D3D10_BIND_RENDER_TARGET;
        if (FAILED(g_ov.dev->CreateTexture2D(&rtd, nullptr, &g_ov.resolve))) {
            hlog("d3d10: CreateTexture2D(resolve) failed");
            safe_release(g_ov.staging);
            return false;
        }
    }

    capture_frame_capture_started();
    return true;
}

bool format_supported(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return true;
    default:
        return false;
    }
}

bool copy_backbuffer(IDXGISwapChain* swap)
{
    if (!g_ov.staging || !format_supported(rtv_format(g_ov.format))) return false;

    ID3D10Texture2D* backbuffer = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&backbuffer)) ||
        !backbuffer) {
        return false;
    }

    if (g_ov.sample_count > 1) {
        g_ov.dev->ResolveSubresource(g_ov.resolve, 0, backbuffer, 0, rtv_format(g_ov.format));
        g_ov.dev->CopyResource(g_ov.staging, g_ov.resolve);
    } else {
        g_ov.dev->CopyResource(g_ov.staging, backbuffer);
    }
    backbuffer->Release();

    D3D10_MAPPED_TEXTURE2D map = {};
    if (FAILED(g_ov.staging->Map(0, D3D10_MAP_READ, 0, &map))) return false;
    if (rtv_format(g_ov.format) == DXGI_FORMAT_R8G8B8A8_UNORM)
        swizzle_rgba_to_bgra(map.pData, (int)g_ov.width, (int)g_ov.height, (int)map.RowPitch);
    const bool ok = capture_write_frame((int)g_ov.width, (int)g_ov.height, map.pData, (int)map.RowPitch);
    g_ov.staging->Unmap(0);
    return ok;
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

    if (!ImGui_ImplDX10_Init(g_ov.dev)) {
        hlog("d3d10: ImGui_ImplDX10_Init failed");
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
    at_overlay_draw_ui("D3D10", (int)io.DisplaySize.x, (int)io.DisplaySize.y);
}

void draw_overlay()
{
    static int no_draw = -1;
    if (env_flag_cached("AT_OVERLAY_DISABLE_DRAW", &no_draw)) return;
    if (!g_ov.rtv || !ensure_imgui()) return;

    ImGui::SetCurrentContext(g_ov.imgui);
    if (at_text_font_dirty()) {
        ImGui_ImplDX10_InvalidateDeviceObjects();
        at_text_font_rebuilt();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_ov.width, (float)g_ov.height);
    io.DeltaTime   = frame_delta();

    ImGui_ImplDX10_NewFrame();
    ImGui::NewFrame();
    build_ui();
    ImGui::Render();

    ImDrawData* draw = ImGui::GetDrawData();
    if (!draw || draw->CmdListsCount == 0) return;

    g_ov.dev->OMSetRenderTargets(1, &g_ov.rtv, nullptr);
    ImGui_ImplDX10_RenderDrawData(draw);
}

// ===============================================================
// Per-present
// ===============================================================

void on_present(IDXGISwapChain* swap)
{
    if (!ensure_device(swap))  return;
    if (!ensure_targets(swap)) return;

    capture_update();

    if (capture_should_stop()) {
        safe_release(g_ov.staging);
        safe_release(g_ov.resolve);
        capture_stop();
    }
    if (capture_should_init()) {
        if (!init_staging_texture()) capture_stop();
    }
    if (capture_ready() && g_ov.staging && capture_frame_requested()) {
        if (copy_backbuffer(swap)) capture_frame_delivered();
    }

    draw_overlay();
}

bool is_d3d10(IDXGISwapChain* swap)
{
    ID3D10Device* d = nullptr;
    if (SUCCEEDED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&d)) && d) {
        d->Release();
        return true;
    }
    return false;
}

} // namespace

// ===============================================================
// Public
// ===============================================================

bool d3d10_try_present(IDXGISwapChain* swap)
{
    if (!is_d3d10(swap)) return false;

    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = true; }

    if (!g_in_present) {
        g_in_present = true;
        {
            Lock lk;
            on_present(swap);
        }
        g_in_present = false;
    }
    return true;
}

void d3d10_on_resize()
{
    if (!g_lock_ready) return;
    Lock lk;
    release_targets();
    g_ov.width = g_ov.height = 0;
}

void d3d10_shutdown()
{
    if (g_lock_ready) {
        EnterCriticalSection(&g_lock);
        shutdown_overlay();
        LeaveCriticalSection(&g_lock);
        DeleteCriticalSection(&g_lock);
        g_lock_ready = false;
    }
}

} // namespace at
