/******************************************************************************
    at-overlay - in-game overlay for AurexTranslator

    Copyright (C) 2026 by Daniil Nabiulin
    Copyright (C) 2021 by David Rosca <nowrep@gmail.com> (obs-vkcapture)

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
NOTE: Derived from obs-vkcapture/src/vklayer.c.
Modifications:
  - Renamed internal functions OBS_* -> AT_* and environment variables
    OBS_VKCAPTURE_* -> AT_OVERLAY_*.
  - Added a call to at_overlay_vk_draw() inside vk_capture() to render
    a Dear ImGui overlay directly into the swapchain image alongside the
    existing capture (see overlay_vk.cpp).
  - Frame capture goes through vk_cpu_init()/vk_cpu_capture()/vk_cpu_free()
    instead of upstream's vk_shtex_* DMA-BUF export, which is removed
    together with everything only it used (surface/window-id tracking,
    per-queue frame pools, DRM format modifiers). The DMA-BUF path hands
    a GPU image to a consumer on the same device - right for OBS's live
    recorder, but OCR needs the pixels in CPU memory anyway and the
    same-GPU requirement fails on hybrid-graphics machines. vk_cpu_*
    copies into a HOST_VISIBLE linear staging image on request and calls
    capture_write_frame(), the shared-memory delivery every other backend
    uses (and upstream OBS's own SHMEM fallback).
  - AT_CreateInstance()/AT_CreateDevice() no longer require or
    force-enable the external-memory/DMA-BUF/DRM-modifier extensions the
    removed path needed: forcing an extension the driver doesn't support
    fails instance/device creation for the *game*, not just capture
*/

#include "vklayer.h"
#include "host/overlay_protocol.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include <vulkan/vk_layer.h>

// Derived from obs-vkcapture/src/vklayer.c, which itself is based on
// obs-studio/plugins/win-capture/graphics-hook/vulkan-capture.c

/* ======================================================================== */
/* defs/statics                                                             */

/* use the loader's dispatch table pointer as a key for internal data maps */
#define GET_LDT(x) (*(void **)x)

/* #define DEBUG_EXTRA 1 */

#define MAX_PRESENT_SWAP_SEMAPHORE_COUNT 32
static VkPipelineStageFlagBits semaphore_dst_stage_masks[MAX_PRESENT_SWAP_SEMAPHORE_COUNT];

static bool vulkan_seen = false;

/* ======================================================================== */
/* hook data                                                                */

struct vk_obj_node {
    uint64_t obj;
    struct vk_obj_node *next;
};

struct vk_obj_list {
    struct vk_obj_node *root;
    pthread_mutex_t mutex;
};

struct vk_swap_data {
    struct vk_obj_node node;

    VkExtent2D image_extent;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkImage *swap_images;
    uint32_t image_count;

    // CPU capture path
    VkImage staging_image;
    VkDeviceMemory staging_mem;
    void *staging_map;
    VkDeviceSize staging_row_pitch;
    VkDeviceSize staging_offset;
};

struct vk_queue_data {
    struct vk_obj_node node;

    uint32_t fam_idx;
};

struct vk_inst_data {
    struct vk_obj_node node;

    VkInstance instance;

    bool valid;

    struct vk_inst_funcs funcs;
};

struct vk_data {
    struct vk_obj_node node;

    VkDevice device;

    bool valid;

    struct vk_device_funcs funcs;
    VkPhysicalDevice phy_device;
    struct vk_obj_list swaps;
    struct vk_swap_data *cur_swap;

    struct vk_obj_list queues;
    VkQueue graphics_queue;

    // CPU capture path
    VkCommandPool cpu_cmd_pool;
    VkCommandBuffer cpu_cmd_buffer;
    VkSemaphore cpu_semaphore;
    VkFence cpu_fence;

    struct vk_inst_data *inst_data;

    VkAllocationCallbacks ac_storage;
    const VkAllocationCallbacks *ac;
};

// ===============================================================

static void *vk_alloc(const VkAllocationCallbacks *ac, size_t size,
        size_t alignment, enum VkSystemAllocationScope scope)
{
    return ac ? ac->pfnAllocation(ac->pUserData, size, alignment, scope)
        : malloc(size);
}

static void vk_free(const VkAllocationCallbacks *ac, void *memory)
{
    if (ac)
        ac->pfnFree(ac->pUserData, memory);
    else
        free(memory);
}

static void add_obj_data(struct vk_obj_list *list, uint64_t obj, void *data)
{
    pthread_mutex_lock(&list->mutex);

    struct vk_obj_node *const node = (struct vk_obj_node*)data;
    node->obj = obj;
    node->next = list->root;
    list->root = node;

    pthread_mutex_unlock(&list->mutex);
}

static struct vk_obj_node *get_obj_data(struct vk_obj_list *list, uint64_t obj)
{
    struct vk_obj_node *data = NULL;

    pthread_mutex_lock(&list->mutex);

    struct vk_obj_node *node = list->root;
    while (node) {
        if (node->obj == obj) {
            data = node;
            break;
        }

        node = node->next;
    }

    pthread_mutex_unlock(&list->mutex);

    return data;
}

static struct vk_obj_node *remove_obj_data(struct vk_obj_list *list,
        uint64_t obj)
{
    struct vk_obj_node *data = NULL;

    pthread_mutex_lock(&list->mutex);

    struct vk_obj_node *prev = NULL;
    struct vk_obj_node *node = list->root;
    while (node) {
        if (node->obj == obj) {
            data = node;
            if (prev)
                prev->next = node->next;
            else
                list->root = node->next;
            break;
        }

        prev = node;
        node = node->next;
    }

    pthread_mutex_unlock(&list->mutex);

    return data;
}

static void init_obj_list(struct vk_obj_list *list)
{
    list->root = NULL;
    pthread_mutex_init(&list->mutex, NULL);
}

static struct vk_obj_node *obj_walk_begin(struct vk_obj_list *list)
{
    pthread_mutex_lock(&list->mutex);
    return list->root;
}

static struct vk_obj_node *obj_walk_next(struct vk_obj_node *node)
{
    return node->next;
}

static void obj_walk_end(struct vk_obj_list *list)
{
    pthread_mutex_unlock(&list->mutex);
}

// ===============================================================

static struct vk_obj_list devices;

static struct vk_data *alloc_device_data(const VkAllocationCallbacks *ac)
{
    struct vk_data *data = vk_alloc(ac, sizeof(struct vk_data),
            _Alignof(struct vk_data),
            VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    return data;
}

static void init_device_data(struct vk_data *data, VkDevice device)
{
    add_obj_data(&devices, (uintptr_t)GET_LDT(device), data);
    data->device = device;

    data->cpu_cmd_pool = VK_NULL_HANDLE;
    data->cpu_cmd_buffer = VK_NULL_HANDLE;
    data->cpu_semaphore = VK_NULL_HANDLE;
    data->cpu_fence = VK_NULL_HANDLE;
}

static struct vk_data *get_device_data(VkDevice device)
{
    return (struct vk_data *)get_obj_data(&devices,
            (uintptr_t)GET_LDT(device));
}

static struct vk_data *get_device_data_by_queue(VkQueue queue)
{
    return (struct vk_data *)get_obj_data(&devices,
            (uintptr_t)GET_LDT(queue));
}

static struct vk_data *remove_device_data(VkDevice device)
{
    return (struct vk_data *)remove_obj_data(&devices,
            (uintptr_t)GET_LDT(device));
}

// ===============================================================

static struct vk_queue_data *add_queue_data(struct vk_data *data, VkQueue queue,
        uint32_t fam_idx,
        bool supports_graphics,
        const VkAllocationCallbacks *ac)
{
    struct vk_queue_data *const queue_data =
        vk_alloc(ac, sizeof(struct vk_queue_data),
                _Alignof(struct vk_queue_data),
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    add_obj_data(&data->queues, (uintptr_t)queue, queue_data);
    queue_data->fam_idx = fam_idx;
    if (supports_graphics)
        data->graphics_queue = queue;
    return queue_data;
}

static struct vk_queue_data *get_queue_data(struct vk_data *data, VkQueue queue)
{
    return (struct vk_queue_data *)get_obj_data(&data->queues,
            (uintptr_t)queue);
}

static void remove_free_queue_all(struct vk_data *data,
        const VkAllocationCallbacks *ac)
{
    struct vk_queue_data *queue_data =
        (struct vk_queue_data *)data->queues.root;
    while (data->queues.root) {
        remove_obj_data(&data->queues, queue_data->node.obj);
        vk_free(ac, queue_data);

        queue_data = (struct vk_queue_data *)data->queues.root;
    }
}

// ===============================================================

static struct vk_swap_data *alloc_swap_data(const VkAllocationCallbacks *ac)
{
    struct vk_swap_data *const swap_data = vk_alloc(
            ac, sizeof(struct vk_swap_data), _Alignof(struct vk_swap_data),
            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    return swap_data;
}

static void init_swap_data(struct vk_swap_data *swap_data, struct vk_data *data,
        VkSwapchainKHR sc)
{
    add_obj_data(&data->swaps, (uint64_t)sc, swap_data);
}

static struct vk_swap_data *get_swap_data(struct vk_data *data,
        VkSwapchainKHR sc)
{
    return (struct vk_swap_data *)get_obj_data(&data->swaps, (uint64_t)sc);
}

static void remove_free_swap_data(struct vk_data *data, VkSwapchainKHR sc,
        const VkAllocationCallbacks *ac)
{
    struct vk_swap_data *const swap_data =
        (struct vk_swap_data *)remove_obj_data(&data->swaps,
                (uint64_t)sc);
    vk_free(ac, swap_data);
}

static struct vk_swap_data *swap_walk_begin(struct vk_data *data)
{
    return (struct vk_swap_data *)obj_walk_begin(&data->swaps);
}

static struct vk_swap_data *swap_walk_next(struct vk_swap_data *swap_data)
{
    return (struct vk_swap_data *)obj_walk_next(
            (struct vk_obj_node *)swap_data);
}

static void swap_walk_end(struct vk_data *data)
{
    obj_walk_end(&data->swaps);
}

// ===============================================================

static struct vk_obj_list instances;

static struct vk_inst_data *alloc_inst_data(const VkAllocationCallbacks *ac)
{
    struct vk_inst_data *idata = vk_alloc(
            ac, sizeof(struct vk_inst_data), _Alignof(struct vk_inst_data),
            VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    return idata;
}

static void init_inst_data(struct vk_inst_data *idata, VkInstance instance)
{
    add_obj_data(&instances, (uintptr_t)GET_LDT(instance), idata);
    idata->instance = instance;
}

static struct vk_inst_data *get_inst_data(VkInstance instance)
{
    return (struct vk_inst_data *)get_obj_data(&instances,
            (uintptr_t)GET_LDT(instance));
}

static struct vk_inst_funcs *get_inst_funcs(VkInstance instance)
{
    struct vk_inst_data *idata =
        (struct vk_inst_data *)get_inst_data(instance);
    return &idata->funcs;
}

static struct vk_inst_data *
get_inst_data_by_physical_device(VkPhysicalDevice physicalDevice)
{
    return (struct vk_inst_data *)get_obj_data(
            &instances, (uintptr_t)GET_LDT(physicalDevice));
}

static struct vk_inst_funcs *
get_inst_funcs_by_physical_device(VkPhysicalDevice physicalDevice)
{
    struct vk_inst_data *idata =
        (struct vk_inst_data *)get_inst_data_by_physical_device(
                physicalDevice);
    return &idata->funcs;
}

static void remove_free_inst_data(VkInstance inst,
        const VkAllocationCallbacks *ac)
{
    struct vk_inst_data *idata = (struct vk_inst_data *)remove_obj_data(
            &instances, (uintptr_t)GET_LDT(inst));
    vk_free(ac, idata);
}

// ===============================================================
/* CPU capture path (see this file's header comment): one                   */
/* HOST_VISIBLE|HOST_COHERENT linear staging image per swapchain, one       */
/* command buffer/fence per device, used only when capture_frame_requested  */

static bool vk_cpu_init(struct vk_data *data, struct vk_swap_data *swap)
{
    struct vk_device_funcs *funcs = &data->funcs;
    VkDevice device = data->device;

#ifndef NDEBUG
    hlog("Texture (CPU) %ux%u", swap->image_extent.width, swap->image_extent.height);
#endif

    VkImageCreateInfo img_info = {0};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    img_info.extent.width = swap->image_extent.width;
    img_info.extent.height = swap->image_extent.height;
    img_info.extent.depth = 1;
    img_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    img_info.tiling = VK_IMAGE_TILING_LINEAR;

    VkResult res = funcs->CreateImage(device, &img_info, data->ac, &swap->staging_image);
    if (res != VK_SUCCESS) {
        hlog("Failed to CreateImage (CPU staging): %s", result_to_str(res));
        swap->staging_image = VK_NULL_HANDLE;
        return false;
    }

    VkImageMemoryRequirementsInfo2 memri = {0};
    memri.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    memri.image = swap->staging_image;

    VkMemoryRequirements2 memr = {0};
    memr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    funcs->GetImageMemoryRequirements2KHR(device, &memri, &memr);

    struct vk_inst_funcs *ifuncs = get_inst_funcs_by_physical_device(data->phy_device);
    VkPhysicalDeviceMemoryProperties pdmp;
    ifuncs->GetPhysicalDeviceMemoryProperties(data->phy_device, &pdmp);

    const uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < pdmp.memoryTypeCount; ++i) {
        if ((memr.memoryRequirements.memoryTypeBits & (1u << i)) &&
                (pdmp.memoryTypes[i].propertyFlags & want) == want) {
            mem_type = i;
            break;
        }
    }
    if (mem_type == UINT32_MAX) {
        hlog("No host-visible+coherent memory type for CPU staging image");
        funcs->DestroyImage(device, swap->staging_image, data->ac);
        swap->staging_image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo memi = {0};
    memi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memi.allocationSize = memr.memoryRequirements.size;
    memi.memoryTypeIndex = mem_type;
    res = funcs->AllocateMemory(device, &memi, NULL, &swap->staging_mem);
    if (res != VK_SUCCESS) {
        hlog("AllocateMemory failed (CPU staging): %s", result_to_str(res));
        funcs->DestroyImage(device, swap->staging_image, data->ac);
        swap->staging_image = VK_NULL_HANDLE;
        return false;
    }

    VkBindImageMemoryInfo bimi = {0};
    bimi.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bimi.image = swap->staging_image;
    bimi.memory = swap->staging_mem;
    res = funcs->BindImageMemory2KHR(device, 1, &bimi);
    if (res != VK_SUCCESS) {
        hlog("BindImageMemory2KHR failed (CPU staging): %s", result_to_str(res));
        funcs->FreeMemory(device, swap->staging_mem, NULL);
        funcs->DestroyImage(device, swap->staging_image, data->ac);
        swap->staging_mem = VK_NULL_HANDLE;
        swap->staging_image = VK_NULL_HANDLE;
        return false;
    }

    VkImageSubresource sbr = {0};
    sbr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout;
    funcs->GetImageSubresourceLayout(device, swap->staging_image, &sbr, &layout);
    swap->staging_row_pitch = layout.rowPitch;
    swap->staging_offset = layout.offset;

    res = funcs->MapMemory(device, swap->staging_mem, 0, VK_WHOLE_SIZE, 0, &swap->staging_map);
    if (res != VK_SUCCESS) {
        hlog("MapMemory failed (CPU staging): %s", result_to_str(res));
        funcs->FreeMemory(device, swap->staging_mem, NULL);
        funcs->DestroyImage(device, swap->staging_image, data->ac);
        swap->staging_mem = VK_NULL_HANDLE;
        swap->staging_image = VK_NULL_HANDLE;
        return false;
    }

    data->cur_swap = swap;
    capture_frame_capture_started();
#ifndef NDEBUG
    hlog("------------------ vulkan capture started (CPU) ------------------");
#endif
    return true;
}

static void vk_cpu_free(struct vk_data *data)
{
    struct vk_swap_data *swap = swap_walk_begin(data);

    while (swap) {
        VkDevice device = data->device;
        if (swap->staging_map) {
            data->funcs.UnmapMemory(device, swap->staging_mem);
            swap->staging_map = NULL;
        }
        if (swap->staging_mem) {
            data->funcs.FreeMemory(device, swap->staging_mem, NULL);
            swap->staging_mem = VK_NULL_HANDLE;
        }
        if (swap->staging_image) {
            data->funcs.DestroyImage(device, swap->staging_image, data->ac);
            swap->staging_image = VK_NULL_HANDLE;
        }

        swap = swap_walk_next(swap);
    }

    swap_walk_end(data);

    data->cur_swap = NULL;
    capture_stop();

#ifndef NDEBUG
    hlog("------------------- vulkan capture freed (CPU) -------------------");
#endif
}

static bool vk_cpu_capture(struct vk_data *data, struct vk_device_funcs *funcs,
        struct vk_swap_data *swap, uint32_t idx,
        VkQueue queue, VkPresentInfoKHR *info)
{
    VkDevice device = data->device;

    struct vk_queue_data *queue_data = get_queue_data(data, queue);
    const uint32_t fam_idx = queue_data->fam_idx;

    if (data->cpu_cmd_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo cpci = {0};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.queueFamilyIndex = fam_idx;
        if (funcs->CreateCommandPool(device, &cpci, data->ac, &data->cpu_cmd_pool) != VK_SUCCESS) {
            hlog("CreateCommandPool failed (CPU capture)");
            return false;
        }

        VkCommandBufferAllocateInfo cbai = {0};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = data->cpu_cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (funcs->AllocateCommandBuffers(device, &cbai, &data->cpu_cmd_buffer) != VK_SUCCESS) {
            hlog("AllocateCommandBuffers failed (CPU capture)");
            return false;
        }
        GET_LDT(data->cpu_cmd_buffer) = GET_LDT(device);

        VkFenceCreateInfo fci = {0};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (funcs->CreateFence(device, &fci, data->ac, &data->cpu_fence) != VK_SUCCESS) {
            hlog("CreateFence failed (CPU capture)");
            return false;
        }

        VkSemaphoreCreateInfo sci = {0};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (funcs->CreateSemaphore(device, &sci, data->ac, &data->cpu_semaphore) != VK_SUCCESS) {
            hlog("CreateSemaphore failed (CPU capture)");
            return false;
        }
    }

    const uint32_t image_index = info->pImageIndices[idx];
    VkImage cur_backbuffer = swap->swap_images[image_index];

    funcs->ResetCommandPool(device, data->cpu_cmd_pool, 0);

    VkCommandBufferBeginInfo begin_info = {0};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    funcs->BeginCommandBuffer(data->cpu_cmd_buffer, &begin_info);

    VkImageMemoryBarrier mb[2] = {0};
    VkImageMemoryBarrier *src_mb = &mb[0];
    VkImageMemoryBarrier *dst_mb = &mb[1];

    src_mb->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    src_mb->srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    src_mb->dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_mb->oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    src_mb->newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_mb->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_mb->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_mb->image = cur_backbuffer;
    src_mb->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    src_mb->subresourceRange.levelCount = 1;
    src_mb->subresourceRange.layerCount = 1;

    /* ------------------------------------------------------ */
    /* transition exportedTexture to transfer dest state      */

    dst_mb->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dst_mb->srcAccessMask = 0;
    dst_mb->dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_mb->oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dst_mb->newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_mb->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_mb->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_mb->image = swap->staging_image;
    dst_mb->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dst_mb->subresourceRange.levelCount = 1;
    dst_mb->subresourceRange.layerCount = 1;

    funcs->CmdPipelineBarrier(data->cpu_cmd_buffer,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 2, mb);

    /* ------------------------------------------------------ */
    /* copy cur_backbuffer's content to our interop image     */

    if (swap->format != VK_FORMAT_B8G8R8A8_UNORM) {
        VkImageBlit blt = {0};
        blt.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blt.srcSubresource.layerCount = 1;
        blt.srcOffsets[1].x = (int32_t)swap->image_extent.width;
        blt.srcOffsets[1].y = (int32_t)swap->image_extent.height;
        blt.srcOffsets[1].z = 1;
        blt.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blt.dstSubresource.layerCount = 1;
        blt.dstOffsets[1].x = (int32_t)swap->image_extent.width;
        blt.dstOffsets[1].y = (int32_t)swap->image_extent.height;
        blt.dstOffsets[1].z = 1;
        funcs->CmdBlitImage(data->cpu_cmd_buffer, cur_backbuffer,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                swap->staging_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blt, VK_FILTER_NEAREST);
    } else {
        VkImageCopy cpy = {0};
        cpy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cpy.srcSubresource.layerCount = 1;
        cpy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cpy.dstSubresource.layerCount = 1;
        cpy.extent.width = swap->image_extent.width;
        cpy.extent.height = swap->image_extent.height;
        cpy.extent.depth = 1;
        funcs->CmdCopyImage(data->cpu_cmd_buffer, cur_backbuffer,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                swap->staging_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &cpy);
    }

    /* ------------------------------------------------------ */
    /* Restore the swap chain image layout to what it was
     * before.  This may not be strictly needed, but it is
     * generally good to restore things to their original
     * state.  */

    src_mb->srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    src_mb->dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    src_mb->oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_mb->newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    dst_mb->srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_mb->dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    dst_mb->oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_mb->newLayout = VK_IMAGE_LAYOUT_GENERAL;

    funcs->CmdPipelineBarrier(data->cpu_cmd_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, NULL, 0, NULL, 2, mb);

    funcs->EndCommandBuffer(data->cpu_cmd_buffer);

    /* Wait on the same semaphores the real present would have waited on
     * (the game's "rendering to this backbuffer is done" signal) so the
     * copy above can't run early, then redirect the present to wait on
     * our own signal instead - a binary semaphore can only be waited on
     * once, so the real QueuePresentKHR below can't also wait on the ones
     * we just consumed. We also wait on cpu_fence (below) before
     * returning, because capture_write_frame() needs this frame's pixels
     * now. */
    VkSubmitInfo submit_info = {0};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &data->cpu_cmd_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &data->cpu_semaphore;

    if (info->waitSemaphoreCount <= MAX_PRESENT_SWAP_SEMAPHORE_COUNT) {
        submit_info.waitSemaphoreCount = info->waitSemaphoreCount;
        submit_info.pWaitSemaphores = info->pWaitSemaphores;
        submit_info.pWaitDstStageMask = semaphore_dst_stage_masks;

        info->waitSemaphoreCount = 1;
        info->pWaitSemaphores = &data->cpu_semaphore;
    }

    funcs->ResetFences(device, 1, &data->cpu_fence);
    VkResult res = funcs->QueueSubmit(queue, 1, &submit_info, data->cpu_fence);
    if (res != VK_SUCCESS) {
        hlog("QueueSubmit failed (CPU capture): %s", result_to_str(res));
        return false;
    }

    res = funcs->WaitForFences(device, 1, &data->cpu_fence, VK_TRUE, ~0ull);
    if (res != VK_SUCCESS) {
        hlog("WaitForFences failed (CPU capture): %s", result_to_str(res));
        return false;
    }

    const uint8_t *src = (const uint8_t*)swap->staging_map + swap->staging_offset;
    return capture_write_frame((int)swap->image_extent.width, (int)swap->image_extent.height,
            src, (int)swap->staging_row_pitch);
}

/* ======================================================================== */
/* capture                                                                  */

static inline bool valid_rect(struct vk_swap_data *swap)
{
    return !!swap->image_extent.width && !!swap->image_extent.height &&
        (swap->image_extent.width > 1 || swap->image_extent.height > 1);
}

/* Overlay (ImGui) — implemented in overlay_vk.cpp. Hidden visibility lets
 * the linker resolve calls in-DSO directly, instead of going through the
 * global symbol table (which would be empty after vklayer.version filters
 * everything except AT_Negotiate out) */
#pragma GCC visibility push(hidden)
extern void at_overlay_vk_draw(struct vk_data *data, VkQueue queue,
        VkPresentInfoKHR *info);
#pragma GCC visibility pop

static void vk_capture(struct vk_data *data, VkQueue queue,
        VkPresentInfoKHR *info)
{
    // Use first swapchain ??
    struct vk_swap_data *swap = get_swap_data(data, info->pSwapchains[0]);

    capture_update_socket();

    if (capture_should_stop()) {
        vk_cpu_free(data);
    }

    if (capture_should_init()) {
        if (valid_rect(swap) && !vk_cpu_init(data, swap)) {
            vk_cpu_free(data);
            data->valid = false;
            hlog("vk_cpu_init failed");
        }
    }

    if (capture_ready()) {
        if (swap != data->cur_swap) {
            vk_cpu_free(data);
            return;
        }

        if (capture_frame_requested()) {
            if (vk_cpu_capture(data, &data->funcs, swap, 0, queue, info)) {
                capture_frame_delivered();
            }
        }
    }

    /* Draw ImGui overlay on top of the swapchain image just before vkQueuePresentKHR */
    at_overlay_vk_draw(data, queue, info);
}

static VkResult VKAPI_CALL AT_QueuePresentKHR(VkQueue queue,
        const VkPresentInfoKHR *info)
{
    VkPresentInfoKHR api = *info;

    struct vk_data *const data = get_device_data_by_queue(queue);
    struct vk_device_funcs *const funcs = &data->funcs;

    if (data->valid) {
        vk_capture(data, data->graphics_queue ? data->graphics_queue : queue, &api);
    }

    return funcs->QueuePresentKHR(queue, &api);
}

/* ======================================================================== */
/* setup hooks                                                              */

static inline bool is_inst_link_info(VkLayerInstanceCreateInfo *lici)
{
    return lici->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
        lici->function == VK_LAYER_LINK_INFO;
}

static VkResult VKAPI_CALL AT_CreateInstance(const VkInstanceCreateInfo *info,
        const VkAllocationCallbacks *ac,
        VkInstance *p_inst)
{
#ifndef NDEBUG
    hlog("CreateInstance");
#endif

    // ===============================================================
    /* step through chain until we get to the link info         */

    VkLayerInstanceCreateInfo *lici = (VkLayerInstanceCreateInfo *)info->pNext;
    while (lici && !is_inst_link_info(lici)) {
        lici = (VkLayerInstanceCreateInfo *)lici->pNext;
    }

    if (lici == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr gpa =
        lici->u.pLayerInfo->pfnNextGetInstanceProcAddr;

    // ===============================================================
    /* move chain on for next layer                             */

    lici->u.pLayerInfo = lici->u.pLayerInfo->pNext;

    // ===============================================================
    /* allocate data node                                       */

    struct vk_inst_data *idata = alloc_inst_data(ac);
    if (!idata) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // ===============================================================
    /* create instance                                          */

    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gpa(NULL, "vkCreateInstance");

    VkResult res = create(info, ac, p_inst);
#ifndef NDEBUG
    hlog("CreateInstance %s", result_to_str(res));
#endif
    if (res != VK_SUCCESS) {
        vk_free(ac, idata);
        return res;
    }

    VkInstance inst = *p_inst;
    init_inst_data(idata, inst);

    // ===============================================================
    /* fetch the functions we need                              */

    struct vk_inst_funcs *ifuncs = &idata->funcs;

#define GETADDR(x)                                      \
    do {                                            \
        ifuncs->x = (PFN_vk##x)gpa(inst, "vk" #x); \
        if (!ifuncs->x) {                       \
            hlog("could not get instance "  \
                    "address for vk" #x);      \
            funcs_found = false;            \
        }                                       \
    } while (false)

    bool funcs_found = true;
    GETADDR(GetInstanceProcAddr);
    GETADDR(DestroyInstance);
    GETADDR(GetPhysicalDeviceQueueFamilyProperties);
    GETADDR(GetPhysicalDeviceMemoryProperties);
#undef GETADDR

    idata->valid = funcs_found;

    return res;
}

static void VKAPI_CALL AT_DestroyInstance(VkInstance instance,
        const VkAllocationCallbacks *ac)
{
#ifndef NDEBUG
    hlog("DestroyInstance");
#endif

    struct vk_inst_funcs *ifuncs = get_inst_funcs(instance);
    PFN_vkDestroyInstance destroy_instance = ifuncs->DestroyInstance;

    remove_free_inst_data(instance, ac);

    destroy_instance(instance, ac);
}

static inline bool is_device_link_info(VkLayerDeviceCreateInfo *lici)
{
    return lici->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
        lici->function == VK_LAYER_LINK_INFO;
}

static VkResult VKAPI_CALL AT_CreateDevice(VkPhysicalDevice phy_device,
        const VkDeviceCreateInfo *info,
        const VkAllocationCallbacks *ac,
        VkDevice *p_device)
{
#ifndef NDEBUG
    hlog("CreateDevice");
#endif

    struct vk_inst_data *idata =
        get_inst_data_by_physical_device(phy_device);
    struct vk_inst_funcs *ifuncs = &idata->funcs;
    struct vk_data *data = NULL;

    const char *req_extensions[] = {
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
    };
    static uint32_t req_extensions_count = 3;

    int new_count = info->enabledExtensionCount + req_extensions_count;
    const char **exts = (const char**)malloc(sizeof(char*) * new_count);
    memcpy(exts, info->ppEnabledExtensionNames, sizeof(char*) * info->enabledExtensionCount);
    for (uint32_t i = 0; i < req_extensions_count; ++i) {
        exts[info->enabledExtensionCount + i] = req_extensions[i];
    }

    VkDeviceCreateInfo create_info = *info;
    create_info.enabledExtensionCount = new_count;
    create_info.ppEnabledExtensionNames = exts;

    VkResult ret = VK_ERROR_INITIALIZATION_FAILED;

    VkLayerDeviceCreateInfo *ldci = (VkLayerDeviceCreateInfo*)info->pNext;

    // ===============================================================
    /* step through chain until we get to the link info         */

    while (ldci && !is_device_link_info(ldci)) {
        ldci = (VkLayerDeviceCreateInfo *)ldci->pNext;
    }

    if (!ldci) {
        free(exts);
        return ret;
    }

    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;

    gipa = ldci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    gdpa = ldci->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    // ===============================================================
    /* move chain on for next layer                             */

    ldci->u.pLayerInfo = ldci->u.pLayerInfo->pNext;

    // ===============================================================
    /* allocate data node                                       */

    data = alloc_device_data(ac);
    if (!data) {
        free(exts);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    init_obj_list(&data->queues);
    data->graphics_queue = VK_NULL_HANDLE;

    // ===============================================================
    /* create device and initialize hook data                   */

    PFN_vkCreateDevice createFunc =
        (PFN_vkCreateDevice)gipa(idata->instance, "vkCreateDevice");

    ret = createFunc(phy_device, &create_info, ac, p_device);
    free(exts);
#ifndef NDEBUG
    hlog("CreateDevice %s", result_to_str(ret));
#endif
    if (ret != VK_SUCCESS) {
        vk_free(ac, data);
        return ret;
    }

    VkDevice device = *p_device;
    init_device_data(data, device);

    data->valid = false; /* set true below if it doesn't go to fail */
    data->phy_device = phy_device;

    // ===============================================================
    /* fetch the functions we need                              */

    struct vk_device_funcs *dfuncs = &data->funcs;
    bool funcs_found = true;

#define GETADDR(x)                                         \
    do {                                               \
        dfuncs->x = (PFN_vk##x)gdpa(device, "vk" #x); \
        if (!dfuncs->x) {                          \
            hlog("could not get device "       \
                    "address for vk" #x);         \
            funcs_found = false;               \
        }                                          \
    } while (false)

    GETADDR(GetDeviceProcAddr);
    GETADDR(DestroyDevice);
    GETADDR(CreateSwapchainKHR);
    GETADDR(DestroySwapchainKHR);
    GETADDR(QueuePresentKHR);
    GETADDR(AllocateMemory);
    GETADDR(FreeMemory);
    GETADDR(MapMemory);
    GETADDR(UnmapMemory);
    GETADDR(BindImageMemory2KHR);
    GETADDR(GetSwapchainImagesKHR);
    GETADDR(CreateImage);
    GETADDR(DestroyImage);
    GETADDR(GetImageMemoryRequirements2KHR);
    GETADDR(ResetCommandPool);
    GETADDR(BeginCommandBuffer);
    GETADDR(EndCommandBuffer);
    GETADDR(CmdCopyImage);
    GETADDR(CmdBlitImage);
    GETADDR(CmdPipelineBarrier);
    GETADDR(GetDeviceQueue);
    GETADDR(QueueSubmit);
    GETADDR(CreateCommandPool);
    GETADDR(DestroyCommandPool);
    GETADDR(AllocateCommandBuffers);
    GETADDR(CreateFence);
    GETADDR(DestroyFence);
    GETADDR(WaitForFences);
    GETADDR(ResetFences);
    GETADDR(GetImageSubresourceLayout);
    GETADDR(CreateSemaphore);
    GETADDR(DestroySemaphore);

#undef GETADDR

    if (!funcs_found) {
        return ret;
    }

    if (!idata->valid) {
        hlog("instance not valid");
        return ret;
    }

    data->inst_data = idata;

    data->ac = NULL;
    if (ac) {
        data->ac_storage = *ac;
        data->ac = &data->ac_storage;
    }

    uint32_t queue_family_property_count = 0;
    ifuncs->GetPhysicalDeviceQueueFamilyProperties(
            phy_device, &queue_family_property_count, NULL);
    VkQueueFamilyProperties *queue_family_properties = (VkQueueFamilyProperties*)malloc(
            sizeof(VkQueueFamilyProperties) * queue_family_property_count);
    ifuncs->GetPhysicalDeviceQueueFamilyProperties(
            phy_device, &queue_family_property_count,
            queue_family_properties);

    for (uint32_t info_index = 0, info_count = info->queueCreateInfoCount;
            info_index < info_count; ++info_index) {
        const VkDeviceQueueCreateInfo *queue_info =
            &info->pQueueCreateInfos[info_index];
        for (uint32_t queue_index = 0,
                queue_count = queue_info->queueCount;
                queue_index < queue_count; ++queue_index) {
            const uint32_t family_index =
                queue_info->queueFamilyIndex;
            VkQueue queue;
            data->funcs.GetDeviceQueue(device, family_index,
                    queue_index, &queue);
            const bool supports_graphics =
                (queue_family_properties[family_index]
                 .queueFlags &
                 (VK_QUEUE_GRAPHICS_BIT)) != 0;
            add_queue_data(data, queue, family_index,
                    supports_graphics, ac);
        }
    }

    free(queue_family_properties);

    init_obj_list(&data->swaps);
    data->cur_swap = NULL;

    data->valid = true;

    return ret;
}

#pragma GCC visibility push(hidden)
extern void at_overlay_vk_device_destroyed(VkDevice device);
#pragma GCC visibility pop

static void VKAPI_CALL AT_DestroyDevice(VkDevice device,
        const VkAllocationCallbacks *ac)
{
#ifndef NDEBUG
    hlog("DestroyDevice");
#endif

    at_overlay_vk_device_destroyed(device);

    struct vk_data *data = remove_device_data(device);

    if (data->valid) {
        remove_free_queue_all(data, ac);

        vk_cpu_free(data);
        if (data->cpu_cmd_pool) {
            data->funcs.DestroyCommandPool(device, data->cpu_cmd_pool, data->ac);
        }
        if (data->cpu_semaphore) {
            data->funcs.DestroySemaphore(device, data->cpu_semaphore, data->ac);
        }
        if (data->cpu_fence) {
            data->funcs.DestroyFence(device, data->cpu_fence, data->ac);
        }
    }

    PFN_vkDestroyDevice destroy_device = data->funcs.DestroyDevice;

    vk_free(ac, data);

    destroy_device(device, ac);
}

static VkResult VKAPI_CALL
AT_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *cinfo,
        const VkAllocationCallbacks *ac, VkSwapchainKHR *p_sc)
{
    struct vk_data *data = get_device_data(device);
    struct vk_device_funcs *funcs = &data->funcs;
    if (!data->valid)
        return funcs->CreateSwapchainKHR(device, cinfo, ac, p_sc);

    VkSwapchainCreateInfoKHR info = *cinfo;
    info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult res = funcs->CreateSwapchainKHR(device, &info, ac, p_sc);
#ifndef NDEBUG
    hlog("CreateSwapchainKHR %s", result_to_str(res));
#endif
    if (res != VK_SUCCESS) {
        /* try again with original imageUsage flags */
        return funcs->CreateSwapchainKHR(device, cinfo, ac, p_sc);
    }

    VkSwapchainKHR sc = *p_sc;
    uint32_t count = 0;
    res = funcs->GetSwapchainImagesKHR(device, sc, &count, NULL);
#ifndef NDEBUG
    hlog("GetSwapchainImagesKHR %s", result_to_str(res));
#endif
    if ((res == VK_SUCCESS) && (count > 0)) {
        struct vk_swap_data *swap_data = alloc_swap_data(ac);
        if (swap_data) {
            init_swap_data(swap_data, data, sc);
            swap_data->swap_images = vk_alloc(
                    ac, count * sizeof(VkImage), _Alignof(VkImage),
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
            res = funcs->GetSwapchainImagesKHR(
                    device, sc, &count, swap_data->swap_images);
#ifndef NDEBUG
            hlog("GetSwapchainImagesKHR %s", result_to_str(res));
#endif
            swap_data->image_extent = cinfo->imageExtent;
            swap_data->format = cinfo->imageFormat;
            swap_data->color_space = cinfo->imageColorSpace;
            swap_data->image_count = count;
            swap_data->staging_image = VK_NULL_HANDLE;
            swap_data->staging_mem = VK_NULL_HANDLE;
            swap_data->staging_map = NULL;
            swap_data->staging_row_pitch = 0;
            swap_data->staging_offset = 0;
        }
    }

    return VK_SUCCESS;
}

#pragma GCC visibility push(hidden)
extern void at_overlay_vk_swapchain_destroyed(VkDevice device, VkSwapchainKHR sc);
#pragma GCC visibility pop

static void VKAPI_CALL AT_DestroySwapchainKHR(VkDevice device,
        VkSwapchainKHR sc,
        const VkAllocationCallbacks *ac)
{
#ifndef NDEBUG
    hlog("DestroySwapchainKHR");
#endif

    struct vk_data *data = get_device_data(device);
    struct vk_device_funcs *funcs = &data->funcs;
    PFN_vkDestroySwapchainKHR destroy_swapchain =
        funcs->DestroySwapchainKHR;

    if ((sc != VK_NULL_HANDLE) && data->valid) {
        at_overlay_vk_swapchain_destroyed(device, sc);

        struct vk_swap_data *swap = get_swap_data(data, sc);
        if (swap) {
            if (data->cur_swap == swap) {
                vk_cpu_free(data);
            }

            vk_free(ac, swap->swap_images);

            remove_free_swap_data(data, sc, ac);
        }
    }

    destroy_swapchain(device, sc, ac);
}

#define GETPROCADDR(func)               \
    if (!strcmp(pName, "vk" #func)) \
    return (PFN_vkVoidFunction)&AT_##func;

#define GETPROCADDR_IF_SUPPORTED(func)  \
    if (!strcmp(pName, "vk" #func)) \
    return funcs && funcs->func ? (PFN_vkVoidFunction)&AT_##func : NULL;

static PFN_vkVoidFunction VKAPI_CALL AT_GetDeviceProcAddr(VkDevice device, const char *pName)
{
    struct vk_data *data = get_device_data(device);
    struct vk_device_funcs *funcs = &data->funcs;

    GETPROCADDR(GetDeviceProcAddr);
    GETPROCADDR(DestroyDevice);
    GETPROCADDR_IF_SUPPORTED(CreateSwapchainKHR);
    GETPROCADDR_IF_SUPPORTED(DestroySwapchainKHR);
    GETPROCADDR_IF_SUPPORTED(QueuePresentKHR);

    if (funcs->GetDeviceProcAddr == NULL)
        return NULL;
    return funcs->GetDeviceProcAddr(device, pName);
}

static PFN_vkVoidFunction VKAPI_CALL AT_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
    /* instance chain functions we intercept */
    GETPROCADDR(GetInstanceProcAddr);
    GETPROCADDR(CreateInstance);

    struct vk_inst_funcs *const funcs = instance ? get_inst_funcs(instance) : NULL;

    /* other instance chain functions we intercept */
    GETPROCADDR(DestroyInstance);

    /* device chain functions we intercept */
    GETPROCADDR(GetDeviceProcAddr);
    GETPROCADDR(CreateDevice);
    GETPROCADDR(DestroyDevice);

    if (!funcs)
        return NULL;

    const PFN_vkGetInstanceProcAddr gipa = funcs->GetInstanceProcAddr;
    return gipa ? gipa(instance, pName) : NULL;
}

#undef GETPROCADDR

VKAPI_ATTR VkResult VKAPI_CALL AT_Negotiate(VkNegotiateLayerInterface *nli)
{
    if (nli->loaderLayerInterfaceVersion >= 2) {
        nli->sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
        nli->pNext = NULL;
        nli->pfnGetInstanceProcAddr = AT_GetInstanceProcAddr;
        nli->pfnGetDeviceProcAddr = AT_GetDeviceProcAddr;
        nli->pfnGetPhysicalDeviceProcAddr = NULL;
    }

    const uint32_t cur_ver = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

    if (nli->loaderLayerInterfaceVersion > cur_ver) {
        nli->loaderLayerInterfaceVersion = cur_ver;
    }

    if (!vulkan_seen) {
#ifndef NDEBUG
        hlog("Init Vulkan (%s)",
#ifdef __x86_64__
            "64bit");
#else
            "32bit");
#endif
#endif
        init_obj_list(&instances);
        init_obj_list(&devices);
        capture_init();

        vulkan_seen = true;

        for (int i = 0; i < MAX_PRESENT_SWAP_SEMAPHORE_COUNT; i++) {
            semaphore_dst_stage_masks[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
    }

    return VK_SUCCESS;
}

/* ======================================================================== */
/* Bridge for ImGui overlay (overlay_vk.cpp)                                */
/* ======================================================================== */

#include "overlay_bridge.h"

bool at_bridge_collect(const struct vk_data *data,
                        VkSwapchainKHR sc,
                        struct at_bridge_info *out)
{
    if (!data || !out) return false;
    if (!data->graphics_queue) return false;

    struct vk_swap_data *swap = get_swap_data((struct vk_data *)data, sc);
    if (!swap || !swap->swap_images || swap->image_count == 0) return false;

    out->device          = data->device;
    out->phy             = data->phy_device;
    out->instance        = data->inst_data ? data->inst_data->instance : VK_NULL_HANDLE;
    out->graphics_queue  = data->graphics_queue;

    struct vk_queue_data *qd = get_queue_data((struct vk_data *)data, data->graphics_queue);
    out->graphics_family = qd ? qd->fam_idx : 0;

    out->swapchain   = sc;
    out->extent      = swap->image_extent;
    out->format      = swap->format;
    out->image_count = swap->image_count;
    out->images      = swap->swap_images;

    out->gdpa = data->funcs.GetDeviceProcAddr;
    out->gipa = data->inst_data ? data->inst_data->funcs.GetInstanceProcAddr : NULL;
    return true;
}
