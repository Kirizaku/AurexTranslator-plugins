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

/* AT-only Vulkan readback fallback. The obs-vkcapture layer remains pristine */
struct at_vk_cpu_state {
    VkDevice device;
    VkImage staging_image;
    VkDeviceMemory memory;
    void *mapped;
    VkDeviceSize row_pitch;
    VkDeviceSize offset;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkFence fence;
    VkSemaphore *present_semaphores;
    uint32_t present_semaphore_count;
    bool capture_pending;
    struct vk_swap_data *swap;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    struct at_vk_cpu_state *next;
};

static struct at_vk_cpu_state *at_vk_cpu_states;
static bool at_vk_cpu_collect(struct vk_data *data, struct at_vk_cpu_state *s,
                              uint64_t timeout);

static struct at_vk_cpu_state *at_vk_cpu_find(VkDevice device)
{
    for (struct at_vk_cpu_state *s = at_vk_cpu_states; s; s = s->next)
        if (s->device == device) return s;
    return NULL;
}

static void at_vk_cpu_release_swap(struct at_vk_cpu_state *s)
{
    if (!s) return;
    struct vk_data *data = get_device_data(s->device);
    if (data && s->capture_pending)
        (void)at_vk_cpu_collect(data, s, UINT64_MAX);
    if (data && s->mapped && s->memory) s->UnmapMemory(s->device, s->memory);
    if (data && s->present_semaphores) {
        for (uint32_t i = 0; i < s->present_semaphore_count; ++i) {
            if (s->present_semaphores[i])
                s->DestroySemaphore(s->device, s->present_semaphores[i], data->ac);
        }
    }
    free(s->present_semaphores);
    s->present_semaphores = NULL;
    s->present_semaphore_count = 0;
    s->capture_pending = false;
    s->mapped = NULL;
    if (data && s->staging_image)
        data->funcs.DestroyImage(s->device, s->staging_image, data->ac);
    if (data && s->memory) data->funcs.FreeMemory(s->device, s->memory, data->ac);
    s->staging_image = VK_NULL_HANDLE;
    s->memory = VK_NULL_HANDLE;
    s->row_pitch = 0;
    s->offset = 0;
    s->swap = NULL;
}

static bool at_vk_cpu_is_10bit(VkFormat format)
{
    return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
           format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}

static bool at_vk_cpu_init(struct vk_data *data, struct vk_swap_data *swap)
{
    if (swap->format != VK_FORMAT_B8G8R8A8_UNORM &&
        swap->format != VK_FORMAT_B8G8R8A8_SRGB &&
        swap->format != VK_FORMAT_R8G8B8A8_UNORM &&
        swap->format != VK_FORMAT_R8G8B8A8_SRGB &&
        !at_vk_cpu_is_10bit(swap->format)) {
        hlog("CPU fallback: unsupported swapchain format %d", (int)swap->format);
        return false;
    }

    struct at_vk_cpu_state *s = at_vk_cpu_find(data->device);
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s) return false;
        s->device = data->device;
#define AT_VK_CPU_LOAD(member, symbol) \
        s->member = (PFN_vk##symbol)data->funcs.GetDeviceProcAddr(data->device, "vk" #symbol)
        AT_VK_CPU_LOAD(MapMemory, MapMemory);
        AT_VK_CPU_LOAD(UnmapMemory, UnmapMemory);
        AT_VK_CPU_LOAD(DestroyFence, DestroyFence);
        AT_VK_CPU_LOAD(DestroySemaphore, DestroySemaphore);
        AT_VK_CPU_LOAD(DestroyCommandPool, DestroyCommandPool);
#undef AT_VK_CPU_LOAD
        if (!s->MapMemory || !s->UnmapMemory || !s->DestroyFence || !s->DestroySemaphore ||
            !s->DestroyCommandPool) {
            free(s);
            return false;
        }

        struct vk_queue_data *qd = get_queue_data(data, data->graphics_queue);
        if (!qd) { free(s); return false; }
        VkCommandPoolCreateInfo pool_info = {0};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = qd->fam_idx;
        if (data->funcs.CreateCommandPool(data->device, &pool_info, data->ac,
                                           &s->pool) != VK_SUCCESS) {
            free(s); return false;
        }
        VkCommandBufferAllocateInfo command_info = {0};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = s->pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        if (data->funcs.AllocateCommandBuffers(data->device, &command_info,
                                                &s->command) != VK_SUCCESS) {
            s->DestroyCommandPool(data->device, s->pool, data->ac);
            free(s); return false;
        }
        GET_LDT(s->command) = GET_LDT(data->device);
        VkFenceCreateInfo fence_info = {0};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (data->funcs.CreateFence(data->device, &fence_info, data->ac,
                                     &s->fence) != VK_SUCCESS) {
            s->DestroyCommandPool(data->device, s->pool, data->ac);
            free(s); return false;
        }
        s->next = at_vk_cpu_states;
        at_vk_cpu_states = s;
    }

    if (s->swap == swap && s->staging_image) return true;
    at_vk_cpu_release_swap(s);

    VkImageCreateInfo image_info = {0};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    image_info.extent = (VkExtent3D){swap->image_extent.width,
                                     swap->image_extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (data->funcs.CreateImage(data->device, &image_info, data->ac, &s->staging_image) != VK_SUCCESS)
        return false;

    VkImageMemoryRequirementsInfo2 requirements_info = {0};
    requirements_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    requirements_info.image = s->staging_image;
    VkMemoryRequirements2 requirements = {0};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    data->funcs.GetImageMemoryRequirements2KHR(data->device, &requirements_info,
                                                &requirements);
    struct vk_inst_funcs *ifuncs = get_inst_funcs_by_physical_device(data->phy_device);
    if (!ifuncs) { at_vk_cpu_release_swap(s); return false; }
    VkPhysicalDeviceMemoryProperties properties;
    ifuncs->GetPhysicalDeviceMemoryProperties(data->phy_device, &properties);
    uint32_t memory_type = UINT32_MAX;
    uint32_t cached_memory_type = UINT32_MAX;
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((requirements.memoryRequirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            if (memory_type == UINT32_MAX) memory_type = i;
            if (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
                cached_memory_type = i;
                break;
            }
        }
    }
    if (cached_memory_type != UINT32_MAX) memory_type = cached_memory_type;
    if (memory_type == UINT32_MAX) { at_vk_cpu_release_swap(s); return false; }

    VkMemoryAllocateInfo allocation = {0};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.memoryRequirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (data->funcs.AllocateMemory(data->device, &allocation, data->ac, &s->memory) != VK_SUCCESS) {
        at_vk_cpu_release_swap(s);
        return false;
    }

    VkBindImageMemoryInfo bind_info = {0};
    bind_info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bind_info.image = s->staging_image;
    bind_info.memory = s->memory;
    if (data->funcs.BindImageMemory2KHR(data->device, 1, &bind_info) != VK_SUCCESS) {
        at_vk_cpu_release_swap(s);
        return false;
    }
    VkImageSubresource subresource = {0};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout;
    data->funcs.GetImageSubresourceLayout(data->device, s->staging_image,
                                           &subresource, &layout);
    s->row_pitch = layout.rowPitch;
    s->offset = layout.offset;
    if (s->MapMemory(data->device, s->memory, 0, VK_WHOLE_SIZE, 0,
                     &s->mapped) != VK_SUCCESS) {
        at_vk_cpu_release_swap(s);
        return false;
    }
    s->swap = swap;
    hlog("CPU staging memory type %u flags 0x%x (host cached: %s)", memory_type,
         properties.memoryTypes[memory_type].propertyFlags,
         (properties.memoryTypes[memory_type].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "yes" : "no");
    hlog("Texture (CPU fallback) %ux%u", swap->image_extent.width, swap->image_extent.height);
    return true;
}

/* Poll the outstanding readback; steady-state presents never wait for it */
static bool at_vk_cpu_collect(struct vk_data *data, struct at_vk_cpu_state *s,
                              uint64_t timeout)
{
    if (!s || !s->capture_pending) return false;

    const VkResult result = data->funcs.WaitForFences(data->device, 1,
        &s->fence, VK_TRUE, timeout);
    if (result == VK_TIMEOUT) return false;
    if (result != VK_SUCCESS) {
        hlog("CPU fallback fence wait failed (VkResult=%d)", result);
        s->capture_pending = false;
        capture_stop();
        return false;
    }

    s->capture_pending = false;
    struct vk_swap_data *swap = s->swap;
    if (!swap || !s->mapped ||
        !capture_write_frame((int)swap->image_extent.width,
            (int)swap->image_extent.height,
            (const uint8_t *)s->mapped + s->offset, (int)s->row_pitch)) {
        return false;
    }

    capture_frame_delivered();
    return true;
}

static bool at_vk_cpu_capture(struct vk_data *data, struct vk_swap_data *swap,
                              uint32_t image_index, VkQueue queue,
                              VkPresentInfoKHR *present)
{
    struct at_vk_cpu_state *s = at_vk_cpu_find(data->device);
    if (!s || s->swap != swap || image_index >= swap->image_count) return false;
    if (s->capture_pending) return false;
    struct vk_queue_data *qd = get_queue_data(data, queue);
    if (!qd) return false;

    if (!s->present_semaphores) {
        s->present_semaphores = calloc(swap->image_count,
                                       sizeof(*s->present_semaphores));
        if (!s->present_semaphores) return false;
        s->present_semaphore_count = swap->image_count;
        VkSemaphoreCreateInfo semaphore_info = {0};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < s->present_semaphore_count; ++i) {
            if (!data->funcs.CreateSemaphore ||
                data->funcs.CreateSemaphore(data->device, &semaphore_info, data->ac,
                                             &s->present_semaphores[i]) != VK_SUCCESS) {
                hlog("CPU fallback semaphore creation failed");
                for (uint32_t j = 0; j < i; ++j)
                    s->DestroySemaphore(data->device, s->present_semaphores[j], data->ac);
                free(s->present_semaphores);
                s->present_semaphores = NULL;
                s->present_semaphore_count = 0;
                return false;
            }
        }
    }

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    data->funcs.ResetCommandPool(data->device, s->pool, 0);
    if (data->funcs.BeginCommandBuffer(s->command, &begin) != VK_SUCCESS) return false;

    VkImageMemoryBarrier barriers[2] = {0};
    VkImageMemoryBarrier *source = &barriers[0];
    source->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    source->srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    source->dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source->oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    source->newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source->image = swap->swap_images[image_index];
    source->subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier *staging = &barriers[1];
    staging->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    staging->dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    staging->oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    staging->newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    staging->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    staging->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    staging->image = s->staging_image;
    staging->subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    data->funcs.CmdPipelineBarrier(s->command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, barriers);

    if (swap->format == VK_FORMAT_B8G8R8A8_UNORM) {
        VkImageCopy copy = {0};
        copy.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = (VkExtent3D){swap->image_extent.width, swap->image_extent.height, 1};
        data->funcs.CmdCopyImage(s->command, source->image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->staging_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else {
        VkImageBlit blit = {0};
        blit.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.srcOffsets[1] = (VkOffset3D){(int32_t)swap->image_extent.width,
                                          (int32_t)swap->image_extent.height, 1};
        blit.dstOffsets[1] = blit.srcOffsets[1];
        data->funcs.CmdBlitImage(s->command, source->image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->staging_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }

    source->srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source->dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    source->oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source->newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    staging->srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    staging->dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    staging->oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    staging->newLayout = VK_IMAGE_LAYOUT_GENERAL;
    data->funcs.CmdPipelineBarrier(s->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 0, NULL, 2, barriers);
    if (data->funcs.EndCommandBuffer(s->command) != VK_SUCCESS) return false;

    VkPipelineStageFlags local_stages[8];
    VkPipelineStageFlags *stages = local_stages;
    if (present->waitSemaphoreCount > sizeof(local_stages) / sizeof(local_stages[0])) {
        stages = malloc(sizeof(*stages) * present->waitSemaphoreCount);
        if (!stages) return false;
    }
    for (uint32_t i = 0; i < present->waitSemaphoreCount; ++i)
        stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
        
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = present->waitSemaphoreCount;
    submit.pWaitSemaphores = present->pWaitSemaphores;
    submit.pWaitDstStageMask = stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s->command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &s->present_semaphores[image_index];
    data->funcs.ResetFences(data->device, 1, &s->fence);
    VkResult result = data->funcs.QueueSubmit(queue, 1, &submit, s->fence);
    if (stages != local_stages) free(stages);
    if (result != VK_SUCCESS) return false;
    /* Keep the application’s present semaphore chain intact */
    present->waitSemaphoreCount = 1;
    present->pWaitSemaphores = &s->present_semaphores[image_index];
    s->capture_pending = true;
    return true;
}

static void at_vk_cpu_destroy_device(VkDevice device)
{
    struct at_vk_cpu_state **link = &at_vk_cpu_states;
    while (*link) {
        struct at_vk_cpu_state *s = *link;
        if (s->device != device) { link = &s->next; continue; }
        struct vk_data *data = get_device_data(device);
        at_vk_cpu_release_swap(s);
        if (data && s->fence) s->DestroyFence(device, s->fence, data->ac);
        if (data && s->pool) s->DestroyCommandPool(device, s->pool, data->ac);
        *link = s->next;
        free(s);
        return;
    }
}
