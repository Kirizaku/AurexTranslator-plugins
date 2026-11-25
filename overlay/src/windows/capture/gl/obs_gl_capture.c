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

#include <windows.h>
#include <detours.h>
static LONG WINAPI at_gl_detour_attach(PVOID *target, PVOID detour);
#define DetourAttach at_gl_detour_attach
#include "../../../../third_party/obs-studio/graphics-hook/gl-capture.c"
#undef DetourAttach

static void at_gl_swap_before(HDC hdc)
{
    gl_swap_begin(hdc);
	if (swap_recurse == 1) {
		at_obs_gl_capture_complete();
		at_obs_gl_render_overlay(hdc);
	}
}

static void at_gl_swap_after(HDC hdc)
{
    gl_swap_end(hdc);
}

static BOOL WINAPI at_gl_swap_buffers(HDC hdc)
{
    at_gl_swap_before(hdc);
    const BOOL result = RealSwapBuffers(hdc);
    at_gl_swap_after(hdc);
    return result;
}

static BOOL WINAPI at_gl_wgl_swap_buffers(HDC hdc)
{
    at_gl_swap_before(hdc);
    const BOOL result = RealWglSwapBuffers(hdc);
    at_gl_swap_after(hdc);
    return result;
}

static BOOL WINAPI at_gl_wgl_swap_layer_buffers(HDC hdc, UINT planes)
{
    at_gl_swap_before(hdc);
    const BOOL result = RealWglSwapLayerBuffers(hdc, planes);
    at_gl_swap_after(hdc);
    return result;
}

static BOOL WINAPI at_gl_delete_context(HGLRC context)
{
    at_obs_gl_context_destroyed(context);
    return hook_wgl_delete_context(context);
}

static LONG WINAPI at_gl_detour_attach(PVOID *target, PVOID detour)
{
    if (detour == (PVOID)hook_swap_buffers) detour = (PVOID)at_gl_swap_buffers;
    else if (detour == (PVOID)hook_wgl_swap_buffers) detour = (PVOID)at_gl_wgl_swap_buffers;
    else if (detour == (PVOID)hook_wgl_swap_layer_buffers) detour = (PVOID)at_gl_wgl_swap_layer_buffers;
    else if (detour == (PVOID)hook_wgl_delete_context) detour = (PVOID)at_gl_delete_context;
    return DetourAttach(target, detour);
}

static struct at_obs_gl_settings settings;
struct at_obs_gl_settings *at_obs_gl_hook_info = &settings;
char at_obs_gl_process_name[MAX_PATH];

void at_obs_gl_force_cpu(bool force_cpu)
{
	settings.force_shmem = force_cpu;
}

bool at_obs_gl_hook_install(void)
{
	strncpy_s(at_obs_gl_process_name, sizeof(at_obs_gl_process_name), get_process_name(), _TRUNCATE);
	return hook_gl();
}
void at_obs_gl_release(void) { if (functions_initialized) gl_free(); }

void at_obs_gl_abandon(void)
{
    capture_free();
    if (data.using_shtex) {
        if (data.d3d11_tex) ID3D11Resource_Release(data.d3d11_tex);
        if (data.d3d11_context) ID3D11DeviceContext_Release(data.d3d11_context);
        if (data.d3d11_device) ID3D11Device_Release(data.d3d11_device);
        if (data.dxgi_swap) IDXGISwapChain_Release(data.dxgi_swap);
        if (data.hwnd) DestroyWindow(data.hwnd);
    }
    memset(&data, 0, sizeof(data));
}
bool at_obs_gl_uses_gpu(void) { return data.using_shtex && capture_active(); }
HDC at_obs_gl_capture_dc(void) { return data.hdc; }
