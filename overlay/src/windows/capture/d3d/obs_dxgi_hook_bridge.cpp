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

#include "win_common.h"
#include "win_capture.h"
#include "obs_capture_bridge.h"
#include "overlay_gl.h"

#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#undef hlog
#define COMPILE_D3D12_HOOK 0
#define capture_active at_obs_dxgi_capture_active
#define should_passthrough at_obs_dxgi_should_passthrough
#include "graphics-hook.h"

void at_obs_dxgi_capture_begin();

// OBS uses capture_active only to suppress swap-chain setup retries. AT wants
// setup_dxgi retried until it finds a usable swap chain, so this stays false
bool at_obs_dxgi_capture_active() { return false; }
bool at_obs_dxgi_should_passthrough() { return at::vk_overlay_is_active() || at::gl_capture_internal_present_active(); }
void at_obs_dxgi_capture_begin() { at::capture_update(); }

#include <detours.h>
static LONG WINAPI at_dxgi_detour_attach(PVOID *target, PVOID detour);
#define DetourAttach at_dxgi_detour_attach
#include "../../../../third_party/obs-studio/graphics-hook/dxgi-capture.cpp"
#undef DetourAttach

static HRESULT STDMETHODCALLTYPE at_bridge_hook_resize_buffers(IDXGISwapChain *swap, UINT buffer_count, UINT width, UINT height,
						     DXGI_FORMAT format, UINT flags)
{
	hlog_verbose("ResizeBuffers callback");
	at_obs_dxgi_on_resize(swap);

	data.swap = nullptr;
	data.capture = nullptr;
	memset(dxgi_possible_swap_queues, 0, sizeof(dxgi_possible_swap_queues));
	dxgi_possible_swap_queue_count = 0;
	dxgi_present_attempted = false;

	if (data.free) {
		data.free();
	}
	data.free = nullptr;

	const HRESULT hr = RealResizeBuffers(swap, buffer_count, width, height, format, flags);

	return hr;
}

static HRESULT STDMETHODCALLTYPE at_bridge_hook_present(IDXGISwapChain *swap, UINT sync_interval, UINT flags)
{
	if (should_passthrough()) {
		dxgi_presenting = true;
		const HRESULT hr = RealPresent(swap, sync_interval, flags);
		dxgi_presenting = false;
		return hr;
	}

	at_obs_dxgi_capture_begin();

	const bool test_draw = (flags & DXGI_PRESENT_TEST) != 0;

	if (data.swap) {
		update_mismatch_count(swap == data.swap);
	}

	if (!data.swap && !capture_active()) {
		setup_dxgi(swap);
	}

	hlog_verbose("Present callback: sync_interval=%u, flags=%u, current_swap=0x%" PRIX64
		     ", expected_swap=0x%" PRIX64,
		     sync_interval, flags, swap, data.swap);
	const bool capture = !test_draw && swap == data.swap && data.capture;
	if (capture) {
		IUnknown *backbuffer = get_dxgi_backbuffer(swap);

		if (backbuffer) {
			at_obs_dxgi_capture(data.capture, swap, backbuffer);
			backbuffer->Release();
		}
	}

	++dxgi_presenting;
	if (!test_draw)
		at_obs_dxgi_render(swap);
	const HRESULT hr = RealPresent(swap, sync_interval, flags);
	--dxgi_presenting;
	dxgi_present_attempted = true;

	return hr;
}

static HRESULT STDMETHODCALLTYPE at_bridge_hook_present1(IDXGISwapChain1 *swap, UINT sync_interval, UINT flags,
					       const DXGI_PRESENT_PARAMETERS *params)
{
	if (should_passthrough()) {
		dxgi_presenting = true;
		const HRESULT hr = RealPresent1(swap, sync_interval, flags, params);
		dxgi_presenting = false;
		return hr;
	}

	at_obs_dxgi_capture_begin();

	const bool test_draw = (flags & DXGI_PRESENT_TEST) != 0;

	if (data.swap) {
		update_mismatch_count(swap == data.swap);
	}

	if (!data.swap && !capture_active()) {
		setup_dxgi(swap);
	}

	hlog_verbose("Present1 callback: sync_interval=%u, flags=%u, current_swap=0x%" PRIX64
		     ", expected_swap=0x%" PRIX64,
		     sync_interval, flags, swap, data.swap);
	const bool capture = !test_draw && swap == data.swap && !!data.capture;
	if (capture) {
		IUnknown *backbuffer = get_dxgi_backbuffer(swap);

		if (backbuffer) {
			at_obs_dxgi_capture(data.capture, swap, backbuffer);
			backbuffer->Release();
		}
	}

	++dxgi_presenting;
	if (!test_draw)
		at_obs_dxgi_render((IDXGISwapChain *)swap);
	
	const HRESULT hr = RealPresent1(swap, sync_interval, flags, params);
	--dxgi_presenting;
	dxgi_present_attempted = true;

	return hr;
}

static LONG WINAPI at_dxgi_detour_attach(PVOID *target, PVOID detour)
{
    if (detour == reinterpret_cast<PVOID>(hook_resize_buffers))
        detour = reinterpret_cast<PVOID>(at_bridge_hook_resize_buffers);
    else if (detour == reinterpret_cast<PVOID>(hook_present))
        detour = reinterpret_cast<PVOID>(at_bridge_hook_present);
    else if (detour == reinterpret_cast<PVOID>(hook_present1))
        detour = reinterpret_cast<PVOID>(at_bridge_hook_present1);
    return DetourAttach(target, detour);
}

namespace at {
bool at_obs_dxgi_hook_install(void *present, void *resize, void *present1)
{
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi || !global_hook_info) return false;

    const uintptr_t base = reinterpret_cast<uintptr_t>(dxgi);
    const size_t image_size = module_size(dxgi);
    auto get_offset = [base, image_size](void *address, uint32_t &offset) {
        const uintptr_t value = reinterpret_cast<uintptr_t>(address);
        if (!address || value < base || value - base >= image_size) return false;
        offset = static_cast<uint32_t>(value - base);
        return true;
    };

    auto &offsets = global_hook_info->offsets.dxgi;
    if (!get_offset(present, offsets.present) ||
        !get_offset(resize, offsets.resize)) {
        hlog("dxgi: OBS hook addresses are outside dxgi.dll");
        return false;
    }
    offsets.present1 = 0;
    if (present1 && !get_offset(present1, offsets.present1)) {
        hlog("dxgi: OBS Present1 address is outside dxgi.dll");
        return false;
    }

    global_hook_info->capture_overlay = false;
    const bool installed = hook_dxgi();
    hlog("dxgi: OBS hook_dxgi %s", installed ? "installed" : "failed");
    return installed;
}
} // namespace at
