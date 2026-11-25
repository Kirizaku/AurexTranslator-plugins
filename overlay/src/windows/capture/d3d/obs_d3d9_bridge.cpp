/******************************************************************************
    at-overlay - in-game overlay for AurexTranslator

    Copyright (C) 2026 by Daniil Nabiulin

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

#include "win_capture.h"
#include "obs_capture_bridge.h"
#include "win_common.h"
#include <detours.h>
#undef hlog

// Route OBS' capture-stop predicate through the AT adapter so it can also draw
// the overlay from the OBS-owned Present hook

#include "graphics-hook.h"
bool at_obs_d3d9_capture_should_stop();
void at_obs_d3d9_capture_free();
bool at_obs_d3d9_capture_init_shmem(shmem_data **out, HWND window,
                                    uint32_t width, uint32_t height,
                                    uint32_t pitch, uint32_t format, bool flip_y);
#undef capture_should_stop
#define capture_should_stop at_obs_d3d9_capture_should_stop
#define capture_free at_obs_d3d9_capture_free
#define capture_init_shmem at_obs_d3d9_capture_init_shmem
static LONG WINAPI at_d3d9_detour_attach(PVOID *target, PVOID detour);
#define DetourAttach at_d3d9_detour_attach

// Compile the unmodified OBS source in this translation unit. AT supplies the
// capture transport and the small callbacks used by the D3D9 overlay renderer

#include "../../../../third_party/obs-studio/graphics-hook/d3d9-capture.cpp"
#undef DetourAttach
#undef capture_should_stop
#undef capture_free
#undef capture_init_shmem
#undef capture_should_init
#undef capture_ready

static HRESULT STDMETHODCALLTYPE at_present(IDirect3DDevice9 *device, CONST RECT *src, CONST RECT *dst, HWND window, CONST RGNDATA *dirty)
{
    IDirect3DSurface9 *backbuffer = nullptr;
    if (!hooked_reset) setup_reset_hooks(device);
    const bool first = present_recurse == 0;
    present_begin(device, backbuffer);
    if (first) at_obs_d3d9_present(device);
    const HRESULT result = RealPresent(device, src, dst, window, dirty);
    present_end(device, backbuffer);
    return result;
}

static HRESULT STDMETHODCALLTYPE at_present_ex(IDirect3DDevice9Ex *device, CONST RECT *src, CONST RECT *dst, HWND window, CONST RGNDATA *dirty, DWORD flags)
{
    IDirect3DSurface9 *backbuffer = nullptr;
    if (!hooked_reset) setup_reset_hooks(device);
    const bool first = present_recurse == 0;
    present_begin(device, backbuffer);
    if (first) at_obs_d3d9_present(device);
    const HRESULT result = RealPresentEx(device, src, dst, window, dirty, flags);
    present_end(device, backbuffer);
    return result;
}

static HRESULT STDMETHODCALLTYPE at_present_swap(IDirect3DSwapChain9 *swap, CONST RECT *src, CONST RECT *dst, HWND window, CONST RGNDATA *dirty, DWORD flags)
{
    IDirect3DSurface9 *backbuffer = nullptr;
    IDirect3DDevice9 *device = nullptr;
    if (!present_recurse) swap->GetDevice(&device);
    if (device) {
        if (!hooked_reset) setup_reset_hooks(device);
        present_begin(device, backbuffer);
        at_obs_d3d9_present(device);
    }
    const HRESULT result = RealPresentSwap(swap, src, dst, window, dirty, flags);
    if (device) { present_end(device, backbuffer); device->Release(); }
    return result;
}

static LONG WINAPI at_d3d9_detour_attach(PVOID *target, PVOID detour)
{
    if (detour == reinterpret_cast<PVOID>(hook_present)) detour = reinterpret_cast<PVOID>(at_present);
    else if (detour == reinterpret_cast<PVOID>(hook_present_ex)) detour = reinterpret_cast<PVOID>(at_present_ex);
    else if (detour == reinterpret_cast<PVOID>(hook_present_swap)) detour = reinterpret_cast<PVOID>(at_present_swap);
    return DetourAttach(target, detour);
}

HWND dummy_window = nullptr;
static void d3d9_install_diagnostic(const char *message)
{
    char line[512] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[at-overlay:%lu] d3d9-hook: %s\n", GetCurrentProcessId(), message);
    at::log_write(line);
}

// OBS uses this gate before freeing its D3D9 state on Reset. The AT overlay
// also owns device-bound ImGui objects, even while frame capture is idle

bool capture_active() { return true; }
HMODULE get_system_module(const char *name) { return GetModuleHandleA(name); }
HMODULE load_system_library(const char *name) { return LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); }
const char *get_process_name() { return at::process_name(); }
size_t module_size(HMODULE module)
{
    if (!module) return 0;
    auto *base = reinterpret_cast<const unsigned char *>(module);
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.SizeOfImage : 0;
}
void *get_offset_addr(HMODULE module, uint32_t offset)
{
    return reinterpret_cast<unsigned char *>(module) + offset;
}

namespace at::d3d9_capture {
void free() { ::d3d9_free(); }
bool uses_gpu() { return at_obs_uses_gpu(); }
bool uses_cpu() { return at_obs_uses_cpu(); }
}

// OBS gates its D3D9 frame-copy path on data.handle, even in shared-memory
// mode. Its shared-memory initializer does not set that shared-texture handle,
// so adapt the gate locally while leaving the vendored OBS source untouched

bool at_obs_d3d9_capture_init_shmem(shmem_data **out, HWND window,
                                    uint32_t width, uint32_t height,
                                    uint32_t pitch, uint32_t format, bool flip_y)
{
    if (!capture_init_shmem(out, window, width, height, pitch, format, flip_y))
        return false;
    data.handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
    return true;
}

bool at_obs_d3d9_capture_should_stop()
{
    static volatile LONG logged_present = 0;
    if (InterlockedCompareExchange(&logged_present, 1, 0) == 0)
        d3d9_install_diagnostic("OBS D3D9 Present hook callback reached");
    // Poll capture requests and control messages from AT
    at::capture_update();
    // OBS performs D3D9 capture directly in its Present hook, outside the
    // generic DXGI adapter that normally signals completion on the next frame
    at_obs_after_capture(false, global_hook_info->force_shmem);
    at_obs_before_capture();
    return at::capture_should_stop();
}

void at_obs_d3d9_capture_free()
{
    if (data.device) at_obs_d3d9_reset(data.device);
    capture_free();
}

bool at_obs_d3d9_install()
{
    d3d9_install_diagnostic("adapter install entered");
    if (!global_hook_info) {
        d3d9_install_diagnostic("global_hook_info is null");
        return false;
    }
    if (RealPresent || RealPresentEx || RealPresentSwap) return true;

    HMODULE d3d9_module = GetModuleHandleA("d3d9.dll");
    const uintptr_t module_base = reinterpret_cast<uintptr_t>(d3d9_module);
    const size_t image_size = module_size(d3d9_module);
    void *present = nullptr, *present_ex = nullptr, *present_swap = nullptr;
    void *reset = nullptr, *reset_ex = nullptr;
    const bool probed = d3d9_module && at_d3d9_probe_present(
        &present, &present_ex, &present_swap, &reset, &reset_ex);
    auto module_offset = [module_base, image_size](void *address, uint32_t &offset) {
        const uintptr_t value = reinterpret_cast<uintptr_t>(address);
        if (!address || value < module_base || value - module_base >= image_size)
            return false;
        offset = static_cast<uint32_t>(value - module_base);
        return true;
    };

    const bool have_offsets = probed &&
        module_offset(present, global_hook_info->offsets.d3d9.present) &&
        module_offset(present_ex, global_hook_info->offsets.d3d9.present_ex) &&
        module_offset(present_swap, global_hook_info->offsets.d3d9.present_swap);

    // OBS falls back to a temporary D3D9Ex device when offsets are unavailable
    // Supply offsets from AT's more tolerant D3D9/D3D9Ex probe when possible
    if (!have_offsets) {
        global_hook_info->offsets.d3d9.present = UINT32_MAX;
        global_hook_info->offsets.d3d9.present_ex = UINT32_MAX;
        global_hook_info->offsets.d3d9.present_swap = UINT32_MAX;
        hlog("d3d9: Present probe unavailable; using OBS temporary-device lookup");
        d3d9_install_diagnostic("AT Present probe unavailable; falling back to OBS temporary device");
    } else {
        hlog("d3d9: using probed OBS Present offsets (%u, %u, %u)",
             global_hook_info->offsets.d3d9.present,
             global_hook_info->offsets.d3d9.present_ex,
             global_hook_info->offsets.d3d9.present_swap);
        d3d9_install_diagnostic("AT Present probe supplied OBS hook offsets");
    }
    uint32_t class_offset = 0, is_ex_offset = 0;
    if (at::d3d9_offset_scan::discover(class_offset, is_ex_offset)) {
        global_hook_info->offsets.d3d9.d3d9_clsoff = class_offset;
        global_hook_info->offsets.d3d9.is_d3d9ex_clsoff = is_ex_offset;
        hlog("d3d9: probed device class offsets (%u, %u)", class_offset, is_ex_offset);
    }
    global_hook_info->capture_overlay = false;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "at_obs_d3d9_hook_window";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    dummy_window = CreateWindowExA(0, wc.lpszClassName, "", WS_POPUP,
                                   0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy_window) return false;
    const bool hook_result = hook_d3d9();
    const bool attached = RealPresent || RealPresentEx || RealPresentSwap;
    if (hook_result && attached) {
        d3d9_install_diagnostic("OBS Present detours attached");
        return true;
    }
    d3d9_install_diagnostic("OBS Present detours NOT attached");
    DestroyWindow(dummy_window);
    dummy_window = nullptr;
    return false;
}

void at_obs_d3d9_uninstall()
{
    if (!RealPresent && !RealPresentEx && !RealPresentSwap && !RealReset && !RealResetEx) {
        if (dummy_window) { DestroyWindow(dummy_window); dummy_window = nullptr; }
        return;
    }
    if (DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    if (RealPresent) DetourDetach(reinterpret_cast<PVOID *>(&RealPresent), at_present);
    if (RealPresentEx) DetourDetach(reinterpret_cast<PVOID *>(&RealPresentEx), at_present_ex);
    if (RealPresentSwap) DetourDetach(reinterpret_cast<PVOID *>(&RealPresentSwap), at_present_swap);
    if (RealReset) DetourDetach(reinterpret_cast<PVOID *>(&RealReset), hook_reset);
    if (RealResetEx) DetourDetach(reinterpret_cast<PVOID *>(&RealResetEx), hook_reset_ex);
    const LONG error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        hlog("d3d9: OBS hook detach failed: %ld", error);
        return;
    }
    RealPresent = nullptr;
    RealPresentEx = nullptr;
    RealPresentSwap = nullptr;
    RealReset = nullptr;
    RealResetEx = nullptr;
    hooked_reset = false;
    if (dummy_window) { DestroyWindow(dummy_window); dummy_window = nullptr; }
}
