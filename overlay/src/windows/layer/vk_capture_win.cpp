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

/*
Vulkan game-frame capture on Windows

CPU readback, structured like hook_dxgi.cpp's copy_backbuffer(): a
per-swapchain readback buffer, copied into on request and mapped straight
into capture_write_frame()
*/

#define VK_USE_PLATFORM_WIN32_KHR
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <windows.h>
#include <vulkan/vulkan.h>

#include "vk_capture_win.h"
#include "win_capture.h"
#include "win_common.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace at {
namespace {

// ===============================================================
// Resolved device dispatch
// ===============================================================

struct VkFns {
    PFN_vkCreateBuffer              CreateBuffer              = nullptr;
    PFN_vkDestroyBuffer             DestroyBuffer             = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory            AllocateMemory            = nullptr;
    PFN_vkFreeMemory                FreeMemory                = nullptr;
    PFN_vkBindBufferMemory          BindBufferMemory          = nullptr;
    PFN_vkMapMemory                 MapMemory                 = nullptr;
    PFN_vkUnmapMemory               UnmapMemory               = nullptr;
    PFN_vkCreateCommandPool         CreateCommandPool         = nullptr;
    PFN_vkDestroyCommandPool        DestroyCommandPool        = nullptr;
    PFN_vkAllocateCommandBuffers    AllocateCommandBuffers    = nullptr;
    PFN_vkBeginCommandBuffer        BeginCommandBuffer        = nullptr;
    PFN_vkEndCommandBuffer          EndCommandBuffer          = nullptr;
    PFN_vkResetCommandBuffer        ResetCommandBuffer        = nullptr;
    PFN_vkCmdCopyImageToBuffer      CmdCopyImageToBuffer      = nullptr;
    PFN_vkCmdPipelineBarrier        CmdPipelineBarrier        = nullptr;
    PFN_vkQueueSubmit               QueueSubmit               = nullptr;
    PFN_vkCreateSemaphore           CreateSemaphore           = nullptr;
    PFN_vkDestroySemaphore          DestroySemaphore          = nullptr;
    PFN_vkCreateFence               CreateFence               = nullptr;
    PFN_vkDestroyFence              DestroyFence              = nullptr;
    PFN_vkWaitForFences             WaitForFences             = nullptr;
    PFN_vkResetFences               ResetFences               = nullptr;
    PFN_vkDeviceWaitIdle            DeviceWaitIdle            = nullptr;
    bool ok = false;
};

struct DeviceCap {
    VkFns                            fn;
    VkPhysicalDeviceMemoryProperties memprops{};
    bool                             tried  = false; /* one-time resolve attempted */
    bool                             usable = false;
};

struct SwapCap {
    VkDevice        device = VK_NULL_HANDLE;
    int             width  = 0;
    int             height = 0;
    bool            rgba_order = false; /* needs swizzle_rgba_to_bgra() before capture_write_frame() */

    VkBuffer        buf    = VK_NULL_HANDLE; /* HOST_VISIBLE readback target */
    VkDeviceMemory  mem    = VK_NULL_HANDLE;
    void*           mapped = nullptr; /* persistently mapped */
    VkDeviceSize    row_pitch = 0; /* tightly packed: width * 4 */

    VkCommandPool   pool   = VK_NULL_HANDLE;
    VkCommandBuffer cmd    = VK_NULL_HANDLE;
    VkFence         fence  = VK_NULL_HANDLE;
    VkSemaphore     sem    = VK_NULL_HANDLE; /* capture -> overlay/present chain */
    bool            fence_pending = false;
};

std::mutex                                     g_mtx;
std::unordered_map<VkDevice, DeviceCap>        g_devs;
std::unordered_map<uint64_t /*VkSwapchainKHR*/, SwapCap> g_swaps;

bool format_supported(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return true;
    default:
        return false;
    }
}

uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties& mp,
                       uint32_t type_bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

// ===============================================================
// Per-device init
// ===============================================================

bool resolve_fns(const VkCaptureFrame& f, VkFns& fn)
{
    auto G = [&](const char* n) { return f.gdpa(f.device, n); };
#define R(name) fn.name = (PFN_vk##name)G("vk" #name); if (!fn.name) return false
    R(CreateBuffer); R(DestroyBuffer); R(GetBufferMemoryRequirements);
    R(AllocateMemory); R(FreeMemory); R(BindBufferMemory);
    R(MapMemory); R(UnmapMemory);
    R(CreateCommandPool); R(DestroyCommandPool); R(AllocateCommandBuffers);
    R(BeginCommandBuffer); R(EndCommandBuffer); R(ResetCommandBuffer);
    R(CmdCopyImageToBuffer); R(CmdPipelineBarrier); R(QueueSubmit);
    R(CreateSemaphore); R(DestroySemaphore);
    R(CreateFence); R(DestroyFence); R(WaitForFences); R(ResetFences);
    R(DeviceWaitIdle);
#undef R
    fn.ok = true;
    return true;
}

DeviceCap* device_cap(const VkCaptureFrame& f)
{
    DeviceCap& dc = g_devs[f.device];
    if (dc.tried) return dc.usable ? &dc : nullptr;
    dc.tried = true;

    if (!resolve_fns(f, dc.fn)) {
        hlog("vk-capture: missing device functions");
        return nullptr;
    }

    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
        f.gipa(f.instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!getMemProps) return nullptr;
    getMemProps(f.phy, &dc.memprops);

    dc.usable = true;
    hlog("vk-capture: device ready (CPU readback)");
    return &dc;
}

// ===============================================================
// Per-swapchain readback buffer
// ===============================================================

void swap_release(DeviceCap* dc, SwapCap& sc)
{
    if (dc && dc->fn.ok && sc.device) {
        if (sc.fence_pending && sc.fence) {
            dc->fn.WaitForFences(sc.device, 1, &sc.fence, VK_TRUE, 1000000000ull);
            sc.fence_pending = false;
        }
        if (sc.mapped) { dc->fn.UnmapMemory(sc.device, sc.mem); sc.mapped = nullptr; }
        if (sc.buf)    dc->fn.DestroyBuffer(sc.device, sc.buf, nullptr);
        if (sc.mem)    dc->fn.FreeMemory(sc.device, sc.mem, nullptr);
        if (sc.sem)    dc->fn.DestroySemaphore(sc.device, sc.sem, nullptr);
        if (sc.fence)  dc->fn.DestroyFence(sc.device, sc.fence, nullptr);
        if (sc.pool)   dc->fn.DestroyCommandPool(sc.device, sc.pool, nullptr);
    }
    sc.buf = VK_NULL_HANDLE; sc.mem = VK_NULL_HANDLE; sc.mapped = nullptr;
    sc.sem = VK_NULL_HANDLE;
    sc.fence = VK_NULL_HANDLE; sc.pool = VK_NULL_HANDLE; sc.cmd = VK_NULL_HANDLE;
    sc.width = sc.height = 0;
    sc.fence_pending = false;
}

bool swap_build(DeviceCap* dc, const VkCaptureFrame& f, SwapCap& sc)
{
    if (!format_supported(f.format)) {
        hlog("vk-capture: unsupported swapchain format %d", (int)f.format);
        return false;
    }

    sc.device = f.device;
    sc.width  = (int)f.extent.width;
    sc.height = (int)f.extent.height;
    sc.row_pitch = (VkDeviceSize)sc.width * 4;
    sc.rgba_order = (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_SRGB);

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = sc.row_pitch * (VkDeviceSize)sc.height;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dc->fn.CreateBuffer(f.device, &bci, nullptr, &sc.buf) != VK_SUCCESS) {
        hlog("vk-capture: vkCreateBuffer failed");
        return false;
    }

    VkMemoryRequirements mr = {};
    dc->fn.GetBufferMemoryRequirements(f.device, sc.buf, &mr);

    uint32_t mt = find_mem_type(dc->memprops, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 
                                                               | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt == UINT32_MAX) {
        mt = find_mem_type(dc->memprops, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (mt == UINT32_MAX) { hlog("vk-capture: no host-visible/coherent memory type"); return false; }

    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mt;
    if (dc->fn.AllocateMemory(f.device, &mai, nullptr, &sc.mem) != VK_SUCCESS) {
        hlog("vk-capture: vkAllocateMemory failed");
        return false;
    }

    if (dc->fn.BindBufferMemory(f.device, sc.buf, sc.mem, 0) != VK_SUCCESS) {
        hlog("vk-capture: vkBindBufferMemory failed");
        return false;
    }

    if (dc->fn.MapMemory(f.device, sc.mem, 0, VK_WHOLE_SIZE, 0, &sc.mapped) != VK_SUCCESS) {
        hlog("vk-capture: vkMapMemory failed");
        return false;
    }

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = f.queue_family;
    if (dc->fn.CreateCommandPool(f.device, &pci, nullptr, &sc.pool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = sc.pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (dc->fn.AllocateCommandBuffers(f.device, &cbi, &sc.cmd) != VK_SUCCESS)
        return false;

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (dc->fn.CreateFence(f.device, &fci, nullptr, &sc.fence) != VK_SUCCESS)
        return false;

    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (dc->fn.CreateSemaphore(f.device, &sci, nullptr, &sc.sem) != VK_SUCCESS)
        return false;

    capture_frame_capture_started();
    hlog("vk-capture: readback buffer %dx%d ready", sc.width, sc.height);
    return true;
}

void img_barrier(DeviceCap* dc, VkCommandBuffer cmd, VkImage img,
                 VkImageLayout from, VkImageLayout to,
                 VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    dc->fn.CmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

// ===============================================================
// Public
// ===============================================================

const char* const* vk_capture_device_extensions(uint32_t* count)
{
    *count = 0;
    return nullptr;
}

const char* const* vk_capture_instance_extensions(uint32_t* count)
{
    *count = 0;
    return nullptr;
}

VkSemaphore vk_capture_present(const VkCaptureFrame& f)
{
    std::lock_guard<std::mutex> lk(g_mtx);

    const bool wantStop = capture_should_stop();
    const bool wantInit = capture_should_init();
    const bool ready    = capture_ready();
    if (!wantStop && !wantInit && !ready) return VK_NULL_HANDLE;

    DeviceCap* dc = device_cap(f);
    if (!dc) return VK_NULL_HANDLE;

    SwapCap& sc = g_swaps[(uint64_t)f.swapchain];

    if (wantStop) { swap_release(dc, sc); capture_stop(); }

    const bool size_changed = sc.buf && (sc.width != (int)f.extent.width || sc.height != (int)f.extent.height);
    if (size_changed) swap_release(dc, sc);

    if (capture_should_init() || (capture_ready() && !sc.buf)) {
        if (!swap_build(dc, f, sc)) { swap_release(dc, sc); capture_stop(); return VK_NULL_HANDLE; }
    }

    if (!sc.buf || !capture_ready() || !capture_frame_requested())
        return VK_NULL_HANDLE;

    if (sc.fence_pending) {
        dc->fn.WaitForFences(f.device, 1, &sc.fence, VK_TRUE, 1000000000ull);
        dc->fn.ResetFences(f.device, 1, &sc.fence);
        sc.fence_pending = false;
    }

    dc->fn.ResetCommandBuffer(sc.cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dc->fn.BeginCommandBuffer(sc.cmd, &bi);

    img_barrier(dc, sc.cmd, f.src_image,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                0, VK_ACCESS_TRANSFER_READ_BIT);

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { f.extent.width, f.extent.height, 1 };
    dc->fn.CmdCopyImageToBuffer(sc.cmd, f.src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc.buf, 1, &region);

    img_barrier(dc, sc.cmd, f.src_image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_READ_BIT, 0);

    dc->fn.EndCommandBuffer(sc.cmd);

    std::vector<VkPipelineStageFlags> stages(f.wait_semaphore_count,
                                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = f.wait_semaphore_count;
    si.pWaitSemaphores = f.wait_semaphores;
    si.pWaitDstStageMask = stages.empty() ? nullptr : stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sc.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sc.sem;

    if (dc->fn.QueueSubmit(f.queue, 1, &si, sc.fence) != VK_SUCCESS) {
        hlog("vk-capture: vkQueueSubmit failed");
        return VK_NULL_HANDLE;
    }
    sc.fence_pending = true;

    if (dc->fn.WaitForFences(f.device, 1, &sc.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) {
        hlog("vk-capture: WaitForFences timed out");
        return sc.sem;
    }
    dc->fn.ResetFences(f.device, 1, &sc.fence);
    sc.fence_pending = false;

    if (sc.rgba_order)
        swizzle_rgba_to_bgra(sc.mapped, sc.width, sc.height, (int)sc.row_pitch);
    if (capture_write_frame(sc.width, sc.height, sc.mapped, (int)sc.row_pitch)) {
        capture_frame_delivered();
    }
    return sc.sem;
}

void vk_capture_swapchain_destroyed(VkDevice device, VkSwapchainKHR swapchain)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_swaps.find((uint64_t)swapchain);
    if (it == g_swaps.end()) return;
    auto dit = g_devs.find(device);
    swap_release(dit == g_devs.end() ? nullptr : &dit->second, it->second);
    g_swaps.erase(it);
    capture_stop();
}

void vk_capture_device_destroyed(VkDevice device)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto dit = g_devs.find(device);
    DeviceCap* dc = dit == g_devs.end() ? nullptr : &dit->second;
    if (dc && dc->fn.ok) dc->fn.DeviceWaitIdle(device);

    for (auto it = g_swaps.begin(); it != g_swaps.end(); ) {
        if (it->second.device == device) {
            swap_release(dc, it->second);
            it = g_swaps.erase(it);
        } else {
            ++it;
        }
    }
    if (dc) g_devs.erase(dit);
}

} // namespace at
