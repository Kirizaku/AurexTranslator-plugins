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
Vulkan game-frame capture on Windows (interface)

Copies the presented image into a host-visible buffer with
vkCmdCopyImageToBuffer and hands it to capture_write_frame()

When it captures, vk_capture_present() returns a VkSemaphore: the copy waits
on (and consumes) the app's render-finished semaphores, so the present hook
must make Present() wait on ours instead, or the game hangs
*/

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <cstdint>

namespace at {

struct VkCaptureFrame {
    VkDevice                  device;
    VkPhysicalDevice          phy;
    VkInstance                instance;
    PFN_vkGetDeviceProcAddr   gdpa;
    PFN_vkGetInstanceProcAddr gipa;
    VkQueue                   queue; /* graphics queue for our submit */
    uint32_t                  queue_family;
    VkSwapchainKHR            swapchain;
    VkImage                   src_image; /* the image being presented */
    VkExtent2D                extent;
    VkFormat                  format; /* swapchain image format */

    /* The semaphores the present would wait on (the app's render-finished
     * signals). The capture submit waits on these so it copies a fully-rendered
     * frame */
    const VkSemaphore*        wait_semaphores;
    uint32_t                  wait_semaphore_count;
};

const char* const* vk_capture_device_extensions(uint32_t* count);
const char* const* vk_capture_instance_extensions(uint32_t* count);

VkSemaphore vk_capture_present(const VkCaptureFrame& f);

void vk_capture_swapchain_destroyed(VkDevice device, VkSwapchainKHR sc);
void vk_capture_device_destroyed(VkDevice device);

} // namespace at
