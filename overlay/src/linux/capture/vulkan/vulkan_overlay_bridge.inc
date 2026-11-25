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

/* Bridge to the AT Vulkan ImGui renderer */
#include "overlay_bridge.h"

bool at_bridge_collect(const struct vk_data *data, VkSwapchainKHR sc, struct at_bridge_info *out)
{
    if (!data || !out || !data->graphics_queue) return false;
    struct vk_swap_data *swap = get_swap_data((struct vk_data *)data, sc);
    if (!swap || !swap->swap_images || swap->image_count == 0) return false;

    out->device = data->device;
    out->phy = data->phy_device;
    out->instance = data->inst_data ? data->inst_data->instance : VK_NULL_HANDLE;
    out->graphics_queue = data->graphics_queue;
    struct vk_queue_data *qd = get_queue_data((struct vk_data *)data, data->graphics_queue);
    out->graphics_family = qd ? qd->fam_idx : 0;
    out->swapchain = sc;
    out->extent = swap->image_extent;
    out->format = swap->format;
    out->image_count = swap->image_count;
    out->images = swap->swap_images;
    out->gdpa = data->funcs.GetDeviceProcAddr;
    out->gipa = data->inst_data ? data->inst_data->funcs.GetInstanceProcAddr : NULL;
    return true;
}

bool at_bridge_set_dispatch_key(VkDevice device, void *dispatchable)
{
    if (!device || !dispatchable)
        return false;
        
    GET_LDT(dispatchable) = GET_LDT(device);
    return true;
}
