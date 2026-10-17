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

#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vk_data;

struct at_bridge_info {
    VkDevice         device;
    VkPhysicalDevice phy;
    VkInstance       instance;
    VkQueue          graphics_queue;
    uint32_t         graphics_family;
    VkSwapchainKHR   swapchain;
    VkExtent2D       extent;
    VkFormat         format;
    uint32_t         image_count;
    const VkImage*   images;
    PFN_vkGetDeviceProcAddr   gdpa;
    PFN_vkGetInstanceProcAddr gipa;
};

bool at_bridge_collect(const struct vk_data *data, VkSwapchainKHR sc, struct at_bridge_info *out);

#ifdef __cplusplus
}
#endif
