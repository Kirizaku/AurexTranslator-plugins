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

/* D3D12 swapchain overlay renderer and capture integration */

#include <d3d12.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi1_4.h>

#include <cstring>
#include <vector>

#include "win_common.h"
#include "win_capture.h"
#include "../capture/d3d/obs_capture_bridge.h"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include "overlay_d3d12.h"
#include "overlay_text.h"
#include "overlay_ui.h"

extern thread_local int dxgi_presenting;
extern ID3D12CommandQueue *dxgi_possible_swap_queues[8];
extern size_t dxgi_possible_swap_queue_count;
extern bool dxgi_present_attempted;

namespace at {
namespace {

// ===============================================================
// Shader-visible SRV heap sub-allocator
// ===============================================================

struct SrvHeap {
    ID3D12DescriptorHeap*       heap = nullptr;
    UINT                        inc  = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = {};
    std::vector<UINT>           free;

    bool init(ID3D12Device* dev, UINT count)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = count;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
                                             (void**)&heap))) {
            return false;
        }
        inc  = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpu0 = heap->GetCPUDescriptorHandleForHeapStart();
        gpu0 = heap->GetGPUDescriptorHandleForHeapStart();
        free.clear();
        free.reserve(count);
        for (UINT i = count; i-- > 0;) free.push_back(i);
        return true;
    }

    void alloc(D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g)
    {
        IM_ASSERT(!free.empty() && "at-overlay: SRV heap exhausted");
        UINT idx = free.back();
        free.pop_back();
        c->ptr = cpu0.ptr + (SIZE_T)idx * inc;
        g->ptr = gpu0.ptr + (UINT64)idx * inc;
    }

    void release_one(D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        UINT idx = (UINT)((c.ptr - cpu0.ptr) / inc);
        free.push_back(idx);
    }

    void destroy() { safe_release(heap); free.clear(); }
};

void srv_alloc_cb(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g)
{
    static_cast<SrvHeap*>(info->UserData)->alloc(c, g);
}
void srv_free_cb(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE g)
{
    static_cast<SrvHeap*>(info->UserData)->release_one(c, g);
}

// ===============================================================
// Per-swapchain overlay state
// ===============================================================

constexpr UINT kSrvHeapSize = 64;

struct Frame {
    ID3D12CommandAllocator*     alloc      = nullptr;
    ID3D12Resource*             backbuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv        = {};
    UINT64                      fence_value = 0; /* our last signal for it */
};

struct OverlayD3D12 {
    IDXGISwapChain*     swap  = nullptr;
    HWND                window = nullptr;
    ID3D12Device*       dev   = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    UINT        width  = 0;
    UINT        height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT        buffer_count = 0;

    ID3D12DescriptorHeap*      rtv_heap = nullptr;
    UINT                       rtv_inc  = 0;
    ID3D12GraphicsCommandList* cmdlist  = nullptr;
    std::vector<Frame>         frames;

    ID3D12Fence* fence      = nullptr;
    UINT64       fence_last = 0;
    HANDLE       fence_event = nullptr;

    SrvHeap       srv;
    ImGuiContext* imgui    = nullptr;
    bool          imgui_ok = false;

    bool                        capture_active  = false;
};

OverlayD3D12 g_ov;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;

struct Lock {
    Lock()  { if (g_lock_ready) EnterCriticalSection(&g_lock); }
    ~Lock() { if (g_lock_ready) LeaveCriticalSection(&g_lock); }
};

thread_local bool g_in_present = false;

// ===============================================================
// GPU synchronisation
// ===============================================================

void wait_fence(UINT64 v)
{
    if (!g_ov.fence || g_ov.fence->GetCompletedValue() >= v) return;
    if (FAILED(g_ov.fence->SetEventOnCompletion(v, g_ov.fence_event))) return;
    WaitForSingleObject(g_ov.fence_event, INFINITE);
}

void wait_idle()
{
    if (g_ov.queue && g_ov.fence) {
        const UINT64 v = ++g_ov.fence_last;
        if (SUCCEEDED(g_ov.queue->Signal(g_ov.fence, v))) wait_fence(v);
    }
}

// ===============================================================
// Teardown
// ===============================================================

void capture_release_shared();

void release_targets()
{
    capture_release_shared();
    for (Frame& f : g_ov.frames) {
        safe_release(f.alloc);
        safe_release(f.backbuffer);
    }
    g_ov.frames.clear();
    safe_release(g_ov.cmdlist);
    safe_release(g_ov.rtv_heap);
    g_ov.width = g_ov.height = 0;
    g_ov.format = DXGI_FORMAT_UNKNOWN;
    g_ov.buffer_count = 0;
}

void shutdown_overlay()
{
    wait_idle();

    if (g_ov.imgui_ok) {
        ImGui::SetCurrentContext(g_ov.imgui);
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext(g_ov.imgui);
        g_ov.imgui    = nullptr;
        g_ov.imgui_ok = false;
    }

    release_targets();
    g_ov.srv.destroy();

    if (g_ov.fence_event) { CloseHandle(g_ov.fence_event); g_ov.fence_event = nullptr; }
    safe_release(g_ov.fence);
    g_ov.fence_last = 0;

    safe_release(g_ov.queue);
    safe_release(g_ov.dev);
    g_ov.swap = nullptr;
    g_ov.window = nullptr;
}

// ===============================================================
// Device + queue acquisition
// ===============================================================

DXGI_FORMAT rtv_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:                              return f;
    }
}

bool acquire_device_queue(IDXGISwapChain* swap,
                          ID3D12Device** out_dev, ID3D12CommandQueue** out_queue)
{
    ID3D12Device* swap_device = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D12Device), (void**)&swap_device)) || !swap_device)
        return false;

    ID3D12CommandQueue* match = nullptr;
    {
        for (size_t i = dxgi_possible_swap_queue_count; i > 0; --i) {
            ID3D12CommandQueue* candidate = dxgi_possible_swap_queues[i - 1];
            ID3D12Device* candidate_device = nullptr;
            if (SUCCEEDED(candidate->GetDevice(__uuidof(ID3D12Device),
                                                (void**)&candidate_device)) && candidate_device) {
                const bool same_device = candidate_device == swap_device;
                candidate_device->Release();
                if (same_device) {
                    candidate->AddRef();
                    match = candidate;
                    break;
                }
            }
        }
    }

    if (!match) {
        swap_device->Release();
        return false;
    }
    *out_dev = swap_device;
    *out_queue = match;
    return true;
}

bool ensure_device(IDXGISwapChain* swap)
{
    if (g_ov.swap == swap && g_ov.dev && g_ov.queue) return true;

    shutdown_overlay();

    ID3D12Device*       dev   = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    if (!acquire_device_queue(swap, &dev, &queue)) return false;

    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                (void**)&g_ov.fence))) {
        hlog("d3d12: CreateFence failed");
        dev->Release();
        queue->Release();
        return false;
    }
    g_ov.fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g_ov.fence_event) {
        hlog("d3d12: CreateEvent failed");
        safe_release(g_ov.fence);
        dev->Release();
        queue->Release();
        return false;
    }

    g_ov.swap  = swap;
    g_ov.dev   = dev;
    g_ov.queue = queue;
    return true;
}

// ===============================================================
// Size-dependent objects (heaps, allocators, RTVs, command list)
// ===============================================================

bool ensure_targets(IDXGISwapChain* swap)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swap->GetDesc(&desc))) return false;
    g_ov.window = desc.OutputWindow;
    const bool changed = (desc.BufferDesc.Width  != g_ov.width) ||
                         (desc.BufferDesc.Height != g_ov.height) ||
                         (desc.BufferDesc.Format != g_ov.format) ||
                         (desc.BufferCount        != g_ov.buffer_count) ||
                         g_ov.frames.empty() || !g_ov.rtv_heap || !g_ov.cmdlist;

    if (!changed) return true;

    wait_idle();
    release_targets();

    g_ov.width        = desc.BufferDesc.Width;
    g_ov.height       = desc.BufferDesc.Height;
    g_ov.format       = desc.BufferDesc.Format;
    g_ov.buffer_count = desc.BufferCount;
    if (g_ov.width == 0 || g_ov.height == 0 || g_ov.buffer_count == 0) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = g_ov.buffer_count;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; /* RTVs are CPU-only */
    if (FAILED(g_ov.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
                                              (void**)&g_ov.rtv_heap))) {
        hlog("d3d12: CreateDescriptorHeap(RTV) failed");
        return false;
    }
    g_ov.rtv_inc = g_ov.dev->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_RENDER_TARGET_VIEW_DESC rtvd = {};
    rtvd.Format        = rtv_format(g_ov.format);
    rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_ov.rtv_heap->GetCPUDescriptorHandleForHeapStart();

    g_ov.frames.resize(g_ov.buffer_count);
    for (UINT i = 0; i < g_ov.buffer_count; ++i) {
        Frame& f = g_ov.frames[i];

        if (FAILED(swap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&f.backbuffer)) || !f.backbuffer) {
            hlog("d3d12: GetBuffer(%u) failed", i);
            release_targets();
            return false;
        }
        f.rtv = rtv;
        g_ov.dev->CreateRenderTargetView(f.backbuffer, &rtvd, f.rtv);
        rtv.ptr += g_ov.rtv_inc;

        if (FAILED(g_ov.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            __uuidof(ID3D12CommandAllocator), (void**)&f.alloc))) {
            hlog("d3d12: CreateCommandAllocator(%u) failed", i);
            release_targets();
            return false;
        }
        f.fence_value = 0;
    }

    if (FAILED(g_ov.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        g_ov.frames[0].alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_ov.cmdlist))) {
        hlog("d3d12: CreateCommandList failed");
        release_targets();
        return false;
    }
    g_ov.cmdlist->Close();

    return true;
}

// ===============================================================
// Capture path
// ===============================================================

void capture_release_shared()
{
    if (g_ov.capture_active) wait_idle();
    d3d12_capture::free();
    g_ov.capture_active = false;
}

// ===============================================================
// ImGui
// ===============================================================

bool ensure_imgui()
{
    if (g_ov.imgui_ok) return true;

    if (!g_ov.srv.heap && !g_ov.srv.init(g_ov.dev, kSrvHeapSize)) {
        hlog("d3d12: SRV heap creation failed");
        return false;
    }

    IMGUI_CHECKVERSION();
    g_ov.imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ov.imgui);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;

    at_text_setup_font();

    ImGui_ImplDX12_InitInfo info = {};
    info.Device            = g_ov.dev;
    info.CommandQueue      = g_ov.queue;
    info.NumFramesInFlight = (int)g_ov.buffer_count;
    info.RTVFormat         = rtv_format(g_ov.format);
    info.DSVFormat         = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g_ov.srv.heap;
    info.UserData          = &g_ov.srv;
    info.SrvDescriptorAllocFn = srv_alloc_cb;
    info.SrvDescriptorFreeFn  = srv_free_cb;

    if (!ImGui_ImplDX12_Init(&info)) {
        hlog("d3d12: ImGui_ImplDX12_Init failed");
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
    at_overlay_draw_ui("D3D12", (int)io.DisplaySize.x, (int)io.DisplaySize.y,
                       g_ov.capture_active ? "GPU" : "Waiting");
}

// ===============================================================
// The per-present draw
// ===============================================================

void draw_overlay(IDXGISwapChain* swap)
{
    static int no_draw = -1;
    if (env_flag_cached("AT_OVERLAY_DISABLE_DRAW", &no_draw)) return;
    if (!ensure_imgui()) return;

    ImGui::SetCurrentContext(g_ov.imgui);
    if (at_text_font_dirty()) {
        ImGui_ImplDX12_InvalidateDeviceObjects();
        at_text_font_rebuilt();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_ov.width, (float)g_ov.height);
    io.DeltaTime   = frame_delta();

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    build_ui();
    ImGui::Render();

    ImDrawData* draw = ImGui::GetDrawData();
    if (!draw || draw->CmdListsCount == 0) return;

    IDXGISwapChain3* swap3 = nullptr;
    if (FAILED(swap->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&swap3)) ||
        !swap3) {
        return;
    }
    const UINT idx = swap3->GetCurrentBackBufferIndex();
    swap3->Release();
    if (idx >= g_ov.frames.size()) return;

    Frame& f = g_ov.frames[idx];

    wait_fence(f.fence_value);

    if (FAILED(f.alloc->Reset())) return;
    if (FAILED(g_ov.cmdlist->Reset(f.alloc, nullptr))) return;

    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = f.backbuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_ov.cmdlist->ResourceBarrier(1, &b);

    g_ov.cmdlist->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g_ov.srv.heap };
    g_ov.cmdlist->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(draw, g_ov.cmdlist);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_ov.cmdlist->ResourceBarrier(1, &b);

    if (FAILED(g_ov.cmdlist->Close())) return;

    ID3D12CommandList* lists[] = { g_ov.cmdlist };
    at_obs_d3d12_execute(g_ov.queue, 1, lists);

    const UINT64 v = ++g_ov.fence_last;
    if (SUCCEEDED(g_ov.queue->Signal(g_ov.fence, v))) f.fence_value = v;

    static bool drew_once = false;
    if (!drew_once) {
        drew_once = true;
        hlog("d3d12: first overlay frame submitted (%ux%u, %u buffers)", g_ov.width, g_ov.height, g_ov.buffer_count);
    }
}

void on_present(IDXGISwapChain* swap)
{
    static bool logged_enter = false;
    if (!logged_enter) { logged_enter = true; hlog("d3d12: present routed to D3D12 backend"); }

    if (!ensure_device(swap)) {
        static bool logged_nodev = false;
        if (!logged_nodev) { logged_nodev = true; hlog("d3d12: no device/queue yet (waiting for ExecuteCommandLists)"); }
        return;
    }
    if (!ensure_targets(swap)) {
        static bool logged_notgt = false;
        if (!logged_notgt) { logged_notgt = true; hlog("d3d12: ensure_targets failed"); }
        return;
    }

    capture_update();

    d3d12_capture::capture(swap, g_ov.queue);
    g_ov.capture_active = d3d12_capture::uses_gpu();

    draw_overlay(swap);
}

bool is_d3d12(IDXGISwapChain* swap)
{
    ID3D12Device* device = nullptr;
    if (FAILED(swap->GetDevice(__uuidof(ID3D12Device), (void**)&device)) || !device)
        return false;
    device->Release();
    return true;
}

} // namespace

// ===============================================================
// Public entry points
// ===============================================================

bool d3d12_hook_queue()
{
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
    return at_obs_d3d12_hook_install();
}

bool d3d12_try_present(IDXGISwapChain* swap)
{
    if (!is_d3d12(swap)) return false;

    if (!g_in_present) {
        g_in_present = true;
        {
            Lock lk;
            on_present(swap);
        }
        g_in_present = false;
    }
    return true;
}

void d3d12_on_resize()
{
    Lock lk;
    if (g_ov.frames.empty() && !g_ov.rtv_heap) return;
    wait_idle();
    release_targets();
}

void d3d12_on_swapchain_shutdown(IDXGISwapChain* swap)
{
    Lock lk;
    if (g_ov.swap != swap) return;

    hlog("d3d12: releasing overlay and capture resources before fullscreen transition");
    shutdown_overlay();
    dxgi_possible_swap_queue_count = 0;
    memset(dxgi_possible_swap_queues, 0, sizeof(dxgi_possible_swap_queues));
}

void d3d12_shutdown()
{
    at_obs_d3d12_hook_uninstall();

    if (g_lock_ready) {
        EnterCriticalSection(&g_lock);
        shutdown_overlay();
        LeaveCriticalSection(&g_lock);
        DeleteCriticalSection(&g_lock);
        g_lock_ready = false;
    }
    dxgi_possible_swap_queue_count = 0;
    memset(dxgi_possible_swap_queues, 0, sizeof(dxgi_possible_swap_queues));
    dxgi_presenting = 0;
    dxgi_present_attempted = false;
}

} // namespace at
