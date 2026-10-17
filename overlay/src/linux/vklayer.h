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
NOTE: Derived from obs-vkcapture/src/vklayer.h, reduced to the functions
the CPU capture path uses (the DMA-BUF export and surface tracking are
removed, see vklayer.c)
*/

#pragma once

#include <stdbool.h>
#include <vulkan/vulkan.h>
#ifndef VK_EXT_image_drm_format_modifier
#define VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT -1000158000
#endif

#define DEF_FUNC(x) PFN_vk##x x

struct vk_inst_funcs {
    DEF_FUNC(GetInstanceProcAddr);
    DEF_FUNC(DestroyInstance);
    DEF_FUNC(GetPhysicalDeviceQueueFamilyProperties);
    DEF_FUNC(GetPhysicalDeviceMemoryProperties);
};

struct vk_device_funcs {
    DEF_FUNC(GetDeviceProcAddr);
    DEF_FUNC(DestroyDevice);
    DEF_FUNC(CreateSwapchainKHR);
    DEF_FUNC(DestroySwapchainKHR);
    DEF_FUNC(QueuePresentKHR);
    DEF_FUNC(AllocateMemory);
    DEF_FUNC(FreeMemory);
    DEF_FUNC(MapMemory);
    DEF_FUNC(UnmapMemory);
    DEF_FUNC(BindImageMemory2KHR);
    DEF_FUNC(GetSwapchainImagesKHR);
    DEF_FUNC(CreateImage);
    DEF_FUNC(DestroyImage);
    DEF_FUNC(GetImageMemoryRequirements2KHR);
    DEF_FUNC(ResetCommandPool);
    DEF_FUNC(BeginCommandBuffer);
    DEF_FUNC(EndCommandBuffer);
    DEF_FUNC(CmdCopyImage);
    DEF_FUNC(CmdBlitImage);
    DEF_FUNC(CmdPipelineBarrier);
    DEF_FUNC(GetDeviceQueue);
    DEF_FUNC(QueueSubmit);
    DEF_FUNC(CreateCommandPool);
    DEF_FUNC(DestroyCommandPool);
    DEF_FUNC(AllocateCommandBuffers);
    DEF_FUNC(CreateFence);
    DEF_FUNC(DestroyFence);
    DEF_FUNC(WaitForFences);
    DEF_FUNC(ResetFences);
    DEF_FUNC(GetImageSubresourceLayout);
    DEF_FUNC(CreateSemaphore);
    DEF_FUNC(DestroySemaphore);
};

#undef DEF_FUNC

static const char *result_to_str(VkResult result)
{
    switch (result) {
#define VAL(x) case x: return #x
    VAL(VK_SUCCESS);
    VAL(VK_NOT_READY);
    VAL(VK_TIMEOUT);
    VAL(VK_EVENT_SET);
    VAL(VK_EVENT_RESET);
    VAL(VK_INCOMPLETE);
    VAL(VK_ERROR_OUT_OF_HOST_MEMORY);
    VAL(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    VAL(VK_ERROR_INITIALIZATION_FAILED);
    VAL(VK_ERROR_DEVICE_LOST);
    VAL(VK_ERROR_MEMORY_MAP_FAILED);
    VAL(VK_ERROR_LAYER_NOT_PRESENT);
    VAL(VK_ERROR_EXTENSION_NOT_PRESENT);
    VAL(VK_ERROR_FEATURE_NOT_PRESENT);
    VAL(VK_ERROR_INCOMPATIBLE_DRIVER);
    VAL(VK_ERROR_TOO_MANY_OBJECTS);
    VAL(VK_ERROR_FORMAT_NOT_SUPPORTED);
    VAL(VK_ERROR_FRAGMENTED_POOL);
    VAL(VK_ERROR_OUT_OF_POOL_MEMORY);
    VAL(VK_ERROR_INVALID_EXTERNAL_HANDLE);
    VAL(VK_ERROR_SURFACE_LOST_KHR);
    VAL(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
    VAL(VK_SUBOPTIMAL_KHR);
    VAL(VK_ERROR_OUT_OF_DATE_KHR);
    VAL(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR);
    VAL(VK_ERROR_VALIDATION_FAILED_EXT);
    VAL(VK_ERROR_INVALID_SHADER_NV);
    VAL(VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
    VAL(VK_ERROR_NOT_PERMITTED_EXT);
    //VAL(VK_ERROR_OUT_OF_POOL_MEMORY_KHR);
    //VAL(VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR);
    VAL(VK_RESULT_MAX_ENUM);
#undef VAL
    default: return "Unknown VkResult";
    }
}
