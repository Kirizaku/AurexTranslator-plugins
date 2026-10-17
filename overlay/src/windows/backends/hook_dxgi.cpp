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
DXGI swapchain hook + D3D11 overlay

Structure follows obs-studio/plugins/win-capture/graphics-hook
(dxgi-capture.c + d3d11-capture.c), rewritten for this project
*/

#include "win_common.h"
#include "win_capture.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <detours/detours.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

#include "overlay_text.h"
#include "overlay_ui.h"
#include "overlay_d3d12.h"
#include "overlay_d3d10.h"
#include "overlay_gl.h"
#include "overlay_d3d9.h"

namespace at {
namespace {

// ===============================================================
// Hooked function types
// ===============================================================

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT,
                                                 const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT,
                                                       UINT, DXGI_FORMAT, UINT);

PFN_Present       real_Present       = nullptr;
PFN_Present1      real_Present1      = nullptr;
PFN_ResizeBuffers real_ResizeBuffers = nullptr;

// ===============================================================
// Per-swapchain overlay state
// ===============================================================

struct OverlayD3D11 {
    IDXGISwapChain*         swap    = nullptr;
    ID3D11Device*           dev     = nullptr;
    ID3D11DeviceContext*    ctx     = nullptr;
    ID3D11RenderTargetView* rtv     = nullptr;

    /* Capture */
    ID3D11Texture2D* staging = nullptr;
    ID3D11Texture2D* resolve = nullptr;

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

thread_local bool g_in_present = false;

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

DXGI_FORMAT strip_srgb(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:                              return f;
    }
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

bool init_staging_texture()
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = g_ov.width;
    td.Height           = g_ov.height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = strip_srgb(g_ov.format);
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_STAGING;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    HRESULT hr = g_ov.dev->CreateTexture2D(&td, nullptr, &g_ov.staging);
    if (FAILED(hr)) {
        hlog("CreateTexture2D(staging) failed: 0x%08lx", (unsigned long)hr);
        return false;
    }

    if (g_ov.sample_count > 1) {
        D3D11_TEXTURE2D_DESC rtd = td;
        rtd.Usage          = D3D11_USAGE_DEFAULT;
        rtd.CPUAccessFlags = 0;
        rtd.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_ov.dev->CreateTexture2D(&rtd, nullptr, &g_ov.resolve))) {
            hlog("CreateTexture2D(resolve) failed");
            safe_release(g_ov.staging);
            return false;
        }
    }

    capture_frame_capture_started();
    return true;
}

bool copy_backbuffer(IDXGISwapChain* swap)
{
    if (!g_ov.staging || !format_supported(strip_srgb(g_ov.format))) return false;

    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer)) ||
        !backbuffer) {
        return false;
    }

    if (g_ov.sample_count > 1) {
        g_ov.ctx->ResolveSubresource(g_ov.resolve, 0, backbuffer, 0, strip_srgb(g_ov.format));
        g_ov.ctx->CopyResource(g_ov.staging, g_ov.resolve);
    } else {
        g_ov.ctx->CopyResource(g_ov.staging, backbuffer);
    }
    backbuffer->Release();

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(g_ov.ctx->Map(g_ov.staging, 0, D3D11_MAP_READ, 0, &map))) return false;
    if (strip_srgb(g_ov.format) == DXGI_FORMAT_R8G8B8A8_UNORM)
        swizzle_rgba_to_bgra(map.pData, (int)g_ov.width, (int)g_ov.height, (int)map.RowPitch);
    const bool ok = capture_write_frame((int)g_ov.width, (int)g_ov.height, map.pData, (int)map.RowPitch);
    g_ov.ctx->Unmap(g_ov.staging, 0);
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
    at_overlay_draw_ui("D3D11", (int)io.DisplaySize.x, (int)io.DisplaySize.y);
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

    capture_update();

    if (capture_should_stop()) {
        safe_release(g_ov.staging);
        safe_release(g_ov.resolve);
        capture_stop();
    }
    if (capture_should_init()) {
        if (!init_staging_texture()) {
            capture_stop();
        }
    }

    if (capture_ready() && g_ov.staging && capture_frame_requested()) {
        if (copy_backbuffer(swap)) capture_frame_delivered();
    }

    draw_overlay();
}

// ===============================================================
// Detours
// ===============================================================

HRESULT STDMETHODCALLTYPE hook_Present(IDXGISwapChain* swap, UINT interval, UINT flags)
{
    HookEntry guard;
    if (guard && !g_in_present) {
        g_in_present = true;
        if (!(flags & DXGI_PRESENT_TEST)) {
            on_present(swap);
        }
        g_in_present = false;
    }
    return real_Present(swap, interval, flags);
}

HRESULT STDMETHODCALLTYPE hook_Present1(IDXGISwapChain1* swap, UINT interval,
                                        UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    HookEntry guard;
    if (guard && !g_in_present) {
        g_in_present = true;
        if (!(flags & DXGI_PRESENT_TEST)) {
            on_present((IDXGISwapChain*)swap);
        }
        g_in_present = false;
    }
    return real_Present1(swap, interval, flags, pp);
}

HRESULT STDMETHODCALLTYPE hook_ResizeBuffers(IDXGISwapChain* swap, UINT count,
                                             UINT w, UINT h, DXGI_FORMAT fmt,
                                             UINT flags)
{
    HookEntry guard;
    if (guard) {
        Lock lk;
        release_targets();
        g_ov.width = g_ov.height = 0;
    }
    d3d12_on_resize();
    d3d10_on_resize();
    const HRESULT hr = real_ResizeBuffers(swap, count, w, h, fmt, flags);
    if (FAILED(hr)) {
        hlog("dxgi: real_ResizeBuffers failed: 0x%08lx (count=%u %ux%u fmt=%d flags=%u)",
             (unsigned long)hr, count, w, h, (int)fmt, flags);
    }
    return hr;
}

// ===============================================================
// Vtable discovery
// ===============================================================

enum : int {
    VT_PRESENT        = 8,
    VT_RESIZE_BUFFERS = 13,
    VT_PRESENT1       = 22,
};

HMODULE module_containing(void* addr)
{
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)addr, &mod);
    return mod;
}

bool offsets_cache_path(char* buf, size_t buflen)
{
    char appdata[MAX_PATH] = {0};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", appdata, sizeof(appdata));
    if (n == 0 || n >= sizeof(appdata)) return false;
    _snprintf_s(buf, buflen, _TRUNCATE, "%s\\AurexTranslator\\plugins\\at-overlay-offsets.txt", appdata);
    return true;
}

bool module_identity(HMODULE mod, char* out, size_t outlen)
{
    char path[MAX_PATH] = {0};
    if (GetModuleFileNameA(mod, path, sizeof(path)) == 0) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return false;
    _snprintf_s(out, outlen, _TRUNCATE, "%08lx%08lx%08lx%08lx",
                fad.nFileSizeHigh, fad.nFileSizeLow,
                fad.ftLastWriteTime.dwHighDateTime, fad.ftLastWriteTime.dwLowDateTime);
    return true;
}

bool load_cached_offset(const char* identity, const char* slot, ptrdiff_t* out_offset)
{
    char path[MAX_PATH];
    if (!offsets_cache_path(path, sizeof(path))) return false;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return false;

    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char lid[128] = {0}, lslot[32] = {0};
        long long off = 0;
        if (sscanf_s(line, "%127[^;];%31[^;];%lld", lid, (unsigned)sizeof(lid),
                     lslot, (unsigned)sizeof(lslot), &off) == 3) {
            if (strcmp(lid, identity) == 0 && strcmp(lslot, slot) == 0) {
                *out_offset = (ptrdiff_t)off;
                found = true;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

void save_cached_offset(const char* identity, const char* slot, ptrdiff_t offset)
{
    char path[MAX_PATH];
    if (!offsets_cache_path(path, sizeof(path))) return;

    ptrdiff_t existing = 0;
    if (load_cached_offset(identity, slot, &existing)) return; /* already have it */

    char dir[MAX_PATH];
    if (offsets_cache_path(dir, sizeof(dir))) {
        char* slash = strrchr(dir, '\\');
        if (slash) { *slash = 0; CreateDirectoryA(dir, nullptr); }
    }

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    fprintf(f, "%s;%s;%lld\n", identity, slot, (long long)offset);
    fclose(f);
    hlog("dxgi: cached %s offset 0x%llx for future (possibly hijacked) captures", slot, (long long)offset);
}

void* resolve_trustworthy(void* raw, HMODULE dxgiMod, const char* identity, const char* slot)
{
    if (!raw) return raw;
    if (module_containing(raw) == dxgiMod) {
        save_cached_offset(identity, slot, (uintptr_t)raw - (uintptr_t)dxgiMod);
        return raw;
    }

    ptrdiff_t cached = 0;
    if (load_cached_offset(identity, slot, &cached)) {
        hlog("dxgi: %s vtable slot already hijacked by another overlay - using cached offset instead", slot);
        return (void*)((uintptr_t)dxgiMod + cached);
    }

    hlog("dxgi: %s vtable slot already hijacked by another overlay and no cached offset yet - "
         "hooking it anyway (best effort, same as before this build)", slot);
    return raw;
}

bool capture_vtable(void** out_present, void** out_resize, void** out_present1)
{
    *out_present = *out_resize = *out_present1 = nullptr;

    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "at_overlay_dummy";
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        hlog("RegisterClassExA failed: %lu", GetLastError());
        return false;
    }

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "at-overlay", WS_OVERLAPPEDWINDOW,
                                0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        hlog("CreateWindowExA failed: %lu", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount       = 1;
    sd.BufferDesc.Width  = 2;
    sd.BufferDesc.Height = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = hwnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0,
                                         D3D_FEATURE_LEVEL_10_1,
                                         D3D_FEATURE_LEVEL_10_0 };

    IDXGISwapChain*      swap = nullptr;
    ID3D11Device*        dev  = nullptr;
    ID3D11DeviceContext* ctx  = nullptr;
    D3D_FEATURE_LEVEL    got  = {};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, (UINT)(sizeof(levels) / sizeof(levels[0])),
        D3D11_SDK_VERSION, &sd, &swap, &dev, &got, &ctx);

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, (UINT)(sizeof(levels) / sizeof(levels[0])),
            D3D11_SDK_VERSION, &sd, &swap, &dev, &got, &ctx);
    }

    if (FAILED(hr) || !swap) {
        hlog("dummy D3D11CreateDeviceAndSwapChain failed: 0x%08lx", (unsigned long)hr);
        DestroyWindow(hwnd);
        return false;
    }

    HMODULE dxgiMod = GetModuleHandleA("dxgi.dll");
    char identity[128] = {0};
    const bool haveIdentity = dxgiMod && module_identity(dxgiMod, identity, sizeof(identity));

    void** vt = *(void***)swap;
    void* rawPresent = vt[VT_PRESENT];
    void* rawResize  = vt[VT_RESIZE_BUFFERS];
    void* rawPresent1 = nullptr;
    IDXGISwapChain1* swap1 = nullptr;

    if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&swap1)) && swap1) {
        void** vt1 = *(void***)swap1;
        rawPresent1 = vt1[VT_PRESENT1];
        swap1->Release();
    }

    if (dxgiMod && haveIdentity) {
        *out_present  = resolve_trustworthy(rawPresent,  dxgiMod, identity, "Present");
        *out_resize   = resolve_trustworthy(rawResize,   dxgiMod, identity, "ResizeBuffers");
        *out_present1 = rawPresent1
            ? resolve_trustworthy(rawPresent1, dxgiMod, identity, "Present1")
            : nullptr;
    } else {
        *out_present  = rawPresent;
        *out_resize   = rawResize;
        *out_present1 = rawPresent1;
    }

    safe_release(ctx);
    safe_release(dev);
    safe_release(swap);
    DestroyWindow(hwnd);
    return true;
}

} // namespace

// ===============================================================
// Public entry points
// ===============================================================

bool hook_dxgi_install()
{
    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;
    bool any = false;
    void *pPresent = nullptr, *pResize = nullptr, *pPresent1 = nullptr;
    if (capture_vtable(&pPresent, &pResize, &pPresent1)) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (pPresent) {
            real_Present = (PFN_Present)pPresent;
            if (DetourAttach(&(PVOID&)real_Present, (PVOID)hook_Present) == NO_ERROR) {
                any = true;
            } else {
                hlog("failed to hook IDXGISwapChain::Present");
                real_Present = nullptr;
            }
        }
        if (pResize) {
            real_ResizeBuffers = (PFN_ResizeBuffers)pResize;
            if (DetourAttach(&(PVOID&)real_ResizeBuffers, (PVOID)hook_ResizeBuffers) != NO_ERROR) {
                hlog("failed to hook IDXGISwapChain::ResizeBuffers");
                real_ResizeBuffers = nullptr;
            }
        }
        if (pPresent1) {
            real_Present1 = (PFN_Present1)pPresent1;
            if (DetourAttach(&(PVOID&)real_Present1, (PVOID)hook_Present1) == NO_ERROR) {
                any = true;
            } else {
                hlog("failed to hook IDXGISwapChain1::Present1");
                real_Present1 = nullptr;
            }
        }

        if (DetourTransactionCommit() != NO_ERROR) {
            hlog("DetourTransactionCommit failed for dxgi hooks");
            any = false;
            real_Present = nullptr;
            real_ResizeBuffers = nullptr;
            real_Present1 = nullptr;
        }

        if (any) d3d12_hook_queue();
    } else {
        hlog("no DXGI device — trying OpenGL only");
    }

    bool anyGl = hook_gl_install();
    bool anyD9 = hook_d3d9_install();

    if (!any && !anyGl && !anyD9) {
        return false;
    }

    capture_init();
    hlog("hooks installed (dxgi Present=%p Present1=%p ResizeBuffers=%p, gl=%d, d3d9=%d)",
         pPresent, pPresent1, pResize, (int)anyGl, (int)anyD9);
    return true;
}

void hook_dxgi_uninstall()
{
    if (!wait_for_hooks_idle()) {
        hlog("wait_for_hooks_idle timed out - a hook may still be running, unloading anyway");
    }

    d3d12_shutdown();
    d3d10_shutdown();
    hook_gl_uninstall();
    hook_d3d9_uninstall();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (real_Present)       DetourDetach(&(PVOID&)real_Present, (PVOID)hook_Present);
    if (real_ResizeBuffers) DetourDetach(&(PVOID&)real_ResizeBuffers, (PVOID)hook_ResizeBuffers);
    if (real_Present1)      DetourDetach(&(PVOID&)real_Present1, (PVOID)hook_Present1);
    DetourTransactionCommit();
    real_Present = nullptr;
    real_ResizeBuffers = nullptr;
    real_Present1 = nullptr;

    if (g_lock_ready) {
        EnterCriticalSection(&g_lock);
        shutdown_overlay();
        LeaveCriticalSection(&g_lock);
        DeleteCriticalSection(&g_lock);
        g_lock_ready = false;
    }

    capture_shutdown();
}

} // namespace at
