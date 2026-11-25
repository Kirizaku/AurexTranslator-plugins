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

#include <windows.h>

#define COBJMACROS
#include <dxgi.h>
#include <d3d11.h>
#include <cstring>
#include <vector>

#include <GL/gl.h>

#include "win_common.h"
#include "win_capture.h"
#include "../capture/d3d/obs_capture_bridge.h"
#include "../capture/gl/obs_gl_bridge.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include "overlay_gl.h"
#include "overlay_text.h"
#include "overlay_ui.h"

#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_DRAW_FRAMEBUFFER 0x8CA9
typedef void (APIENTRY *BindFramebufferProc)(GLenum, GLuint);
static BindFramebufferProc glBindFramebuffer = nullptr;

void at_imgui_gl_abandon();

namespace at {
namespace {

static HGLRC overlay_context = nullptr;
static ImGuiContext *overlay_imgui = nullptr;
static bool overlay_imgui_ok = false;
static bool overlay_functions_initialized = false;
static thread_local bool internal_capture_present = false;
static CRITICAL_SECTION gl_lock;
static bool gl_lock_ready = false;

class GlLock {
public:
    GlLock() { if (gl_lock_ready) EnterCriticalSection(&gl_lock); }
    ~GlLock() { if (gl_lock_ready) LeaveCriticalSection(&gl_lock); }
};

static bool init_overlay_functions()
{
    const auto bind = wglGetProcAddress("glBindFramebuffer");
    if (!bind || bind == reinterpret_cast<PROC>(1) || bind == reinterpret_cast<PROC>(2) ||
        bind == reinterpret_cast<PROC>(3) || bind == reinterpret_cast<PROC>(-1)) return false;

    glBindFramebuffer = reinterpret_cast<BindFramebufferProc>(bind);
    return true;
}
static void shutdown_imgui()
{
	if (!overlay_imgui_ok)
		return;

	ImGui::SetCurrentContext(overlay_imgui);
	ImGui_ImplOpenGL3_Shutdown();
	ImGui::DestroyContext(overlay_imgui);
	overlay_imgui = nullptr;
	overlay_imgui_ok = false;
	overlay_context = nullptr;
}

static bool ensure_imgui(HGLRC context)
{
	if (overlay_imgui_ok && overlay_context == context)
		return true;

	shutdown_imgui();
	overlay_context = context;
	IMGUI_CHECKVERSION();
	overlay_imgui = ImGui::CreateContext();
	if (!overlay_imgui)
		return false;

	ImGui::SetCurrentContext(overlay_imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGui::GetStyle().WindowRounding = 6.0f;
	at_text_setup_font();
	if (!ImGui_ImplOpenGL3_Init(nullptr)) {
		hlog("gl: ImGui OpenGL3 backend init failed");
		ImGui::DestroyContext(overlay_imgui);
		overlay_imgui = nullptr;
		overlay_context = nullptr;
		return false;
	}
	overlay_imgui_ok = true;
	return true;
}

static void draw_overlay(HDC hdc)
{
	static int disabled = -1;
	if (env_flag_cached("AT_OVERLAY_DISABLE_DRAW", &disabled))
		return;

	HGLRC context = wglGetCurrentContext();
	if (!context || !ensure_imgui(context))
		return;

	RECT rc = {};
	HWND hwnd = WindowFromDC(hdc);
	if (!hwnd || !GetClientRect(hwnd, &rc))
		return;

	const int width = rc.right - rc.left;
	const int height = rc.bottom - rc.top;
	if (width <= 0 || height <= 0)
		return;

	ImGui::SetCurrentContext(overlay_imgui);
	if (at_text_font_dirty()) {
		ImGui_ImplOpenGL3_DestroyDeviceObjects();
		at_text_font_rebuilt();
	}

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)width, (float)height);
	io.DeltaTime = frame_delta();
	GLint previous_fbo = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glViewport(0, 0, width, height);
	ImGui_ImplOpenGL3_NewFrame();
	ImGui::NewFrame();
	at_overlay_draw_ui("OpenGL", width, height, at_obs_gl_uses_gpu() ? "GPU" : (capture_ready() ? "CPU" : "Waiting"));
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_fbo);
}

static void on_present(HDC hdc)
{
	if (vk_overlay_is_active())
		return;

	if (!overlay_functions_initialized)
		overlay_functions_initialized = init_overlay_functions();

	if (!overlay_functions_initialized) return;

	draw_overlay(hdc);
}

} // namespace

extern "C" void at_obs_gl_capture_begin(void)
{
	capture_update();
	at_obs_gl_force_cpu(capture_force_cpu_requested());
	at_obs_before_capture();
	internal_capture_present = true;
}

extern "C" void at_obs_gl_capture_complete(void)
{
	at_obs_after_capture(false, false);
	internal_capture_present = false;
}

extern "C" void at_obs_gl_render_overlay(HDC hdc)
{
	HookEntry hook_entry;
	if (!hook_entry || vk_overlay_is_active()) return;
	GlLock lock;
	on_present(hdc);
}

extern "C" void at_obs_gl_context_destroyed(HGLRC context)
{
	HookEntry hook_entry;
	if (!hook_entry || overlay_context != context || !overlay_imgui_ok) return;
	GlLock lock;
	HDC last_hdc = wglGetCurrentDC();
	HGLRC last_context = wglGetCurrentContext();
	HDC capture_dc = at_obs_gl_capture_dc();
	const bool made_current = last_context == context || (capture_dc && wglMakeCurrent(capture_dc, context));

	if (!made_current) {
		ImGui::SetCurrentContext(overlay_imgui);
		at_imgui_gl_abandon();
		ImGui::DestroyContext(overlay_imgui);
		overlay_imgui = nullptr;
		overlay_imgui_ok = false;
		overlay_context = nullptr;
		at_obs_gl_abandon();
		return;
	}
	shutdown_imgui();
	at_obs_gl_release();
	if (last_context != context) wglMakeCurrent(last_hdc, last_context);
}

bool hook_gl_install(void)
{
	if (!gl_lock_ready) {
		InitializeCriticalSection(&gl_lock);
		gl_lock_ready = true;
	}
	return at_obs_gl_hook_install();
}

bool gl_capture_internal_present_active()
{
	return internal_capture_present;
}

void hook_gl_uninstall(void)
{
	// OBS owns these hooks and does not expose an uninstaller. The injector
	// keeps the module loaded for the process lifetime, so leave its callbacks
	// valid until process exit
}

} // namespace at
