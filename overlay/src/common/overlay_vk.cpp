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

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include "overlay_bridge.h"
#include "overlay_text.h"
#include "overlay_ui.h"

#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include "win_common.h"
static inline int64_t os_time_get_nano() { return ::at::time_get_nano(); }
#else
extern "C" {
#include "utils.h"
}
#endif

namespace {

struct OverlayCtx {
    VkDevice         device   = VK_NULL_HANDLE;
    VkPhysicalDevice phy      = VK_NULL_HANDLE;
    VkInstance       instance = VK_NULL_HANDLE;

    PFN_vkGetDeviceProcAddr   gdpa = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;

#define DEV(fn) PFN_vk##fn fn = nullptr;
    DEV(CreateRenderPass)        DEV(DestroyRenderPass)
    DEV(CreateImageView)         DEV(DestroyImageView)
    DEV(CreateFramebuffer)       DEV(DestroyFramebuffer)
    DEV(CreateDescriptorPool)    DEV(DestroyDescriptorPool)
    DEV(CreateCommandPool)       DEV(DestroyCommandPool)
    DEV(AllocateCommandBuffers)  DEV(FreeCommandBuffers)
    DEV(BeginCommandBuffer)      DEV(EndCommandBuffer)
    DEV(CmdBeginRenderPass)      DEV(CmdEndRenderPass)
    DEV(QueueSubmit)             DEV(QueueWaitIdle)
    DEV(DeviceWaitIdle)
    DEV(CreateFence)             DEV(DestroyFence)
    DEV(ResetFences)             DEV(WaitForFences)
    DEV(CreateSemaphore)         DEV(DestroySemaphore)
#undef DEV

    VkSwapchainKHR currentSwap = VK_NULL_HANDLE;
    VkFormat       swapFormat  = VK_FORMAT_UNDEFINED;
    bool           swapIsSrgb  = false;
    VkExtent2D     swapExtent  = {0, 0};
    std::vector<VkImageView>   views;
    std::vector<VkFramebuffer> fbs;

    VkRenderPass     renderPass = VK_NULL_HANDLE;
    VkDescriptorPool descPool   = VK_NULL_HANDLE;
    VkCommandPool    cmdPool    = VK_NULL_HANDLE;

    static constexpr uint32_t kFramesInFlight = 3;
    struct Frame {
        VkCommandBuffer cmd   = VK_NULL_HANDLE;
        VkFence         fence = VK_NULL_HANDLE;
        VkSemaphore     done  = VK_NULL_HANDLE;
        bool            submitted = false;
    };
    Frame    frames[kFramesInFlight];
    uint32_t frameIdx = 0;

    ImGuiContext* imgui = nullptr;
    bool          imguiInit = false;

    uint32_t graphicsFamily = 0;
    VkQueue  graphicsQueue  = VK_NULL_HANDLE;
};

std::mutex g_mtx;
std::unordered_map<VkDevice, OverlayCtx*> g_ctx;

#define LOAD_DEV(c, fn) c->fn = (PFN_vk##fn)c->gdpa(c->device, "vk" #fn)

void load_dispatch(OverlayCtx* c)
{
    LOAD_DEV(c, CreateRenderPass);        LOAD_DEV(c, DestroyRenderPass);
    LOAD_DEV(c, CreateImageView);         LOAD_DEV(c, DestroyImageView);
    LOAD_DEV(c, CreateFramebuffer);       LOAD_DEV(c, DestroyFramebuffer);
    LOAD_DEV(c, CreateDescriptorPool);    LOAD_DEV(c, DestroyDescriptorPool);
    LOAD_DEV(c, CreateCommandPool);       LOAD_DEV(c, DestroyCommandPool);
    LOAD_DEV(c, AllocateCommandBuffers);  LOAD_DEV(c, FreeCommandBuffers);
    LOAD_DEV(c, BeginCommandBuffer);      LOAD_DEV(c, EndCommandBuffer);
    LOAD_DEV(c, CmdBeginRenderPass);      LOAD_DEV(c, CmdEndRenderPass);
    LOAD_DEV(c, QueueSubmit);             LOAD_DEV(c, QueueWaitIdle);
    LOAD_DEV(c, DeviceWaitIdle);
    LOAD_DEV(c, CreateFence);             LOAD_DEV(c, DestroyFence);
    LOAD_DEV(c, ResetFences);             LOAD_DEV(c, WaitForFences);
    LOAD_DEV(c, CreateSemaphore);         LOAD_DEV(c, DestroySemaphore);
}

bool format_is_srgb(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

float srgb_decode(float c)
{
    return (c <= 0.04045f) ? (c / 12.92f) : powf((c + 0.055f) / 1.055f, 2.4f);
}

void linearize_draw_data_colors(ImDrawData* draw)
{
    for (int n = 0; n < draw->CmdListsCount; ++n) {
        ImDrawList* list = draw->CmdLists[n];
        for (ImDrawVert& v : list->VtxBuffer) {
            const ImVec4 c = ImGui::ColorConvertU32ToFloat4(v.col);
            v.col = ImGui::ColorConvertFloat4ToU32(ImVec4(srgb_decode(c.x), srgb_decode(c.y), srgb_decode(c.z), c.w));
        }
    }
}

bool init_render_pass(OverlayCtx* c)
{
    VkAttachmentDescription att{};
    att.format         = c->swapFormat;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &att;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies   = &dep;

    return c->CreateRenderPass(c->device, &rpci, nullptr, &c->renderPass) == VK_SUCCESS;
}

bool init_descriptor_pool(OverlayCtx* c)
{
    VkDescriptorPoolSize ps[] = {{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 }};
    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = 64;
    ci.poolSizeCount = 1;
    ci.pPoolSizes    = ps;
    return c->CreateDescriptorPool(c->device, &ci, nullptr, &c->descPool) == VK_SUCCESS;
}

bool init_command_pool(OverlayCtx* c)
{
    VkCommandPoolCreateInfo cpi{};
    cpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = c->graphicsFamily;
    if (c->CreateCommandPool(c->device, &cpi, nullptr, &c->cmdPool) != VK_SUCCESS)
        return false;

    for (auto& f : c->frames) {
        VkCommandBufferAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = c->cmdPool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (c->AllocateCommandBuffers(c->device, &ai, &f.cmd) != VK_SUCCESS) return false;

        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (c->CreateFence(c->device, &fi, nullptr, &f.fence) != VK_SUCCESS) return false;

        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (c->CreateSemaphore(c->device, &si, nullptr, &f.done) != VK_SUCCESS) return false;
    }
    return true;
}

void destroy_framebuffers(OverlayCtx* c)
{
    for (auto fb : c->fbs) if (fb) c->DestroyFramebuffer(c->device, fb, nullptr);
    for (auto v  : c->views) if (v) c->DestroyImageView(c->device, v, nullptr);
    c->fbs.clear();
    c->views.clear();
}

bool init_framebuffers(OverlayCtx* c, const at_bridge_info* bi)
{
    destroy_framebuffers(c);
    c->views.resize(bi->image_count);
    c->fbs.resize(bi->image_count);

    for (uint32_t i = 0; i < bi->image_count; ++i) {
        VkImageViewCreateInfo iv{};
        iv.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image    = bi->images[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format   = bi->format;
        iv.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        iv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (c->CreateImageView(c->device, &iv, nullptr, &c->views[i]) != VK_SUCCESS) return false;

        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = c->renderPass;
        fb.attachmentCount = 1;
        fb.pAttachments    = &c->views[i];
        fb.width           = bi->extent.width;
        fb.height          = bi->extent.height;
        fb.layers          = 1;
        if (c->CreateFramebuffer(c->device, &fb, nullptr, &c->fbs[i]) != VK_SUCCESS) return false;
    }
    c->swapExtent  = bi->extent;
    c->swapFormat  = bi->format;
    c->swapIsSrgb  = format_is_srgb(bi->format);
    c->currentSwap = bi->swapchain;
    return true;
}

PFN_vkVoidFunction imgui_loader_cb(const char* name, void* ud)
{
    auto* c = (OverlayCtx*)ud;
    if (c->gdpa) {
        if (auto p = c->gdpa(c->device, name)) return p;
    }
    if (c->gipa) return c->gipa(c->instance, name);
    return nullptr;
}

bool init_imgui(OverlayCtx* c, uint32_t imageCount)
{
    if (c->imguiInit) return true;
    IMGUI_CHECKVERSION();
    c->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(c->imgui);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)c->swapExtent.width, (float)c->swapExtent.height);

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;

    at_text_setup_font();

    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, imgui_loader_cb, c);

    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion      = VK_API_VERSION_1_1;
    ii.Instance        = c->instance;
    ii.PhysicalDevice  = c->phy;
    ii.Device          = c->device;
    ii.QueueFamily     = c->graphicsFamily;
    ii.Queue           = c->graphicsQueue;
    ii.DescriptorPool  = c->descPool;
    ii.MinImageCount   = imageCount;
    ii.ImageCount      = imageCount;
    ii.PipelineInfoMain.RenderPass  = c->renderPass;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = false;

    if (!ImGui_ImplVulkan_Init(&ii)) {
        hlog("ImGui_ImplVulkan_Init failed");
        ImGui::DestroyContext(c->imgui);
        c->imgui = nullptr;
        return false;
    }
    c->imguiInit = true;
    return true;
}

float frame_delta()
{
    static int64_t last = 0;
    const int64_t now = os_time_get_nano();
    if (last == 0) { last = now; return 1.0f / 60.0f; }
    const float dt = (float)(now - last) / 1e9f;
    last = now;
    if (dt <= 0.0f) return 1.0f / 60.0f;
    return dt > 0.25f ? 0.25f : dt;
}

void build_ui()
{
    ImGuiIO& io = ImGui::GetIO();
    at_overlay_draw_ui("Vulkan", (int)io.DisplaySize.x, (int)io.DisplaySize.y);
}

} // anonymous

#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif

extern "C" void at_overlay_vk_draw(struct vk_data* data, VkQueue queue, VkPresentInfoKHR* info)
{
    if (!info || info->swapchainCount == 0) return;
    if (getenv("AT_OVERLAY_DISABLE_DRAW")) return;

    at_bridge_info bi{};
    if (!at_bridge_collect(data, info->pSwapchains[0], &bi)) return;

    std::lock_guard<std::mutex> lk(g_mtx);

    auto it = g_ctx.find(bi.device);
    OverlayCtx* c = (it != g_ctx.end()) ? it->second : nullptr;
    if (!c) {
        c = new OverlayCtx();
        c->device         = bi.device;
        c->phy            = bi.phy;
        c->instance       = bi.instance;
        c->gdpa           = bi.gdpa;
        c->gipa           = bi.gipa;
        c->graphicsFamily = bi.graphics_family;
        c->graphicsQueue  = bi.graphics_queue;
        c->swapFormat     = bi.format;
        c->swapIsSrgb     = format_is_srgb(bi.format);
        c->swapExtent     = bi.extent;
        load_dispatch(c);
        if (!init_render_pass(c) || !init_descriptor_pool(c) || !init_command_pool(c)) {
            hlog("overlay_vk: ctx init failed");
            delete c;
            return;
        }
        g_ctx[bi.device] = c;
    }

    const bool formatChanged = (c->swapFormat != bi.format);
    const bool swapChanged   = (c->currentSwap != bi.swapchain) ||
                               (c->swapExtent.width  != bi.extent.width) ||
                               (c->swapExtent.height != bi.extent.height);

    if (formatChanged || swapChanged) {
        if (c->DeviceWaitIdle) c->DeviceWaitIdle(c->device);

        destroy_framebuffers(c);

        if (formatChanged) {
            if (c->imguiInit) {
                ImGui::SetCurrentContext(c->imgui);
                ImGui_ImplVulkan_Shutdown();
                ImGui::DestroyContext(c->imgui);
                c->imgui = nullptr;
                c->imguiInit = false;
            }
            if (c->renderPass) {
                c->DestroyRenderPass(c->device, c->renderPass, nullptr);
                c->renderPass = VK_NULL_HANDLE;
            }
            c->swapFormat = bi.format;
            c->swapIsSrgb = format_is_srgb(bi.format);
            if (!init_render_pass(c)) {
                hlog("overlay_vk: render pass recreate failed");
                return;
            }
        }

        c->swapExtent = bi.extent;
        if (!init_framebuffers(c, &bi)) {
            hlog("overlay_vk: framebuffer recreate failed");
            return;
        }
    }

    if (!c->imguiInit) {
        if (!init_imgui(c, bi.image_count)) return;
    }

    if (c->fbs.empty() || !c->renderPass) return;

    const uint32_t imageIndex = info->pImageIndices[0];
    if (imageIndex >= c->fbs.size()) return;

    ImGui::SetCurrentContext(c->imgui);

    if (at_text_font_dirty()) {
        at_text_font_rebuilt();
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)c->swapExtent.width, (float)c->swapExtent.height);
    io.DeltaTime   = frame_delta();

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    build_ui();
    ImGui::Render();

    ImDrawData* draw = ImGui::GetDrawData();
    if (!draw) return;
    if (c->swapIsSrgb) linearize_draw_data_colors(draw);

    auto& f = c->frames[c->frameIdx % OverlayCtx::kFramesInFlight];
    c->frameIdx++;

    if (f.submitted) {
        c->WaitForFences(c->device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    }

    VkCommandBufferBeginInfo bi2{};
    bi2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    c->BeginCommandBuffer(f.cmd, &bi2);

    VkRenderPassBeginInfo rpb{};
    rpb.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass  = c->renderPass;
    rpb.framebuffer = c->fbs[imageIndex];
    rpb.renderArea  = { {0, 0}, c->swapExtent };
    c->CmdBeginRenderPass(f.cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    if (draw->CmdListsCount > 0) {
        ImGui_ImplVulkan_RenderDrawData(draw, f.cmd);
    }
    c->CmdEndRenderPass(f.cmd);
    c->EndCommandBuffer(f.cmd);

    std::vector<VkSemaphore>          waits(info->pWaitSemaphores,
                                            info->pWaitSemaphores + info->waitSemaphoreCount);
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount,
                                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = (uint32_t)waits.size();
    si.pWaitSemaphores      = waits.empty() ? nullptr : waits.data();
    si.pWaitDstStageMask    = stages.empty() ? nullptr : stages.data();
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &f.done;

    c->ResetFences(c->device, 1, &f.fence);
    f.submitted = false;

    if (c->QueueSubmit(c->graphicsQueue ? c->graphicsQueue : queue, 1, &si, f.fence) != VK_SUCCESS) {
        hlog("overlay_vk: QueueSubmit failed");
        return;
    }
    f.submitted = true;

    info->waitSemaphoreCount = 1;
    info->pWaitSemaphores    = &f.done;

    static bool drewOnce = false;
    if (!drewOnce) { drewOnce = true; hlog("overlay_vk: first overlay frame submitted"); }
}

extern "C" void at_overlay_vk_device_destroyed(VkDevice device)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_ctx.find(device);
    if (it == g_ctx.end()) return;
    OverlayCtx* c = it->second;
    g_ctx.erase(it);

    if (c->DeviceWaitIdle) c->DeviceWaitIdle(c->device);
    if (c->imguiInit) {
        ImGui::SetCurrentContext(c->imgui);
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext(c->imgui);
    }
    destroy_framebuffers(c);
    if (c->renderPass) c->DestroyRenderPass(c->device, c->renderPass, nullptr);
    if (c->descPool)   c->DestroyDescriptorPool(c->device, c->descPool, nullptr);
    for (auto& f : c->frames) {
        if (f.fence) c->DestroyFence(c->device, f.fence, nullptr);
        if (f.done)  c->DestroySemaphore(c->device, f.done, nullptr);
    }
    if (c->cmdPool) c->DestroyCommandPool(c->device, c->cmdPool, nullptr);
    delete c;
}

extern "C" void at_overlay_vk_swapchain_destroyed(VkDevice device, VkSwapchainKHR sc)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_ctx.find(device);
    if (it == g_ctx.end()) return;
    OverlayCtx* c = it->second;

    if (c->currentSwap != sc) return;
    if (c->DeviceWaitIdle) c->DeviceWaitIdle(c->device);

    destroy_framebuffers(c);
    c->currentSwap = VK_NULL_HANDLE;
    c->swapExtent  = {0, 0};
}

#if defined(__GNUC__)
#pragma GCC visibility pop
#endif
