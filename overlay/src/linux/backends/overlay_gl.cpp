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

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include <mutex>

extern "C" {
#include "utils.h"
#include "overlay_gl_bridge.h"
}

#include "overlay_text.h"
#include "overlay_ui.h"

#define AT_GL_FRAMEBUFFER              0x8D40
#define AT_GL_DRAW_FRAMEBUFFER         0x8CA9
#define AT_GL_READ_FRAMEBUFFER         0x8CA8
#define AT_GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define AT_GL_READ_FRAMEBUFFER_BINDING 0x8CAA

namespace {

struct GLState {
    void*         imgui_ctx = nullptr;
    ImGuiContext* imgui     = nullptr;
    bool          imgui_ok  = false;
    int           width     = 0;
    int           height    = 0;
};

GLState        g;
std::mutex     g_mtx;

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

void build_ui(int w, int h)
{
    at_overlay_draw_ui("OpenGL", w, h);
}

void shutdown_imgui()
{
    if (g.imgui_ok) {
        ImGui::SetCurrentContext(g.imgui);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext(g.imgui);
        g.imgui = nullptr;
        g.imgui_ok = false;
        g.imgui_ctx = nullptr;
    }
}

bool ensure_imgui(void* ctx)
{
    if (g.imgui_ok && g.imgui_ctx == ctx) return true;
    if (g.imgui_ok) shutdown_imgui();

    g.imgui_ctx = ctx;
    IMGUI_CHECKVERSION();
    g.imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g.imgui);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;

    at_text_setup_font();

    if (!ImGui_ImplOpenGL3_Init(nullptr)) {
        hlog("overlay_gl: ImGui_ImplOpenGL3_Init failed");
        ImGui::DestroyContext(g.imgui);
        g.imgui = nullptr;
        return false;
    }
    g.imgui_ok = true;
    return true;
}

} // anonymous

#pragma GCC visibility push(hidden)

extern "C" void at_overlay_gl_draw(int width, int height, const struct at_gl_fb_funcs* fb)
{
    std::lock_guard<std::mutex> lk(g_mtx);

    void* ctx = (fb && fb->GetCurrentContext) ? fb->GetCurrentContext() : nullptr;
    if (!ctx || !ensure_imgui(ctx)) return;
    ImGui::SetCurrentContext(g.imgui);

    if (at_text_font_dirty()) {
        at_text_font_rebuilt();
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)width, (float)height);
    io.DeltaTime   = frame_delta();
    g.width  = width;
    g.height = height;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    build_ui(width, height);
    ImGui::Render();

    ImDrawData* draw = ImGui::GetDrawData();
    if (!draw || draw->CmdListsCount == 0) return;

    // A texture name of the overlay's that the game has taken over (see GetTextureSize in glinject.c):
    // drawing with it shows the game's texture as glyphs, and the atlas updates fail with GL errors.
    // The overlay's texture is made again under a new name, and the game's is left alone
    if (fb && fb->GetTextureSize) {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->Status != ImTextureStatus_OK && tex->Status != ImTextureStatus_WantUpdates)
                continue;
            
            const unsigned id = (unsigned)(intptr_t)tex->TexID;
            int w = 0, h = 0;
            if (id && fb->GetTextureSize(id, &w, &h) && (w != tex->Width || h != tex->Height)) {
                hlog("overlay_gl: texture %u is %dx%d in GL but %dx%d here, the game took the name; making it again", 
                    id, w, h, tex->Width, tex->Height);
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_WantCreate);
            }
        }
    }

    int last_draw_fbo = 0, last_read_fbo = 0;
    const bool retarget = (fb && fb->GetIntegerv && fb->BindFramebuffer);
    if (retarget) {
        fb->GetIntegerv(AT_GL_DRAW_FRAMEBUFFER_BINDING, &last_draw_fbo);
        fb->GetIntegerv(AT_GL_READ_FRAMEBUFFER_BINDING, &last_read_fbo);
        fb->BindFramebuffer(AT_GL_FRAMEBUFFER, 0);
    }

    ImGui_ImplOpenGL3_RenderDrawData(draw);

    if (retarget) {
        fb->BindFramebuffer(AT_GL_DRAW_FRAMEBUFFER, (unsigned)last_draw_fbo);
        fb->BindFramebuffer(AT_GL_READ_FRAMEBUFFER, (unsigned)last_read_fbo);
    }
}

extern "C" void at_overlay_gl_context_destroyed(void)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    shutdown_imgui();
}

#pragma GCC visibility pop
