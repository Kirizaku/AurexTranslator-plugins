/******************************************************************************
    at-overlay - in-game overlay for AurexTranslator

    Copyright (C) 2026 by Daniil Nabiulin
    Copyright (C) by Lain Bailey and the OBS Studio contributors (obs-studio)

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
OpenGL (WGL) overlay + capture on Windows

Structure parallels obs-studio/plugins/win-capture/graphics-hook (gl-capture.c),
rewritten for this project.
*/

#include "win_common.h"
#include "win_capture.h"
#include "overlay_gl.h"
#include "overlay_text.h"
#include "overlay_ui.h"

#include <windows.h>
#include <GL/gl.h>
#include <vector>

#include <detours/detours.h>
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#define AT_GL_FRAMEBUFFER 0x8D40

namespace at {
namespace {

typedef BOOL (WINAPI* PFN_wglSwapBuffers)(HDC);
typedef BOOL (WINAPI* PFN_SwapBuffers)(HDC);
typedef BOOL (WINAPI* PFN_wglSwapLayerBuffers)(HDC, UINT);
typedef void (WINAPI* PFN_glBindFramebuffer)(GLenum, GLuint);
typedef HGLRC (WINAPI* PFN_wglGetCurrentContext)(void);
typedef PROC  (WINAPI* PFN_wglGetProcAddress)(LPCSTR);

PFN_wglSwapBuffers       real_wglSwapBuffers    = nullptr;
PFN_SwapBuffers          real_SwapBuffers       = nullptr;
PFN_wglSwapLayerBuffers  real_wglSwapLayerBuffers = nullptr;
PFN_glBindFramebuffer    p_glBindFramebuffer    = nullptr;
PFN_wglGetCurrentContext p_wglGetCurrentContext = nullptr;
PFN_wglGetProcAddress    p_wglGetProcAddress    = nullptr;
bool                     g_bind_fbo_resolved    = false;

struct GLState {
    HGLRC         ctx      = nullptr;
    HGLRC         imgui_ctx = nullptr;
    ImGuiContext* imgui    = nullptr;
    bool          imgui_ok = false;
    int           width    = 0;
    int           height   = 0;

    /* Capture: CPU readback into the shared frame mapping (capture_write_frame) */
    int                  cap_w = 0;
    int                  cap_h = 0;
    std::vector<uint8_t> readbuf;
    std::vector<uint8_t> flipbuf;
};

GLState g;
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
thread_local bool g_in_present = false;

struct Lock {
    Lock()  { if (g_lock_ready) EnterCriticalSection(&g_lock); }
    ~Lock() { if (g_lock_ready) LeaveCriticalSection(&g_lock); }
};

// ===============================================================
// Capture
// ===============================================================

void cap_release()
{
    g.cap_w = g.cap_h = 0;
    capture_stop();
}

bool cap_init(int w, int h)
{
    g.cap_w = w; g.cap_h = h;
    capture_frame_capture_started();
    return true;
}

bool cap_copy(int w, int h)
{
    const size_t stride = (size_t)w * 4;
    const size_t bytes  = stride * h;
    g.readbuf.resize(bytes);
    g.flipbuf.resize(bytes);

    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, g.readbuf.data());

    for (int y = 0; y < h; ++y)
        memcpy(g.flipbuf.data() + (size_t)y * stride, g.readbuf.data() + (size_t)(h - 1 - y) * stride, stride);

    return capture_write_frame(w, h, g.flipbuf.data(), (int)stride);
}

// ===============================================================
// Overlay
// ===============================================================

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

bool ensure_imgui(HGLRC ctx)
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
        hlog("gl: ImGui_ImplOpenGL3_Init failed");
        ImGui::DestroyContext(g.imgui);
        g.imgui = nullptr;
        return false;
    }
    g.imgui_ok = true;
    return true;
}

void build_ui()
{
    ImGuiIO& io = ImGui::GetIO();
    at_overlay_draw_ui("OpenGL", (int)io.DisplaySize.x, (int)io.DisplaySize.y);
}

void draw_overlay()
{
    static int no_draw = -1;
    if (env_flag_cached("AT_OVERLAY_DISABLE_DRAW", &no_draw)) return;
    if (!g.ctx || !ensure_imgui(g.ctx)) return;

    ImGui::SetCurrentContext(g.imgui);
    if (at_text_font_dirty()) {
        ImGui_ImplOpenGL3_DestroyDeviceObjects();
        at_text_font_rebuilt();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g.width, (float)g.height);
    io.DeltaTime   = frame_delta();

    if (!g_bind_fbo_resolved && p_wglGetProcAddress) {
        g_bind_fbo_resolved = true;
        p_glBindFramebuffer = (PFN_glBindFramebuffer)p_wglGetProcAddress("glBindFramebuffer");
    }

    GLint prev_fbo = 0;
    if (p_glBindFramebuffer) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        p_glBindFramebuffer(AT_GL_FRAMEBUFFER, 0);
    }
    glViewport(0, 0, g.width, g.height);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    build_ui();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (p_glBindFramebuffer)
        p_glBindFramebuffer(AT_GL_FRAMEBUFFER, (GLuint)prev_fbo);
}

// ===============================================================
// Per-swap
// ===============================================================

void on_present(HDC hdc)
{
    if (vk_overlay_is_active()) return;

    HGLRC ctx = p_wglGetCurrentContext ? p_wglGetCurrentContext() : nullptr;
    if (!ctx) return;
    g.ctx = ctx;

    HWND hwnd = WindowFromDC(hdc);
    RECT rc = {};
    if (hwnd) GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    g.width = w; g.height = h;

    capture_update();

    if (capture_should_stop()) cap_release();
    if (capture_ready() && (g.cap_w != w || g.cap_h != h)) cap_release();
    if (capture_should_init()) {
        if (!cap_init(w, h)) { cap_release(); capture_stop(); }
    }
    if (capture_ready() && capture_frame_requested()) {
        if (cap_copy(w, h)) capture_frame_delivered();
    }

    draw_overlay();
}

BOOL WINAPI hook_wglSwapBuffers(HDC hdc)
{
    HookEntry guard;
    if (guard && !g_in_present) {
        g_in_present = true;
        { Lock lk; on_present(hdc); }
        g_in_present = false;
    }
    return real_wglSwapBuffers(hdc);
}

BOOL WINAPI hook_SwapBuffers(HDC hdc)
{
    HookEntry guard;
    if (guard && !g_in_present) {
        g_in_present = true;
        { Lock lk; on_present(hdc); }
        g_in_present = false;
    }
    return real_SwapBuffers(hdc);
}

BOOL WINAPI hook_wglSwapLayerBuffers(HDC hdc, UINT planes)
{
    HookEntry guard;
    if (guard && !g_in_present) {
        g_in_present = true;
        { Lock lk; on_present(hdc); }
        g_in_present = false;
    }
    return real_wglSwapLayerBuffers(hdc, planes);
}

} // namespace

// ===============================================================
// Public
// ===============================================================

bool hook_gl_install()
{
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) gl = LoadLibraryA("opengl32.dll");
    if (!gl) return false;

    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = true; }

    p_wglGetCurrentContext = (PFN_wglGetCurrentContext)GetProcAddress(gl, "wglGetCurrentContext");
    p_wglGetProcAddress = (PFN_wglGetProcAddress)GetProcAddress(gl, "wglGetProcAddress");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    bool any = false;

    void* pWgl = (void*)GetProcAddress(gl, "wglSwapBuffers");
    if (pWgl) {
        real_wglSwapBuffers = (PFN_wglSwapBuffers)pWgl;
        if (DetourAttach(&(PVOID&)real_wglSwapBuffers, (PVOID)hook_wglSwapBuffers) == NO_ERROR) {
            any = true;
        } else {
            real_wglSwapBuffers = nullptr;
        }
    }

    void* pWglLayer = (void*)GetProcAddress(gl, "wglSwapLayerBuffers");
    if (pWglLayer) {
        real_wglSwapLayerBuffers = (PFN_wglSwapLayerBuffers)pWglLayer;
        if (DetourAttach(&(PVOID&)real_wglSwapLayerBuffers, (PVOID)hook_wglSwapLayerBuffers) == NO_ERROR) {
            any = true;
        } else {
            real_wglSwapLayerBuffers = nullptr;
        }
    }

    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (gdi) {
        void* pSwap = (void*)GetProcAddress(gdi, "SwapBuffers");
        if (pSwap) {
            real_SwapBuffers = (PFN_SwapBuffers)pSwap;
            if (DetourAttach(&(PVOID&)real_SwapBuffers, (PVOID)hook_SwapBuffers) == NO_ERROR) {
                any = true;
            } else {
                real_SwapBuffers = nullptr;
            }
        }
    }

    if (DetourTransactionCommit() != NO_ERROR) {
        hlog("gl: DetourTransactionCommit failed");
        real_wglSwapBuffers = nullptr;
        real_wglSwapLayerBuffers = nullptr;
        real_SwapBuffers = nullptr;
        any = false;
    }

    if (any) hlog("gl: swap hooks created (wgl=%p layer=%p)", pWgl, pWglLayer);
    return any;
}

void hook_gl_uninstall()
{
    if (real_wglSwapBuffers || real_wglSwapLayerBuffers || real_SwapBuffers) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        if (real_wglSwapBuffers)
            DetourDetach(&(PVOID&)real_wglSwapBuffers, (PVOID)hook_wglSwapBuffers);

        if (real_wglSwapLayerBuffers)
            DetourDetach(&(PVOID&)real_wglSwapLayerBuffers, (PVOID)hook_wglSwapLayerBuffers);

        if (real_SwapBuffers)
            DetourDetach(&(PVOID&)real_SwapBuffers, (PVOID)hook_SwapBuffers);

        DetourTransactionCommit();

        real_wglSwapBuffers = nullptr;
        real_wglSwapLayerBuffers = nullptr;
        real_SwapBuffers = nullptr;
    }

    if (!g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    shutdown_imgui();
    cap_release();
    LeaveCriticalSection(&g_lock);
    DeleteCriticalSection(&g_lock);
    g_lock_ready = false;
}

} // namespace at
