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
D3D12 swapchain overlay backend

Structure follows obs-studio/plugins/win-capture/graphics-hook
(d3d12-capture.c), rewritten for this project.
*/

#include "win_common.h"
#include "win_capture.h"
#include "overlay_d3d12.h"
#include "overlay_text.h"
#include "overlay_ui.h"

#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include <atomic>
#include <vector>

#include <detours/detours.h>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

namespace at {
namespace {

// ===============================================================
// Command-queue capture
// ===============================================================

typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PFN_ExecuteCommandLists real_ExecuteCommandLists = nullptr;
std::atomic<ID3D12CommandQueue*> g_captured_queue{nullptr};

void STDMETHODCALLTYPE hook_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n,
                                                ID3D12CommandList* const* lists)
{
    HookEntry guard;
    if (guard && q) {
        /* Only DIRECT queues present; compute/copy queues are not what the
         * swapchain runs on, so ignore them to avoid capturing the wrong one. */
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_captured_queue.store(q, std::memory_order_relaxed);
        }
    }
    real_ExecuteCommandLists(q, n, lists);
}

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

    ID3D12Resource*            cap_buf         = nullptr;
    ID3D12CommandAllocator*    cap_alloc       = nullptr;
    ID3D12GraphicsCommandList* cap_cmdlist     = nullptr;
    ID3D12Fence*               cap_fence       = nullptr;
    UINT64                     cap_fence_value = 0;
    HANDLE                     cap_fence_event = nullptr;
    UINT                       cap_row_pitch   = 0;
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
void capture_release_device();

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
    capture_release_device();
    g_ov.srv.destroy();

    if (g_ov.fence_event) { CloseHandle(g_ov.fence_event); g_ov.fence_event = nullptr; }
    safe_release(g_ov.fence);
    g_ov.fence_last = 0;

    safe_release(g_ov.queue);
    safe_release(g_ov.dev);
    g_ov.swap = nullptr;
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
    ID3D12CommandQueue* q = nullptr;
    if (SUCCEEDED(swap->GetDevice(__uuidof(ID3D12CommandQueue), (void**)&q)) && q) {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(q->GetDevice(__uuidof(ID3D12Device), (void**)&d)) && d) {
            *out_queue = q;
            *out_dev   = d;
            return true;
        }
        q->Release();
    }

    ID3D12CommandQueue* cq = g_captured_queue.load(std::memory_order_relaxed);
    if (cq) {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(cq->GetDevice(__uuidof(ID3D12Device), (void**)&d)) && d) {
            cq->AddRef();
            *out_queue = cq;
            *out_dev   = d;
            return true;
        }
    }
    return false;
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

    const bool changed = (desc.BufferDesc.Width  != g_ov.width) ||
                         (desc.BufferDesc.Height != g_ov.height) ||
                         (desc.BufferDesc.Format != g_ov.format) ||
                         (desc.BufferCount        != g_ov.buffer_count) ||
                         g_ov.frames.empty();

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
            return false;
        }
        f.rtv = rtv;
        g_ov.dev->CreateRenderTargetView(f.backbuffer, &rtvd, f.rtv);
        rtv.ptr += g_ov.rtv_inc;

        if (FAILED(g_ov.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            __uuidof(ID3D12CommandAllocator), (void**)&f.alloc))) {
            hlog("d3d12: CreateCommandAllocator(%u) failed", i);
            return false;
        }
        f.fence_value = 0;
    }

    if (FAILED(g_ov.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        g_ov.frames[0].alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_ov.cmdlist))) {
        hlog("d3d12: CreateCommandList failed");
        return false;
    }
    g_ov.cmdlist->Close();

    return true;
}

// ===============================================================
// Capture path
// ===============================================================

constexpr UINT kTexturePitchAlign = 256; /* D3D12_TEXTURE_DATA_PITCH_ALIGNMENT */

UINT align_up(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }

void capture_release_shared()
{
    safe_release(g_ov.cap_buf);
    capture_stop();
}

void capture_release_device()
{
    if (g_ov.cap_fence_event) { CloseHandle(g_ov.cap_fence_event); g_ov.cap_fence_event = nullptr; }
    safe_release(g_ov.cap_fence);
    safe_release(g_ov.cap_cmdlist);
    safe_release(g_ov.cap_alloc);
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

bool capture_init(IDXGISwapChain* swap)
{
    (void)swap;
    if (!format_supported(rtv_format(g_ov.format))) return false;

    if (!g_ov.cap_fence) {
        if (FAILED(g_ov.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            __uuidof(ID3D12CommandAllocator), (void**)&g_ov.cap_alloc))) {
            hlog("d3d12: CreateCommandAllocator(capture) failed");
            return false;
        }
        if (FAILED(g_ov.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            g_ov.cap_alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_ov.cap_cmdlist))) {
            hlog("d3d12: CreateCommandList(capture) failed");
            capture_release_device();
            return false;
        }
        g_ov.cap_cmdlist->Close();
        if (FAILED(g_ov.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            __uuidof(ID3D12Fence), (void**)&g_ov.cap_fence))) {
            hlog("d3d12: CreateFence(capture) failed");
            capture_release_device();
            return false;
        }
        g_ov.cap_fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!g_ov.cap_fence_event) {
            hlog("d3d12: CreateEvent(capture) failed");
            capture_release_device();
            return false;
        }
    }

    g_ov.cap_row_pitch = align_up(g_ov.width * 4, kTexturePitchAlign);
    const UINT64 bufSize = (UINT64)g_ov.cap_row_pitch * g_ov.height;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = bufSize;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Format           = DXGI_FORMAT_UNKNOWN; /* buffers are always UNKNOWN */

    if (FAILED(g_ov.dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, __uuidof(ID3D12Resource), (void**)&g_ov.cap_buf))) {
        hlog("d3d12: CreateCommittedResource(readback) failed");
        return false;
    }

    capture_frame_capture_started();
    return true;
}

bool capture_copy(UINT idx)
{
    if (!g_ov.cap_buf || idx >= g_ov.frames.size()) return false;
    Frame& f = g_ov.frames[idx];
    if (!f.backbuffer) return false;

    if (FAILED(g_ov.cap_alloc->Reset())) return false;
    if (FAILED(g_ov.cap_cmdlist->Reset(g_ov.cap_alloc, nullptr))) return false;

    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = f.backbuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_ov.cap_cmdlist->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource                          = g_ov.cap_buf;
    dst.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset             = 0;
    dst.PlacedFootprint.Footprint.Format   = rtv_format(g_ov.format);
    dst.PlacedFootprint.Footprint.Width    = g_ov.width;
    dst.PlacedFootprint.Footprint.Height   = g_ov.height;
    dst.PlacedFootprint.Footprint.Depth    = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g_ov.cap_row_pitch;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource        = f.backbuffer;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    g_ov.cap_cmdlist->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_ov.cap_cmdlist->ResourceBarrier(1, &b);

    if (FAILED(g_ov.cap_cmdlist->Close())) return false;

    ID3D12CommandList* lists[] = { g_ov.cap_cmdlist };

    if (real_ExecuteCommandLists) {
        real_ExecuteCommandLists(g_ov.queue, 1, lists);
    } else {
        g_ov.queue->ExecuteCommandLists(1, lists);
    }

    const UINT64 v = ++g_ov.cap_fence_value;
    if (FAILED(g_ov.queue->Signal(g_ov.cap_fence, v))) return false;
    if (g_ov.cap_fence->GetCompletedValue() < v) {
        g_ov.cap_fence->SetEventOnCompletion(v, g_ov.cap_fence_event);
        WaitForSingleObject(g_ov.cap_fence_event, INFINITE);
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange = { 0, (SIZE_T)g_ov.cap_row_pitch * g_ov.height };
    if (FAILED(g_ov.cap_buf->Map(0, &readRange, &mapped))) return false;

    if (rtv_format(g_ov.format) == DXGI_FORMAT_R8G8B8A8_UNORM)
        swizzle_rgba_to_bgra(mapped, (int)g_ov.width, (int)g_ov.height, (int)g_ov.cap_row_pitch);

    const bool ok = capture_write_frame((int)g_ov.width, (int)g_ov.height, mapped, (int)g_ov.cap_row_pitch);

    const D3D12_RANGE writtenRange = { 0, 0};
    g_ov.cap_buf->Unmap(0, &writtenRange);
    return ok;
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
    at_overlay_draw_ui("D3D12", (int)io.DisplaySize.x, (int)io.DisplaySize.y);
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
    if (real_ExecuteCommandLists) {
        real_ExecuteCommandLists(g_ov.queue, 1, lists);
    } else {
        g_ov.queue->ExecuteCommandLists(1, lists);
    }

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

    if (capture_should_stop()) capture_release_shared();
    if (capture_should_init()) { if (!capture_init(swap)) capture_stop(); }
    if (capture_ready() && g_ov.cap_buf && capture_frame_requested()) {
        IDXGISwapChain3* s3 = nullptr;
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&s3)) && s3) {
            const UINT idx = s3->GetCurrentBackBufferIndex();
            s3->Release();
            if (capture_copy(idx)) capture_frame_delivered();
        }
    }

    draw_overlay(swap);
}

bool is_d3d12(IDXGISwapChain* swap)
{
    ID3D12CommandQueue* q = nullptr;
    if (SUCCEEDED(swap->GetDevice(__uuidof(ID3D12CommandQueue), (void**)&q)) && q) {
        q->Release();
        return true;
    }

    if (g_captured_queue.load(std::memory_order_relaxed)) {
        ID3D11Device* d11 = nullptr;
        if (SUCCEEDED(swap->GetDevice(__uuidof(ID3D11Device), (void**)&d11)) && d11) {
            d11->Release();
            return false;
        }
        return true;
    }
    return false;
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

    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
    if (FAILED(hr) || !dev) {
        IDXGIFactory4* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory)) && factory) {
            IDXGIAdapter* warp = nullptr;
            if (SUCCEEDED(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), (void**)&warp)) && warp) {
                hr = D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
                warp->Release();
            }
            factory->Release();
        }
    }
    if (FAILED(hr) || !dev) {
        hlog("d3d12: no device (D3D12 unavailable) — D3D12 overlay disabled");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    hr = dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&queue);
    if (FAILED(hr) || !queue) {
        hlog("d3d12: CreateCommandQueue (dummy) failed");
        dev->Release();
        return false;
    }

    void** vt = *(void***)queue;
    void* target = vt[10]; /*ExecuteCommandLists*/

    queue->Release();
    dev->Release();

    real_ExecuteCommandLists = (PFN_ExecuteCommandLists)target;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const bool attached = DetourAttach(&(PVOID&)real_ExecuteCommandLists,
                                        (PVOID)hook_ExecuteCommandLists) == NO_ERROR;
    if (!attached || DetourTransactionCommit() != NO_ERROR) {
        hlog("d3d12: failed to hook ID3D12CommandQueue::ExecuteCommandLists");
        real_ExecuteCommandLists = nullptr;
        return false;
    }

    hlog("d3d12: ExecuteCommandLists hooked (%p)", target);
    return true;
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

void d3d12_shutdown()
{
    if (real_ExecuteCommandLists) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)real_ExecuteCommandLists, (PVOID)hook_ExecuteCommandLists);
        DetourTransactionCommit();
        real_ExecuteCommandLists = nullptr;
    }

    if (g_lock_ready) {
        EnterCriticalSection(&g_lock);
        shutdown_overlay();
        LeaveCriticalSection(&g_lock);
        DeleteCriticalSection(&g_lock);
        g_lock_ready = false;
    }
}

} // namespace at
