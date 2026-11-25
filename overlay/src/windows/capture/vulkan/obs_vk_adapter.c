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

// AT integration for the unmodified OBS Vulkan capture layer

#include <windows.h>
static FARPROC WINAPI at_vk_get_proc_address(HMODULE module, LPCSTR name);
#define GetProcAddress at_vk_get_proc_address
#define AT_OBS_VULKAN 1
#include "../../../../third_party/obs-studio/graphics-hook/vulkan-capture.c"
#undef GetProcAddress
#include "overlay_bridge.h"

extern void at_overlay_vk_draw(struct vk_data *, VkQueue, VkPresentInfoKHR *);
extern void at_overlay_vk_device_destroyed(VkDevice);
extern void at_overlay_vk_swapchain_destroyed(VkDevice, VkSwapchainKHR);

static struct obs_vk_settings settings;
struct obs_vk_settings *global_hook_info = &settings;
static struct shtex_data texture_token;
static __declspec(thread) bool request_gate;
static __declspec(thread) struct vk_data *capture_device;
static SRWLOCK capture_lock = SRWLOCK_INIT;
static struct vk_data *pending_device;
static VkFence pending_fence;
static bool request_pending;
static VkPhysicalDevice cached_physical_device;
static VkQueueFamilyProperties *cached_families;
static uint32_t cached_family_count;

static HRESULT WINAPI at_vk_create_d3d11_device(
    IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
    ID3D11Device **device, D3D_FEATURE_LEVEL *selected, ID3D11DeviceContext **context)
{
    HMODULE module = GetModuleHandleW(L"d3d11.dll");
    PFN_D3D11_CREATE_DEVICE create = module ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(module, "D3D11CreateDevice") : NULL;
    if (!create) return E_FAIL;

    IDXGIAdapter1 *match = NULL;
    if (capture_device && capture_device->inst_data) {
        struct vk_inst_data *instance = capture_device->inst_data;
        PFN_vkGetPhysicalDeviceProperties2 properties2 = (PFN_vkGetPhysicalDeviceProperties2)
            instance->funcs.GetInstanceProcAddr(instance->instance, "vkGetPhysicalDeviceProperties2");
        if (properties2) {
            VkPhysicalDeviceIDProperties id = {0};
            id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
            VkPhysicalDeviceProperties2 properties = {0};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            properties.pNext = &id;
            properties2(capture_device->phy_device, &properties);
            if (id.deviceLUIDValid)
                match = (IDXGIAdapter1 *)at_bridge_find_dxgi_adapter(id.deviceLUID);
        }
    }
    if (match) hlog("vk-capture: using DXGI adapter matched to Vulkan device");
    else hlog("vk-capture: Vulkan adapter LUID unavailable; using OBS default adapter");
    HRESULT result = create(match ? (IDXGIAdapter *)match : adapter, type, software, flags,
                            levels, level_count, sdk, device, selected, context);
    if (match) IDXGIAdapter1_Release(match);
    return result;
}

static FARPROC WINAPI at_vk_get_proc_address(HMODULE module, LPCSTR name)
{
    FARPROC proc = GetProcAddress(module, name);
    if (proc && name && strcmp(name, "D3D11CreateDevice") == 0)
        return (FARPROC)at_vk_create_d3d11_device;
    return proc;
}

static void clear_family_cache(void)
{
    free(cached_families);
    cached_families = NULL;
    cached_family_count = 0;
    cached_physical_device = VK_NULL_HANDLE;
}

bool at_obs_vk_ready(void) { return at_bridge_capture_ready() && request_gate; }
bool at_obs_vk_init_shtex(struct shtex_data **out, HWND window, uint32_t width,
                          uint32_t height, uint32_t format, bool flip, uintptr_t handle)
{
    (void)window;
    const DXGI_FORMAT dxgi_format = vk_format_to_dxgi((VkFormat)format);
    if (flip || dxgi_format == DXGI_FORMAT_UNKNOWN ||
        !at_bridge_capture_publish(width, height, (uint32_t)dxgi_format, handle)) return false;
    *out = &texture_token;
    at_bridge_capture_frame_started();
    at_bridge_capture_set_method("GPU");
    return true;
}

static void poll_completion(struct vk_data *data)
{
    if (pending_device != data || !pending_fence) return;
    if (data->funcs.WaitForFences(data->device, 1, &pending_fence, VK_TRUE, 0) == VK_SUCCESS) {
        pending_device = NULL;
        pending_fence = VK_NULL_HANDLE;
        at_bridge_capture_frame_delivered();
    }
}

static VkResult VKAPI_CALL at_vk_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    struct vk_data *data = get_device_data_by_queue(queue);
    if (!data) return VK_ERROR_DEVICE_LOST;
    struct vk_queue_data *queue_data = get_queue_data(data, queue);
    VkPresentInfoKHR present = *info;
    bool target = at_bridge_process_is_target();
    if (target) {
        at_bridge_mark_vk_active();
        at_bridge_capture_update();
        if (at_bridge_capture_force_cpu_requested()) {
            static bool cpu_fallback_warning_logged;
            if (!cpu_fallback_warning_logged) {
                hlog("vk-capture: forced CPU readback is unsupported by the Windows OBS Vulkan backend; keeping GPU capture");
                cpu_fallback_warning_logged = true;
            }
        }
    }

    AcquireSRWLockExclusive(&capture_lock);
    poll_completion(data);
    if (pending_device == data) {
        bool current_swap_present = false;
        for (uint32_t i = 0; i < info->swapchainCount; ++i) {
            if (get_swap_data(data, info->pSwapchains[i]) == data->cur_swap) {
                current_swap_present = true;
                break;
            }
        }
        if (at_bridge_capture_should_stop() || !current_swap_present) {
            pending_device = NULL;
            pending_fence = VK_NULL_HANDLE;
            request_pending = !at_bridge_capture_should_stop();
        }
    }
    if (!at_bridge_capture_ready() && !at_bridge_capture_should_init()) request_pending = false;
    if (data->valid && queue_data && queue_data->supports_transfer) {
        if (!pending_fence && !request_pending && at_bridge_capture_ready())
            request_pending = at_bridge_capture_frame_requested();
        request_gate = request_pending && !pending_fence;
        const uint32_t previous_count = queue_data->frame_count;
        const uint32_t previous_index = queue_data->frame_index;
        capture_device = data;
        vk_capture(data, queue, info);
        capture_device = NULL;
        if (request_gate && queue_data->frame_count &&
            (!previous_count || queue_data->frame_count != previous_count ||
             queue_data->frame_index != previous_index || queue_data->frame_count == 1)) {
            uint32_t index = (queue_data->frame_index + queue_data->frame_count - 1) % queue_data->frame_count;
            struct vk_frame_data *frame = &queue_data->frames[index];
            if (frame->cmd_buffer_busy) {
                pending_device = data;
                pending_fence = frame->fence;
                request_pending = false;
            }
        }
        request_gate = false;
    }
    if (target) at_overlay_vk_draw(data, queue, &present);
    ReleaseSRWLockExclusive(&capture_lock);

    vk_presenting++;
    VkResult result = data->funcs.QueuePresentKHR(queue, &present);
    vk_presenting--;
    return result;
}

static void VKAPI_CALL at_vk_destroy_device(VkDevice device, const VkAllocationCallbacks *ac)
{
    AcquireSRWLockExclusive(&capture_lock);
    if (pending_device && pending_device->device == device) {
        pending_device = NULL;
        pending_fence = VK_NULL_HANDLE;
        request_pending = false;
    }
    at_overlay_vk_device_destroyed(device);
    clear_family_cache();
    OBS_DestroyDevice(device, ac);
    ReleaseSRWLockExclusive(&capture_lock);
}

static void VKAPI_CALL at_vk_destroy_swapchain(VkDevice device, VkSwapchainKHR swap,
                                                 const VkAllocationCallbacks *ac)
{
    AcquireSRWLockExclusive(&capture_lock);
    if (pending_device && pending_device->device == device &&
        pending_device->cur_swap == get_swap_data(pending_device, swap)) {
        pending_device = NULL;
        pending_fence = VK_NULL_HANDLE;
        request_pending = true;
    }
    at_overlay_vk_swapchain_destroyed(device, swap);
    OBS_DestroySwapchainKHR(device, swap, ac);
    ReleaseSRWLockExclusive(&capture_lock);
}

static PFN_vkVoidFunction VKAPI_CALL at_vk_get_device_proc(VkDevice device, const char *name);
static PFN_vkVoidFunction VKAPI_CALL at_vk_get_instance_proc(VkInstance instance, const char *name)
{
    PFN_vkVoidFunction orig = OBS_GetInstanceProcAddr(instance, name);
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)at_vk_get_instance_proc;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)at_vk_get_device_proc;
    if (!strcmp(name, "vkDestroyDevice") && orig) return (PFN_vkVoidFunction)at_vk_destroy_device;
    return orig;
}

static PFN_vkVoidFunction VKAPI_CALL at_vk_get_device_proc(VkDevice device, const char *name)
{
    PFN_vkVoidFunction orig = OBS_GetDeviceProcAddr(device, name);
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)at_vk_get_device_proc;
    if (!strcmp(name, "vkQueuePresentKHR") && orig) return (PFN_vkVoidFunction)at_vk_present;
    if (!strcmp(name, "vkDestroyDevice") && orig) return (PFN_vkVoidFunction)at_vk_destroy_device;
    if (!strcmp(name, "vkDestroySwapchainKHR") && orig) return (PFN_vkVoidFunction)at_vk_destroy_swapchain;
    return orig;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{ return at_vk_get_instance_proc(instance, name); }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{ return at_vk_get_device_proc(device, name); }
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *nli)
{
    VkResult result = OBS_Negotiate(nli);
    if (result == VK_SUCCESS) {
        at_bridge_capture_init();
        if (nli->loaderLayerInterfaceVersion >= 2) {
            nli->pfnGetInstanceProcAddr = at_vk_get_instance_proc;
            nli->pfnGetDeviceProcAddr = at_vk_get_device_proc;
        }
    }
    return result;
}

bool at_bridge_collect(const struct vk_data *data_const, VkSwapchainKHR sc, struct at_bridge_info *out)
{
    if (!data_const || !out) return false;
    struct vk_data *data = (struct vk_data *)data_const;
    struct vk_swap_data *swap = get_swap_data(data, sc);
    if (!swap || !swap->swap_images || !swap->image_count || !data->inst_data) return false;

    if (cached_physical_device != data->phy_device || !cached_families) {
        clear_family_cache();
        uint32_t count = 0;
        data->inst_data->funcs.GetPhysicalDeviceQueueFamilyProperties(data->phy_device, &count, NULL);
        if (!count) return false;
        cached_families = malloc(sizeof(*cached_families) * count);
        if (!cached_families) return false;
        data->inst_data->funcs.GetPhysicalDeviceQueueFamilyProperties(data->phy_device, &count, cached_families);
        cached_physical_device = data->phy_device;
        cached_family_count = count;
    }
    VkQueue graphics_queue = VK_NULL_HANDLE;
    uint32_t graphics_family = 0;
    struct vk_queue_data *q = queue_walk_begin(data);
    while (q) {
        if (q->fam_idx < cached_family_count && (cached_families[q->fam_idx].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            graphics_queue = (VkQueue)q->node.obj;
            graphics_family = q->fam_idx;
            break;
        }
        q = queue_walk_next(q);
    }
    queue_walk_end(data);
    if (!graphics_queue) return false;
    memset(out, 0, sizeof(*out));
    out->device = data->device;
    out->phy = data->phy_device;
    out->instance = data->inst_data->instance;
    out->graphics_queue = graphics_queue;
    out->graphics_family = graphics_family;
    out->swapchain = sc;
    out->extent = swap->image_extent;
    out->format = swap->format;
    out->image_count = swap->image_count;
    out->images = swap->swap_images;
    out->gdpa = data->funcs.GetDeviceProcAddr;
    out->gipa = data->inst_data->funcs.GetInstanceProcAddr;
    return true;
}

bool at_bridge_set_dispatch_key(VkDevice device, void *dispatchable)
{
    if (!device || !dispatchable) return false;
    GET_LDT(dispatchable) = GET_LDT(device);
    return true;
}
