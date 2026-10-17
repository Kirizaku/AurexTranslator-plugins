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

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <atomic>

#include "overlay_bridge.h"
#include "win_common.h"
#include "win_capture.h"
#include "overlay_text.h"
#include "vk_capture_win.h"

#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstring>

extern "C" void at_overlay_vk_draw(struct vk_data *data, VkQueue queue, VkPresentInfoKHR *info);
extern "C" void at_overlay_vk_device_destroyed(VkDevice device);
extern "C" void at_overlay_vk_swapchain_destroyed(VkDevice device, VkSwapchainKHR sc);

// ===============================================================
// Per-object state
// ===============================================================

namespace {

struct SwapData {
    VkExtent2D           extent{};
    VkFormat             format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images;
};

struct InstanceData {
    VkInstance                                    instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr                     gipa     = nullptr;
    PFN_vkDestroyInstance                         DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties  GetPhysicalDeviceQueueFamilyProperties = nullptr;
};

} // namespace

struct vk_data {
    VkDevice          device   = VK_NULL_HANDLE;
    VkPhysicalDevice  phy      = VK_NULL_HANDLE;
    VkInstance        instance = VK_NULL_HANDLE;
    InstanceData     *inst     = nullptr;

    PFN_vkGetDeviceProcAddr   gdpa = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;

    /* Device functions this layer itself calls */
    PFN_vkGetDeviceQueue        GetDeviceQueue        = nullptr;
    PFN_vkDestroyDevice         DestroyDevice         = nullptr;
    PFN_vkCreateSwapchainKHR    CreateSwapchainKHR    = nullptr;
    PFN_vkDestroySwapchainKHR   DestroySwapchainKHR   = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkQueuePresentKHR       QueuePresentKHR       = nullptr;

    uint32_t graphics_family = 0;
    VkQueue  graphics_queue  = VK_NULL_HANDLE;

    std::mutex                                    swaps_mtx;
    std::unordered_map<VkSwapchainKHR, SwapData>  swaps;
};

namespace {

std::mutex                                     g_lock;
std::unordered_map<VkInstance, InstanceData *> g_instances;
std::unordered_map<VkDevice,   vk_data *>      g_devices;
std::unordered_map<VkQueue,    vk_data *>      g_queues;

vk_data *device_for(VkDevice device)
{
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_devices.find(device);
    return it == g_devices.end() ? nullptr : it->second;
}

vk_data *device_for_queue(VkQueue queue)
{
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_queues.find(queue);
    return it == g_queues.end() ? nullptr : it->second;
}

// ===============================================================
// Chain-link plumbing
// ===============================================================

VkLayerInstanceCreateInfo *find_inst_link(const VkInstanceCreateInfo *ci)
{
    auto *p = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                  p->function == VK_LAYER_LINK_INFO))
        p = (VkLayerInstanceCreateInfo *)p->pNext;
    return p;
}

VkLayerDeviceCreateInfo *find_dev_link(const VkDeviceCreateInfo *ci)
{
    auto *p = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                  p->function == VK_LAYER_LINK_INFO))
        p = (VkLayerDeviceCreateInfo *)p->pNext;
    return p;
}

void merge_exts(const char *const *have, uint32_t haveCount,
                const char *const *add, uint32_t addCount,
                const std::vector<VkExtensionProperties> &avail,
                std::vector<const char *> &storage)
{
    storage.assign(have, have + haveCount);
    for (uint32_t i = 0; i < addCount; ++i) {
        bool present = false;
        for (const char *e : storage) if (!strcmp(e, add[i])) { present = true; break; }
        if (present) continue;
        bool sup = false;
        for (const auto &e : avail) if (!strcmp(e.extensionName, add[i])) { sup = true; break; }
        if (sup) storage.push_back(add[i]);
    }
}

// ===============================================================
// Instance
// ===============================================================

VKAPI_ATTR VkResult VKAPI_CALL AT_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *ac, VkInstance *pInstance)
{
    VkLayerInstanceCreateInfo *link = find_inst_link(ci);
    if (!link || !link->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr nextGIPA = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto createInstance = (PFN_vkCreateInstance)nextGIPA(VK_NULL_HANDLE, "vkCreateInstance");
    if (!createInstance)
        return VK_ERROR_INITIALIZATION_FAILED;

    std::vector<VkExtensionProperties> avail;
    std::vector<const char *> exts;
    VkInstanceCreateInfo ci2 = *ci;
    if (at::process_is_vk_overlay_target()) {
        if (auto enumInst = (PFN_vkEnumerateInstanceExtensionProperties)
                nextGIPA(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties")) {
            uint32_t n = 0;
            enumInst(nullptr, &n, nullptr);
            avail.resize(n);
            if (n) enumInst(nullptr, &n, avail.data());
        }
        uint32_t addN = 0;
        const char *const *addExts = at::vk_capture_instance_extensions(&addN);
        merge_exts(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, addExts, addN, avail, exts);
        ci2.enabledExtensionCount = (uint32_t)exts.size();
        ci2.ppEnabledExtensionNames = exts.data();
    }

    VkResult res = createInstance(&ci2, ac, pInstance);
    if (res != VK_SUCCESS) {
        res = createInstance(ci, ac, pInstance);
        if (res != VK_SUCCESS)
            return res;
    }

    auto *id = new InstanceData();
    id->instance = *pInstance;
    id->gipa = nextGIPA;
    id->DestroyInstance = (PFN_vkDestroyInstance)nextGIPA(*pInstance, "vkDestroyInstance");
    id->GetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)nextGIPA(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties");
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_instances[*pInstance] = id;
    }
    hlog("vk: CreateInstance");
    return res;
}

VKAPI_ATTR void VKAPI_CALL AT_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *ac)
{
    InstanceData *id = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_instances.find(instance);
        if (it != g_instances.end()) { id = it->second; g_instances.erase(it); }
    }
    if (id && id->DestroyInstance)
        id->DestroyInstance(instance, ac);
    delete id;
}

// ===============================================================
// Device
// ===============================================================

VKAPI_ATTR VkResult VKAPI_CALL AT_CreateDevice(VkPhysicalDevice phy, const VkDeviceCreateInfo *ci,
                                               const VkAllocationCallbacks *ac, VkDevice *pDevice)
{
    VkLayerDeviceCreateInfo *link = find_dev_link(ci);
    if (!link || !link->u.pLayerInfo)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr nextGIPA =
        link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr nextGDPA =
        link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    InstanceData *id = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_instances.empty())
            id = g_instances.begin()->second;
    }
    if (!id)
        return VK_ERROR_INITIALIZATION_FAILED;

    auto createDevice =
        (PFN_vkCreateDevice)nextGIPA(id->instance, "vkCreateDevice");
    if (!createDevice)
        return VK_ERROR_INITIALIZATION_FAILED;

    std::vector<VkExtensionProperties> avail;
    std::vector<const char *> exts;
    VkDeviceCreateInfo ci2 = *ci;
    if (at::process_is_vk_overlay_target()) {
        if (auto enumDev = (PFN_vkEnumerateDeviceExtensionProperties)nextGIPA(id->instance, "vkEnumerateDeviceExtensionProperties")) {
            uint32_t n = 0;
            enumDev(phy, nullptr, &n, nullptr);
            avail.resize(n);
            if (n) enumDev(phy, nullptr, &n, avail.data());
        }
        uint32_t addN = 0;
        const char *const *addExts = at::vk_capture_device_extensions(&addN);
        merge_exts(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, addExts, addN, avail, exts);
        ci2.enabledExtensionCount = (uint32_t)exts.size();
        ci2.ppEnabledExtensionNames = exts.data();
    }

    VkResult res = createDevice(phy, &ci2, ac, pDevice);
    if (res != VK_SUCCESS)
        return res;

    auto *d       = new vk_data();
    d->device     = *pDevice;
    d->phy        = phy;
    d->inst       = id;
    d->instance   = id->instance;
    d->gdpa       = nextGDPA;
    d->gipa       = id->gipa;

    d->GetDeviceQueue        = (PFN_vkGetDeviceQueue)       nextGDPA(*pDevice, "vkGetDeviceQueue");
    d->DestroyDevice         = (PFN_vkDestroyDevice)        nextGDPA(*pDevice, "vkDestroyDevice");
    d->CreateSwapchainKHR    = (PFN_vkCreateSwapchainKHR)   nextGDPA(*pDevice, "vkCreateSwapchainKHR");
    d->DestroySwapchainKHR   = (PFN_vkDestroySwapchainKHR)  nextGDPA(*pDevice, "vkDestroySwapchainKHR");
    d->GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)nextGDPA(*pDevice, "vkGetSwapchainImagesKHR");
    d->QueuePresentKHR       = (PFN_vkQueuePresentKHR)      nextGDPA(*pDevice, "vkQueuePresentKHR");

    bool found = false;
    if (id->GetPhysicalDeviceQueueFamilyProperties) {
        uint32_t famCount = 0;
        id->GetPhysicalDeviceQueueFamilyProperties(phy, &famCount, nullptr);
        std::vector<VkQueueFamilyProperties> props(famCount);
        if (famCount)
            id->GetPhysicalDeviceQueueFamilyProperties(phy, &famCount, props.data());

        for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
            uint32_t fam = ci->pQueueCreateInfos[i].queueFamilyIndex;
            if (fam < famCount &&
                (props[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                d->graphics_family = fam;
                found = true;
                break;
            }
        }
    }
    if (found && d->GetDeviceQueue)
        d->GetDeviceQueue(*pDevice, d->graphics_family, 0, &d->graphics_queue);

    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_devices[*pDevice] = d;
    }
    hlog("vk: CreateDevice (graphics family %u%s)", d->graphics_family, found ? "" : " NOT FOUND");
    return res;
}

VKAPI_ATTR void VKAPI_CALL AT_DestroyDevice(
        VkDevice device, const VkAllocationCallbacks *ac)
{
    at_overlay_vk_device_destroyed(device);
    at::vk_capture_device_destroyed(device);

    vk_data *d = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_devices.find(device);
        if (it != g_devices.end()) { d = it->second; g_devices.erase(it); }
        for (auto qit = g_queues.begin(); qit != g_queues.end(); ) {
            if (qit->second == d) qit = g_queues.erase(qit);
            else                  ++qit;
        }
    }
    if (d && d->DestroyDevice)
        d->DestroyDevice(device, ac);
    delete d;
}

VKAPI_ATTR void VKAPI_CALL AT_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *pQueue)
{
    vk_data *d = device_for(device);
    if (d && d->GetDeviceQueue)
        d->GetDeviceQueue(device, family, index, pQueue);
    if (d && pQueue && *pQueue) {
        std::lock_guard<std::mutex> lk(g_lock);
        g_queues[*pQueue] = d; /* app-facing queue -> device, for present */
    }
}

VKAPI_ATTR void VKAPI_CALL AT_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *pQueue)
{
    vk_data *d = device_for(device);
    if (d) {
        auto fn = (PFN_vkGetDeviceQueue2)d->gdpa(device, "vkGetDeviceQueue2");
        if (fn) fn(device, info, pQueue);
    }
    if (d && pQueue && *pQueue) {
        std::lock_guard<std::mutex> lk(g_lock);
        g_queues[*pQueue] = d;
    }
}

// ===============================================================
// Swapchain
// ===============================================================

VKAPI_ATTR VkResult VKAPI_CALL AT_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                     const VkAllocationCallbacks *ac, VkSwapchainKHR *pSc)
{
    vk_data *d = device_for(device);
    if (!d || !d->CreateSwapchainKHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = d->CreateSwapchainKHR(device, ci, ac, pSc);
    if (res != VK_SUCCESS)
        return res;

    SwapData sd;
    sd.extent = ci->imageExtent;
    sd.format = ci->imageFormat;

    uint32_t n = 0;
    d->GetSwapchainImagesKHR(device, *pSc, &n, nullptr);
    sd.images.resize(n);
    if (n)
        d->GetSwapchainImagesKHR(device, *pSc, &n, sd.images.data());

    {
        std::lock_guard<std::mutex> lk(d->swaps_mtx);
        d->swaps[*pSc] = std::move(sd);
    }
    hlog("vk: CreateSwapchainKHR %ux%u (%u images)",
         ci->imageExtent.width, ci->imageExtent.height, n);
    return res;
}

VKAPI_ATTR void VKAPI_CALL AT_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks *ac)
{
    vk_data *d = device_for(device);
    if (d) {
        at_overlay_vk_swapchain_destroyed(device, sc);
        at::vk_capture_swapchain_destroyed(device, sc);
        std::lock_guard<std::mutex> lk(d->swaps_mtx);
        d->swaps.erase(sc);
    }
    if (d && d->DestroySwapchainKHR)
        d->DestroySwapchainKHR(device, sc, ac);
}

// ===============================================================
// Present — where the overlay is drawn
// ===============================================================

VKAPI_ATTR VkResult VKAPI_CALL AT_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    vk_data *d = device_for_queue(queue);

    VkPresentInfoKHR api = *info;

    static std::atomic<bool> was_target{false};
    const bool is_target = at::process_is_vk_overlay_target();
    if (was_target.exchange(is_target) && !is_target) {
        at::capture_force_disconnect();
    }

    if (d && !is_target) {
        return d->QueuePresentKHR(queue, info);
    }

    if (d) {
        at::mark_vk_overlay_active();
        at::capture_update(); /* pump the translated-text channel */
        VkQueue drawQ = d->graphics_queue ? d->graphics_queue : queue;
        VkSemaphore capSem = VK_NULL_HANDLE;
        if (info->swapchainCount >= 1 && info->pImageIndices && info->pSwapchains) {
            VkSwapchainKHR sc0 = info->pSwapchains[0];
            uint32_t       idx = info->pImageIndices[0];
            VkImage    src = VK_NULL_HANDLE;
            VkExtent2D ext = {};
            VkFormat   fmt = VK_FORMAT_UNDEFINED;
            {
                std::lock_guard<std::mutex> lk(d->swaps_mtx);
                auto it = d->swaps.find(sc0);
                if (it != d->swaps.end() && idx < it->second.images.size()) {
                    src = it->second.images[idx];
                    ext = it->second.extent;
                    fmt = it->second.format;
                }
            }
            if (src) {
                at::VkCaptureFrame cf = {};
                cf.device = d->device; cf.phy = d->phy; cf.instance = d->instance;
                cf.gdpa = d->gdpa; cf.gipa = d->gipa;
                cf.queue = drawQ; cf.queue_family = d->graphics_family;
                cf.swapchain = sc0; cf.src_image = src; cf.extent = ext; cf.format = fmt;
                cf.wait_semaphores = api.pWaitSemaphores;
                cf.wait_semaphore_count = api.waitSemaphoreCount;
                capSem = at::vk_capture_present(cf);
            }
        }
        VkSemaphore capSemStore = capSem;
        if (capSem != VK_NULL_HANDLE) {
            api.waitSemaphoreCount = 1;
            api.pWaitSemaphores    = &capSemStore;
        }

        at_overlay_vk_draw(d, drawQ, &api);
        return d->QueuePresentKHR(queue, &api);
    }

    hlog("vk: present on unknown queue");
    return VK_SUCCESS;
}

// ===============================================================
// GetProcAddr
// ===============================================================

#define GET_PROC(name) \
    if (!strcmp(pName, "vk" #name)) return (PFN_vkVoidFunction)AT_##name

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL AT_GetDeviceProcAddr(VkDevice device, const char *pName)
{
    GET_PROC(GetDeviceProcAddr);
    GET_PROC(DestroyDevice);
    GET_PROC(GetDeviceQueue);
    GET_PROC(GetDeviceQueue2);
    GET_PROC(CreateSwapchainKHR);
    GET_PROC(DestroySwapchainKHR);
    GET_PROC(QueuePresentKHR);

    vk_data *d = device_for(device);
    if (d && d->gdpa) return d->gdpa(device, pName);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL AT_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
    GET_PROC(GetInstanceProcAddr);
    GET_PROC(CreateInstance);
    GET_PROC(DestroyInstance);
    GET_PROC(GetDeviceProcAddr);
    GET_PROC(CreateDevice);
    GET_PROC(DestroyDevice);
    GET_PROC(GetDeviceQueue);
    GET_PROC(GetDeviceQueue2);
    GET_PROC(CreateSwapchainKHR);
    GET_PROC(DestroySwapchainKHR);
    GET_PROC(QueuePresentKHR);

    InstanceData *id = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_instances.find(instance);
        if (it != g_instances.end()) id = it->second;
    }
    if (id && id->gipa) return id->gipa(instance, pName);
    return nullptr;
}

#undef GET_PROC

} // namespace

// ===============================================================
// Loader interface — exported
// ===============================================================

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *nli)
{
    static bool once = false;
    if (!once) { once = true; at::capture_init(); hlog("vk: at-overlay layer init"); }

    if (nli->loaderLayerInterfaceVersion >= 2) {
        nli->sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
        nli->pNext = nullptr;
        nli->pfnGetInstanceProcAddr       = AT_GetInstanceProcAddr;
        nli->pfnGetDeviceProcAddr         = AT_GetDeviceProcAddr;
        nli->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    if (nli->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION)
        nli->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;

    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    return AT_GetInstanceProcAddr(instance, pName);
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    return AT_GetDeviceProcAddr(device, pName);
}

// ===============================================================
// at_bridge_collect — the contract overlay_vk.cpp calls into
// ===============================================================

extern "C" bool at_bridge_collect(const struct vk_data *data, VkSwapchainKHR sc, struct at_bridge_info *out)
{
    if (!data || !out) return false;
    if (!data->graphics_queue) return false;

    auto *d = const_cast<vk_data *>(data);
    std::lock_guard<std::mutex> lk(d->swaps_mtx);
    auto it = d->swaps.find(sc);
    if (it == d->swaps.end() || it->second.images.empty())
        return false;

    const SwapData &sd = it->second;

    out->device         = d->device;
    out->phy            = d->phy;
    out->instance       = d->instance;
    out->graphics_queue = d->graphics_queue;
    out->graphics_family = d->graphics_family;

    out->swapchain   = sc;
    out->extent      = sd.extent;
    out->format      = sd.format;
    out->image_count = (uint32_t)sd.images.size();
    out->images      = sd.images.data();

    out->gdpa = d->gdpa;
    out->gipa = d->gipa;
    return true;
}
