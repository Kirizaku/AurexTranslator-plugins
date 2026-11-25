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
#undef hlog

#include <d3d12.h>
#include <dxgi.h>
#include "graphics-hook.h"

// Keep OBS's private hook trampoline and its capture code in this adapter TU
#include "../../../../third_party/obs-studio/graphics-hook/d3d12-capture.cpp"
#undef capture_should_stop
#undef capture_should_init
#undef capture_ready

bool at_obs_d3d12_hook_install() { return hook_d3d12(); }
void at_obs_d3d12_hook_uninstall()
{
    if (!RealExecuteCommandLists || DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID *>(&RealExecuteCommandLists), hook_execute_command_lists);
    if (DetourTransactionCommit() == NO_ERROR) RealExecuteCommandLists = nullptr;
}
void at_obs_d3d12_execute(ID3D12CommandQueue *queue, UINT count,
                          ID3D12CommandList *const *lists)
{
    if (RealExecuteCommandLists) RealExecuteCommandLists(queue, count, lists);
    else queue->ExecuteCommandLists(count, lists);
}

namespace at::d3d12_capture {
void capture(IDXGISwapChain *swap, ID3D12CommandQueue *queue)
{
    static ULONGLONG retry_after = 0;
    static bool cpu_fallback_logged = false;
    // OBS's D3D12 capture path only supports shared GPU textures. Prevent a
    // CPU-fallback request from stopping and repeatedly reinitializing capture
    if (at::capture_force_cpu_requested()) {
        at::capture_disable_cpu_fallback();
        if (!cpu_fallback_logged) {
            hlog("d3d12 CPU readback is unsupported; keeping OBS GPU capture");
            cpu_fallback_logged = true;
        }
    }
    at_obs_before_capture();
    const bool initializing = at::capture_should_init();
    if (initializing && GetTickCount64() < retry_after) return;
    if (initializing) {
        dxgi_possible_swap_queues[0] = queue;
        dxgi_possible_swap_queue_count = queue ? 1 : 0;
    }
    ::d3d12_capture(swap, nullptr);
    dxgi_possible_swap_queues[0] = nullptr;
    dxgi_possible_swap_queue_count = 0;
    at_obs_after_capture(false, false);

    retry_after = initializing && !at::capture_ready() ? GetTickCount64() + 1000 : 0;
}
void free()
{
    ::d3d12_free();
}
bool uses_gpu() { return at::capture_ready(); }
}
