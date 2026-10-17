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
Direct3D9 overlay + capture on Windows

Structure parallels obs-studio/plugins/win-capture/graphics-hook (d3d9-capture.c),
rewritten for this project.
*/

#include "win_common.h"
#include "win_capture.h"
#include "overlay_d3d9.h"
#include "overlay_text.h"
#include "overlay_ui.h"

#include <d3d9.h>
#include <vector>

#include <detours/detours.h>
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"

namespace at {
namespace {

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

PFN_Present real_Present = nullptr;
PFN_Reset   real_Reset   = nullptr;

struct D9State {
    IDirect3DDevice9* dev      = nullptr;
    ImGuiContext*     imgui    = nullptr;
    bool              imgui_ok = false;
    int               width    = 0;
    int               height   = 0;

    /* Capture */
    IDirect3DSurface9* sysmem = nullptr; /* GetRenderTargetData target */
    int                cap_w  = 0;
    int                cap_h  = 0;
};

D9State g;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
thread_local bool g_in_present = false;

struct Lock {
    Lock()  { if (g_lock_ready) EnterCriticalSection(&g_lock); }
    ~Lock() { if (g_lock_ready) LeaveCriticalSection(&g_lock); }
};

// ===============================================================
// Capture
// ===============================================================

void cap_release()
{
    safe_release(g.sysmem);
    g.cap_w = g.cap_h = 0;
    capture_stop();
}

bool cap_init(int w, int h)
{
    g.cap_w = w; g.cap_h = h;
    capture_frame_capture_started();
    return true;
}

bool cap_copy()
{
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(g.dev->GetRenderTarget(0, &rt)) || !rt) return false;
    D3DSURFACE_DESC rd = {};
    rt->GetDesc(&rd);

    if (rd.Format != D3DFMT_X8R8G8B8 && rd.Format != D3DFMT_A8R8G8B8) {
        rt->Release();
        return false;
    }

    if (!g.sysmem &&
        FAILED(g.dev->CreateOffscreenPlainSurface(rd.Width, rd.Height, rd.Format, D3DPOOL_SYSTEMMEM, &g.sysmem, nullptr))) {
        hlog("d3d9: CreateOffscreenPlainSurface failed");
        rt->Release();
        return false;
    }

    HRESULT hr = g.dev->GetRenderTargetData(rt, g.sysmem);
    rt->Release();
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) { logged = true; hlog("d3d9: GetRenderTargetData failed: 0x%08lx", (unsigned long)hr); }
        return false;
    }

    D3DLOCKED_RECT lr = {};
    if (FAILED(g.sysmem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
    const bool ok = capture_write_frame((int)rd.Width, (int)rd.Height, lr.pBits, (int)lr.Pitch);
    g.sysmem->UnlockRect();
    return ok;
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
    at_overlay_draw_ui("D3D9", (int)io.DisplaySize.x, (int)io.DisplaySize.y);
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

void on_present(IDirect3DDevice9* dev)
{
    if (vk_overlay_is_active()) return;

    if (g.dev != dev) { shutdown_imgui(); cap_release(); g.dev = dev; }

    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    D3DSURFACE_DESC sd = {};
    rt->GetDesc(&sd);
    rt->Release();
    const int w = (int)sd.Width, h = (int)sd.Height;
    if (w <= 0 || h <= 0) return;
    g.width = w; g.height = h;

    capture_update();

    if (capture_should_stop()) cap_release();
    if (capture_ready() && (g.cap_w != w || g.cap_h != h)) cap_release();
    if (capture_should_init()) {
        if (!cap_init(w, h)) { cap_release(); capture_stop(); }
    }
    if (capture_ready() && capture_frame_requested()) {
        if (cap_copy()) capture_frame_delivered();
    }

    draw_overlay();
}

HRESULT STDMETHODCALLTYPE hook_Present(IDirect3DDevice9* dev, const RECT* src,
                                       const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    HookEntry guard;
    if (guard && !g_in_present) {
        g_in_present = true;
        { Lock lk; on_present(dev); }
        g_in_present = false;
    }
    return real_Present(dev, src, dst, wnd, dirty);
}

HRESULT STDMETHODCALLTYPE hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    HookEntry guard;
    if (guard) {
        Lock lk;
        if (g.imgui_ok) { ImGui::SetCurrentContext(g.imgui); ImGui_ImplDX9_InvalidateDeviceObjects(); }
        cap_release();
        g.width = g.height = 0;
    }
    return real_Reset(dev, pp);
}

// ===============================================================
// Vtable discovery
// ===============================================================

bool capture_vtable(void** out_present, void** out_reset)
{
    *out_present = *out_reset = nullptr;

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { hlog("d3d9: Direct3DCreate9 failed"); return false; }

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "at_overlay_d3d9_dummy";
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        d3d->Release();
        return false;
    }
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "at-overlay", WS_OVERLAPPEDWINDOW,
                                0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { d3d->Release(); return false; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth  = 2;
    pp.BackBufferHeight = 2;
    pp.BackBufferCount  = 1;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.hDeviceWindow    = hwnd;

    struct { D3DDEVTYPE type; DWORD flags; } tries[] = {
        { D3DDEVTYPE_HAL, D3DCREATE_HARDWARE_VERTEXPROCESSING },
        { D3DDEVTYPE_HAL, D3DCREATE_SOFTWARE_VERTEXPROCESSING },
        { D3DDEVTYPE_REF, D3DCREATE_SOFTWARE_VERTEXPROCESSING },
    };

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = D3DERR_NOTAVAILABLE;
    for (auto& t : tries) {
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, t.type, hwnd, t.flags, &pp, &dev);
        if (SUCCEEDED(hr) && dev) break;
    }

    if (FAILED(hr) || !dev) {
        hlog("d3d9: dummy CreateDevice failed: 0x%08lx", (unsigned long)hr);
        DestroyWindow(hwnd);
        d3d->Release();
        return false;
    }

    void** vt = *(void***)dev;
    *out_reset = vt[16]; /* IDirect3DDevice9::Reset */
    *out_present = vt[17]; /* IDirect3DDevice9::Present */

    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return true;
}

} // namespace

// ===============================================================
// Public
// ===============================================================

bool hook_d3d9_install()
{
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) return false;

    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = true; }

    void *pPresent = nullptr, *pReset = nullptr;
    if (!capture_vtable(&pPresent, &pReset)) return false;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    bool any = false;
    if (pPresent) {
        real_Present = (PFN_Present)pPresent;
        if (DetourAttach(&(PVOID&)real_Present, (PVOID)hook_Present) == NO_ERROR) {
            any = true;
        } else {
            real_Present = nullptr;
        }
    }
    if (pReset) {
        real_Reset = (PFN_Reset)pReset;
        if (DetourAttach(&(PVOID&)real_Reset, (PVOID)hook_Reset) != NO_ERROR) {
            real_Reset = nullptr;
        }
    }

    if (DetourTransactionCommit() != NO_ERROR) {
        hlog("d3d9: DetourTransactionCommit failed");
        real_Present = nullptr;
        real_Reset = nullptr;
        any = false;
    }

    if (any) hlog("d3d9: hooks created (Present=%p Reset=%p)", pPresent, pReset);
    return any;
}

void hook_d3d9_uninstall()
{
    if (real_Present || real_Reset) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        if (real_Present) DetourDetach(&(PVOID&)real_Present, (PVOID)hook_Present);
        if (real_Reset)   DetourDetach(&(PVOID&)real_Reset, (PVOID)hook_Reset);
        DetourTransactionCommit();
        real_Present = nullptr;
        real_Reset = nullptr;
    }

    if (!g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    shutdown_imgui();
    cap_release();
    LeaveCriticalSection(&g_lock);
    DeleteCriticalSection(&g_lock);
    g_lock_ready = false;
}

} // namespace at
