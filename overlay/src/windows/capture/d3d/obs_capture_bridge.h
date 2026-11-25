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

#include <cstdint>

struct IDXGISwapChain;
struct IDirect3DDevice9;
struct ID3D12CommandQueue;
struct ID3D12CommandList;

namespace at::d3d9_capture {
void free();
bool uses_gpu();
bool uses_cpu();
}
namespace at::d3d10_capture {
bool uses_gpu();
bool uses_cpu();
}
namespace at::d3d11_capture {
bool uses_gpu();
bool uses_cpu();
}
namespace at::d3d12_capture {
void capture(IDXGISwapChain* swap, ID3D12CommandQueue* queue);
void free();
bool uses_gpu();
}

namespace at::d3d9_offset_scan {
bool discover(uint32_t& device_class_offset, uint32_t& is_ex_offset);
}

void at_obs_before_capture();
void at_obs_after_capture(bool initializing, bool tried_cpu);
bool at_obs_uses_gpu();
bool at_obs_uses_cpu();
bool at_obs_d3d9_install();
void at_obs_d3d9_uninstall();
void at_obs_d3d9_present(IDirect3DDevice9* device);
void at_obs_d3d9_reset(IDirect3DDevice9* device);
bool at_d3d9_probe_present(void** present, void** present_ex, void** present_swap,
                           void** reset, void** reset_ex);
bool at_obs_d3d12_hook_install();
void at_obs_d3d12_hook_uninstall();
void at_obs_d3d12_execute(ID3D12CommandQueue* queue, unsigned int count,
                          ID3D12CommandList* const* lists);
void at_obs_dxgi_render(IDXGISwapChain* swap);
void at_obs_dxgi_on_resize(IDXGISwapChain* swap);
void at_obs_dxgi_capture(void (*capture)(void*, void*), void* swap, void* backbuffer);
namespace at {
bool at_obs_dxgi_hook_install(void* present, void* resize, void* present1);
}
