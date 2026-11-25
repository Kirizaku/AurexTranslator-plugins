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
 * AT adapter around the unmodified obs-vkcapture Vulkan layer
 * Capture creation, image export, barriers, and QueueSubmit remain upstream
 * This file supplies the AT protocol bridge and overlay renderer hooks
 */
 
#include "../../../../third_party/obs-vkcapture/vklayer.c"
extern bool capture_write_frame(int, int, const void *, int);
extern void capture_frame_delivered(void);
extern bool capture_frame_requested(void);
#include "vulkan_cpu_fallback.inc"

#include "overlay_bridge.h"

extern void at_overlay_vk_draw(struct vk_data *, VkQueue, VkPresentInfoKHR *);
extern void at_overlay_vk_device_destroyed(VkDevice);
extern void at_overlay_vk_swapchain_destroyed(VkDevice, VkSwapchainKHR);
extern void capture_stop(void);
extern void capture_gpu_frame_delivered(void);
extern bool capture_gpu_fallback_requested(void);
extern void capture_frame_capture_started(void);
extern void capture_set_method(const char *method);
extern void capture_set_device_uuid(const uint8_t uuid[16]);

static VkResult VKAPI_CALL at_vk_queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    struct vk_data *data = get_device_data_by_queue(queue);
    if (!data)
        return VK_ERROR_DEVICE_LOST;

    if (info->swapchainCount == 0)
        return data->funcs.QueuePresentKHR(queue, info);

    VkPresentInfoKHR present = *info;
    capture_update_socket();
    capture_set_device_uuid(data->device_uuid);
    struct vk_swap_data *presented_swap = get_swap_data(data, info->pSwapchains[0]);

    struct at_vk_cpu_state *cpu = at_vk_cpu_find(data->device);
    const bool cpu_active = cpu && cpu->swap;
    const bool use_cpu = capture_gpu_fallback_requested() || !data->valid || cpu_active;
    if (cpu_active && presented_swap && cpu->swap != presented_swap) {
        at_vk_cpu_release_swap(cpu);
        capture_stop();
    } else if (!cpu_active && data->cur_swap && presented_swap && data->cur_swap != presented_swap) {
        vk_shtex_free(data);
    }

    if (capture_should_stop()) {
        if (cpu_active) at_vk_cpu_release_swap(cpu);
        if (data->valid && !cpu_active) vk_capture(data, queue, &present);
        else capture_stop();
    } else if (use_cpu && presented_swap && capture_ready() && !cpu_active && data->cur_swap) {
        vk_shtex_free(data);
    }

    cpu = at_vk_cpu_find(data->device);
    const bool cpu_running = cpu && cpu->swap;
    const bool fallback = capture_gpu_fallback_requested() || !data->valid || cpu_running;
    bool gpu_frame_published = false;

    if (fallback && presented_swap && !capture_should_stop() &&
        (capture_ready() || capture_should_init())) {
        if (!cpu_running) {
            capture_stop();
            if (!at_vk_cpu_init(data, presented_swap)) {
                hlog("Vulkan CPU fallback initialization failed");
                capture_stop();
            } else {
                data->valid = true;
                capture_frame_capture_started();
                capture_set_method("CPU");
            }
        }
        cpu = at_vk_cpu_find(data->device);
        if (cpu && cpu->swap == presented_swap)
            (void)at_vk_cpu_collect(data, cpu, 0);
        if (cpu && cpu->swap == presented_swap && !cpu->capture_pending &&
            capture_frame_requested() &&
            at_vk_cpu_capture(data, presented_swap, present.pImageIndices[0], 
                              data->graphics_queue ? data->graphics_queue : queue, &present)) { }
    } else {
        const bool stopping = capture_should_stop();
        const bool initializing = capture_should_init();
        const bool requested = capture_ready() && capture_frame_requested();
        const bool capture_now = stopping || initializing || requested;
        if (data->valid && presented_swap && capture_now) {
            VkQueue capture_queue = data->graphics_queue ? data->graphics_queue : queue;
            if (present.waitSemaphoreCount <= MAX_PRESENT_SWAP_SEMAPHORE_COUNT) {
                for (uint32_t i = 0; i < present.waitSemaphoreCount; ++i)
                    semaphore_dst_stage_masks[i] = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            }
            vk_capture(data, capture_queue, &present);
            if (requested && !stopping && capture_ready() && data->cur_swap == presented_swap) {
                struct vk_queue_data *capture_queue_data = get_queue_data(data, capture_queue);
                if (capture_queue_data && capture_queue_data->frame_count > 0) {
                    const uint32_t frame_index =
                        (capture_queue_data->frame_index + capture_queue_data->frame_count - 1) %
                        capture_queue_data->frame_count;
                    struct vk_frame_data *frame = &capture_queue_data->frames[frame_index];
                    if (frame->cmd_buffer_busy) {
                        VkResult wait = data->funcs.WaitForFences(data->device, 1,
                            &frame->fence, VK_TRUE, UINT64_MAX);
                        gpu_frame_published = wait == VK_SUCCESS;
                    }
                }
            }
        }
        if (!data->valid && presented_swap && !capture_should_stop() &&
            at_vk_cpu_init(data, presented_swap)) {
            data->valid = true;
            capture_frame_capture_started();
            capture_set_method("CPU");
            cpu = at_vk_cpu_find(data->device);
            if (cpu && capture_frame_requested())
                at_vk_cpu_capture(data, presented_swap, present.pImageIndices[0],
                                  data->graphics_queue ? data->graphics_queue : queue, &present);
        }
    }

    at_overlay_vk_draw(data, queue, &present);
    const VkResult result = data->funcs.QueuePresentKHR(queue, &present);
    if (gpu_frame_published)
        capture_gpu_frame_delivered();
    return result;
}

static void VKAPI_CALL at_vk_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator)
{
    struct vk_data *data = get_device_data(device);
    struct at_vk_cpu_state *cpu = at_vk_cpu_find(device);
    if (data && cpu && cpu->swap == get_swap_data(data, swapchain)) {
        at_vk_cpu_release_swap(cpu);
        capture_stop();
    }
    if (data && data->cur_swap == get_swap_data(data, swapchain))
        vk_shtex_free(data);

    at_overlay_vk_swapchain_destroyed(device, swapchain);
    OBS_DestroySwapchainKHR(device, swapchain, allocator);
}

static void VKAPI_CALL at_vk_destroy_device(VkDevice device, const VkAllocationCallbacks *allocator)
{
    at_vk_cpu_destroy_device(device);
    at_overlay_vk_device_destroyed(device);
    OBS_DestroyDevice(device, allocator);
}

static PFN_vkVoidFunction VKAPI_CALL at_vk_get_device_proc_addr(VkDevice device, const char *name)
{
    if (!strcmp(name, "vkQueuePresentKHR"))
        return (PFN_vkVoidFunction)at_vk_queue_present;
    if (!strcmp(name, "vkDestroySwapchainKHR"))
        return (PFN_vkVoidFunction)at_vk_destroy_swapchain;
    if (!strcmp(name, "vkDestroyDevice"))
        return (PFN_vkVoidFunction)at_vk_destroy_device;
    return OBS_GetDeviceProcAddr(device, name);
}

static PFN_vkVoidFunction VKAPI_CALL at_vk_get_instance_proc_addr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)at_vk_get_device_proc_addr;

    return OBS_GetInstanceProcAddr(instance, name);
}

VKAPI_ATTR VkResult VKAPI_CALL AT_Negotiate(VkNegotiateLayerInterface *nli)
{
    const VkResult result = OBS_Negotiate(nli);
    if (result == VK_SUCCESS && nli->loaderLayerInterfaceVersion >= 2) {
        nli->pfnGetInstanceProcAddr = at_vk_get_instance_proc_addr;
        nli->pfnGetDeviceProcAddr = at_vk_get_device_proc_addr;
    }
    return result;
}

#include "vulkan_overlay_bridge.inc"
