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

/* D3D9 overlay renderer and hook integration */

#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

#include <detours/detours.h>

#include <stdio.h>
#include <cstring>
#include <intrin.h>

#include "win_common.h"
#include "win_capture.h"
#include "../capture/d3d/obs_capture_bridge.h"

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"

#include "overlay_d3d9.h"
#include "overlay_text.h"
#include "overlay_ui.h"

namespace at {
namespace {

struct D9State {
    IDirect3DDevice9* dev      = nullptr;
    ImGuiContext*     imgui    = nullptr;
    bool              imgui_ok = false;
    int               width    = 0;
    int               height   = 0;

};

D9State g;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

struct Lock {
    Lock()  { if (g_lock_ready) EnterCriticalSection(&g_lock); }
    ~Lock() { if (g_lock_ready) LeaveCriticalSection(&g_lock); }
};

void cap_release()
{
    d3d9_capture::free();
}

// ===============================================================
// Overlay
// ===============================================================

void shutdown_imgui()
{
    if (g.imgui_ok) {
        ImGui::SetCurrentContext(g.imgui);
        ImGui_ImplDX9_Shutdown();
        ImGui::DestroyContext(g.imgui);
        g.imgui = nullptr;
        g.imgui_ok = false;
    }
}

bool ensure_imgui()
{
    if (g.imgui_ok) return true;
    IMGUI_CHECKVERSION();
    g.imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g.imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    at_text_setup_font();

    if (!ImGui_ImplDX9_Init(g.dev)) {
        hlog("d3d9: ImGui_ImplDX9_Init failed");
        ImGui::DestroyContext(g.imgui);
        g.imgui = nullptr;
        return false;
    }
    g.imgui_ok = true;
    return true;
}

void build_ui()
{
    ImGuiIO& io = ImGui::GetIO();
    at_overlay_draw_ui("D3D9", (int)io.DisplaySize.x, (int)io.DisplaySize.y,
                       d3d9_capture::uses_gpu() ? "GPU" :
                       d3d9_capture::uses_cpu() ? "CPU" : "Waiting");
}

void draw_overlay()
{
    static int no_draw = -1;
    if (env_flag_cached("AT_OVERLAY_DISABLE_DRAW", &no_draw)) return;
    if (!ensure_imgui()) return;

    ImGui::SetCurrentContext(g.imgui);
    if (at_text_font_dirty()) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        at_text_font_rebuilt();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g.width, (float)g.height);
    io.DeltaTime   = frame_delta();

    if (g.dev->BeginScene() != D3D_OK) return;
    ImGui_ImplDX9_NewFrame();
    ImGui::NewFrame();
    build_ui();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    g.dev->EndScene();
}

// ===============================================================
// Per-present
// ===============================================================

void draw_obs_present(IDirect3DDevice9* dev)
{
    if (!dev || vk_overlay_is_active()) return;
    HookEntry guard;
    if (!guard) return;
    Lock lock;
    if (g.dev != dev) {
        shutdown_imgui();
        g.dev = dev;
    }

    IDirect3DSurface9* surface = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &surface)) || !surface) return;
    D3DSURFACE_DESC desc = {};
    if (SUCCEEDED(surface->GetDesc(&desc)) && desc.Width && desc.Height) {
        g.width = static_cast<int>(desc.Width);
        g.height = static_cast<int>(desc.Height);
        draw_overlay();
    }
    surface->Release();
}

void reset_obs_device(IDirect3DDevice9* dev)
{
    HookEntry guard;
    if (!guard) return;
    Lock lock;
    if (g.dev == dev && g.imgui_ok) {
        ImGui::SetCurrentContext(g.imgui);
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    g.width = g.height = 0;
}

} // namespace

// ===============================================================
// Public
// ===============================================================

bool hook_d3d9_install()
{
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = true; }
    const bool installed = at_obs_d3d9_install();
    hlog("d3d9: OBS hooks %s", installed ? "installed" : "failed");
    return installed;
}

void hook_d3d9_uninstall()
{
    at_obs_d3d9_uninstall();

    if (!g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    shutdown_imgui();
    cap_release();
    LeaveCriticalSection(&g_lock);
    DeleteCriticalSection(&g_lock);
    g_lock_ready = false;
}

} // namespace at

void at_obs_d3d9_present(IDirect3DDevice9* device)
{
    static volatile LONG logged_draw = 0;
    if (InterlockedCompareExchange(&logged_draw, 1, 0) == 0) {
        char line[192] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[at-overlay:%lu] d3d9-hook: AT draw callback reached (device=%p)\n",
                    GetCurrentProcessId(), device);
        at::log_write(line);
    }
    at::draw_obs_present(device);
}

void at_obs_d3d9_reset(IDirect3DDevice9* device)
{
    at::reset_obs_device(device);
}
