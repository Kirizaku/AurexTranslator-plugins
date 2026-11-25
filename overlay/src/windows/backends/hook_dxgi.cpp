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

/* DXGI Present/Resize hooks; API capture and drawing are dispatched to the
   D3D10, D3D11, D3D12, OpenGL, and D3D9 backends. */

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <detours/detours.h>

#include "win_common.h"
#include "win_capture.h"
#include "../capture/d3d/obs_capture_bridge.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

#include "overlay_text.h"
#include "overlay_ui.h"
#include "overlay_d3d12.h"
#include "overlay_d3d10.h"
#include "overlay_d3d11.h"
#include "overlay_gl.h"
#include "overlay_d3d9.h"

namespace at {
namespace {

// ===============================================================
// Hooked function types
// ===============================================================

typedef HRESULT(STDMETHODCALLTYPE* PFN_SetFullscreenState)(IDXGISwapChain*, BOOL,
                                                            IDXGIOutput*);

PFN_SetFullscreenState real_SetFullscreenState = nullptr;

// ===============================================================
// Detours
// ===============================================================

HRESULT STDMETHODCALLTYPE hook_SetFullscreenState(IDXGISwapChain* swap, BOOL fullscreen,
                                                    IDXGIOutput* target)
{
    HookEntry guard;
    if (guard && !fullscreen) d3d12_on_swapchain_shutdown(swap);
    return real_SetFullscreenState(swap, fullscreen, target);
}

// ===============================================================
// Vtable discovery
// ===============================================================

enum : int {
    VT_PRESENT        = 8,
    VT_SET_FULLSCREEN = 10,
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

bool capture_vtable(void** out_present, void** out_resize, void** out_present1,
                    void** out_set_fullscreen)
{
    *out_present = *out_resize = *out_present1 = *out_set_fullscreen = nullptr;

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
    void* rawSetFullscreen = vt[VT_SET_FULLSCREEN];
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
        *out_set_fullscreen = resolve_trustworthy(rawSetFullscreen, dxgiMod, identity,
                                                  "SetFullscreenState");
        *out_resize   = resolve_trustworthy(rawResize,   dxgiMod, identity, "ResizeBuffers");
        *out_present1 = rawPresent1
            ? resolve_trustworthy(rawPresent1, dxgiMod, identity, "Present1")
            : nullptr;
    } else {
        *out_present  = rawPresent;
        *out_set_fullscreen = rawSetFullscreen;
        *out_resize   = rawResize;
        *out_present1 = rawPresent1;
    }

    safe_release(ctx);
    safe_release(dev);
    safe_release(swap);
    DestroyWindow(hwnd);
    return true;
}

thread_local bool g_in_present = false;

void dxgi_obs_present(IDXGISwapChain* swap)
{
    HookEntry guard;
    if (!guard || g_in_present) return;
    g_in_present = true;
    if (!vk_overlay_is_active()) d3d11_dispatch_present(swap);
    g_in_present = false;
}

void dxgi_obs_resize()
{
    HookEntry guard;
    if (!guard) return;
    d3d11_dispatch_resize();
}

} // namespace

// ===============================================================
// Public entry points
// ===============================================================

bool hook_dxgi_install()
{
    d3d11_dispatch_init();
    bool any = false;
    void *pPresent = nullptr, *pResize = nullptr, *pPresent1 = nullptr;
    void *pSetFullscreen = nullptr;
    if (capture_vtable(&pPresent, &pResize, &pPresent1, &pSetFullscreen)) {
        any = at_obs_dxgi_hook_install(pPresent, pResize, pPresent1);
        if (any && pSetFullscreen) {
            real_SetFullscreenState = reinterpret_cast<PFN_SetFullscreenState>(pSetFullscreen);
            if (DetourTransactionBegin() == NO_ERROR) {
                DetourUpdateThread(GetCurrentThread());
                const LONG attach = DetourAttach(reinterpret_cast<PVOID *>(&real_SetFullscreenState), hook_SetFullscreenState);
                const LONG commit = DetourTransactionCommit();
                if (attach != NO_ERROR || commit != NO_ERROR) real_SetFullscreenState = nullptr;
            } else {
                real_SetFullscreenState = nullptr;
            }
        }
        if (any) d3d12_hook_queue();
    } else {
        hlog("no DXGI device â€” trying OpenGL only");
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
    if (real_SetFullscreenState && DetourTransactionBegin() == NO_ERROR) {
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(reinterpret_cast<PVOID *>(&real_SetFullscreenState), hook_SetFullscreenState);
        if (DetourTransactionCommit() == NO_ERROR) real_SetFullscreenState = nullptr;
    }
    if (!wait_for_hooks_idle()) {
        hlog("wait_for_hooks_idle timed out - a hook may still be running, unloading anyway");
    }

    d3d12_shutdown();
    d3d10_shutdown();
    hook_gl_uninstall();
    hook_d3d9_uninstall();

    d3d11_dispatch_shutdown();

    capture_shutdown();
}

} // namespace at

void at_obs_dxgi_render(IDXGISwapChain* swap) { at::dxgi_obs_present(swap); }
void at_obs_dxgi_on_resize(IDXGISwapChain*) { at::dxgi_obs_resize(); }
