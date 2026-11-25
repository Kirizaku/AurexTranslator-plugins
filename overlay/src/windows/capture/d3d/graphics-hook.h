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

#pragma once

#ifndef __cplusplus
#ifdef AT_OBS_VULKAN
#include "../vulkan/vulkan_graphics_hook.h"
#else
#include "../gl/gl_graphics_hook.h"
#endif
#else

#include <windows.h>
#include <cstddef>
#include <cstdint>

// The OBS capture sources include this interface. AT implements its transport
// and request handling separately in obs_dxgi_bridge.cpp

#define NUM_BUFFERS 3
#ifndef COMPILE_D3D12_HOOK
#define COMPILE_D3D12_HOOK 1
#endif
#define hlog_verbose(...) (void)0
#define capture_should_stop obs_capture_should_stop
#define capture_should_init obs_capture_should_init
#define capture_ready obs_capture_ready

struct d3d9_offsets {
    uint32_t present, present_ex, present_swap, d3d9_clsoff, is_d3d9ex_clsoff;
};
struct dxgi_offsets { uint32_t present, resize, present1; };
struct hook_info {
    bool allow_srgb_alias;
    bool force_shmem;
    bool capture_overlay;
    struct { d3d9_offsets d3d9; dxgi_offsets dxgi; } offsets;
};
struct shtex_data {};
struct shmem_data {};
struct ID3D12CommandQueue;

extern hook_info *global_hook_info;
extern thread_local int dxgi_presenting;
extern ID3D12CommandQueue *dxgi_possible_swap_queues[8];
extern size_t dxgi_possible_swap_queue_count;
extern bool dxgi_present_attempted;

void hlog(const char *format, ...);
void hlog_hr(const char *message, HRESULT error);
bool capture_should_stop();
bool capture_should_init();
bool capture_ready();
void capture_free();
bool capture_init_shtex(shtex_data **data, HWND window, uint32_t width, uint32_t height,
                        uint32_t format, bool flip, uintptr_t handle);
bool capture_init_shmem(shmem_data **data, HWND window, uint32_t width, uint32_t height,
                        uint32_t pitch, uint32_t format, bool flip);
void shmem_copy_data(size_t index, void *volatile data);
bool shmem_texture_data_lock(int index);
void shmem_texture_data_unlock(int index);
bool capture_active();
HMODULE get_system_module(const char *name);
HMODULE load_system_library(const char *name);
const char *get_process_name();
size_t module_size(HMODULE module);
void *get_offset_addr(HMODULE module, uint32_t offset);
extern HWND dummy_window;

void d3d10_capture(void *swap, void *backbuffer);
void d3d10_free();
void d3d11_capture(void *swap, void *backbuffer);
void d3d11_free();
#endif
