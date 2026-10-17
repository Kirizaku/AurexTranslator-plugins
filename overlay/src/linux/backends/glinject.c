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
NOTE: Derived from obs-vkcapture/src/glinject.c.
Modifications:
  - Frame capture goes through gl_cpu_copy() instead of upstream's
    gl_shtex_* DMA-BUF export; the eglCreateWindowSurface hook it needed
    is removed along with it.
  - Environment variables renamed OBS_VKCAPTURE_* -> AT_OVERLAY_*
*/

#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES

#include "glinject.h"
#include "host/overlay_protocol.h"
#include "utils.h"
#include "dlsym.h"

#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>

static bool glx_seen = false;
static bool egl_seen = false;
static struct gl_funcs gl_f;
static struct egl_funcs egl_f;
static struct glx_funcs glx_f;

struct gl_data {
    bool glx;
    bool valid;
};
static struct gl_data data;

#define GETADDR(s, p, func) \
    p.func = (typeof(p.func))real_dlsym(RTLD_NEXT, #s #func); \
    if (!p.func && handle) { \
        p.func = (typeof(p.func))real_dlsym(handle, #s #func); \
    } \
    if (!p.func) { \
        hlog("Failed to resolve " #s #func); \
        return false; \
    } \

#define GETEGLADDR(func) GETADDR(egl, egl_f, func)
#define GETGLXADDR(func) GETADDR(glX, glx_f, func)

#define GETPROCADDR(s, p, func) \
    p.func = (typeof(p.func))p.GetProcAddress(#s #func); \
    if (!p.func) { \
        hlog("Failed to resolve " #s #func); \
        return false; \
    } \

#define GETGLPROCADDR(func) GETPROCADDR(gl, gl_f, func)
#define GETEGLPROCADDR(func) GETPROCADDR(egl, egl_f, func)
#define GETGLXPROCADDR(func) GETPROCADDR(glX, glx_f, func)

static bool gl_init_funcs(bool glx)
{
    if (glx && glx_seen) {
        return glx_f.valid;
    }
    if (!glx && egl_seen) {
        return egl_f.valid;
    }

#ifndef NDEBUG
    hlog("Init %s (%s)", glx ? "GLX" : "EGL",
#ifdef __x86_64__
        "64bit");
#else
        "32bit");
#endif
#endif

    capture_init();
    memset(&data, 0, sizeof(struct gl_data));
    data.glx = glx;

    if (glx) {
        glx_seen = true;
        glx_f.valid = false;
        void *handle = dlopen("libGLX.so.0", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libGLX.so.0");
            return false;
        }
        GETGLXADDR(GetProcAddress);
        GETGLXADDR(GetProcAddressARB);
        GETGLXPROCADDR(DestroyContext);
        GETGLXPROCADDR(CreateWindow);
        GETGLXPROCADDR(SwapBuffers);
        GETGLXPROCADDR(SwapBuffersMscOML);
        GETGLXPROCADDR(QueryDrawable);
        GETGLXPROCADDR(GetCurrentContext);
        gl_f.GetProcAddress = glx_f.GetProcAddress;
        glx_f.valid = true;
    } else {
        egl_seen = true;
        egl_f.valid = false;
        void *handle = dlopen("libEGL.so.1", RTLD_LAZY);
        if (!handle) {
            hlog("Failed to open libEGL.so.1");
            return false;
        }
        GETEGLADDR(GetProcAddress);
        GETEGLPROCADDR(DestroyContext);
        GETEGLPROCADDR(GetCurrentContext);
        GETEGLPROCADDR(QuerySurface);
        GETEGLPROCADDR(SwapBuffers);
        gl_f.GetProcAddress = egl_f.GetProcAddress;
        egl_f.valid = true;
    }

    GETGLPROCADDR(GetIntegerv);
    GETGLPROCADDR(BindFramebuffer);
    GETGLPROCADDR(ReadBuffer);
    GETGLPROCADDR(GetString);
    GETGLPROCADDR(ReadPixels);
    GETGLPROCADDR(PixelStorei);

    data.valid = true;

    return true;
}

#undef GETADDR
#undef GETEGLADDR
#undef GETGLXADDR
#undef GETPROCADDR
#undef GETGLPROCADDR
#undef GETEGLPROCADDR
#undef GETGLXPROCADDR

static void querySurfaceArgs(void *display, void *surface,
                              int *width, int *height)
{
    *width = 0;
    *height = 0;
    if (!display || !surface) return;
    if (data.glx) {
        unsigned w = 0, h = 0;
        glx_f.QueryDrawable(display, surface, P_GLX_WIDTH, &w);
        glx_f.QueryDrawable(display, surface, P_GLX_HEIGHT, &h);
        *width = (int)w;
        *height = (int)h;
    } else {
        egl_f.QuerySurface(display, surface, P_EGL_WIDTH, width);
        egl_f.QuerySurface(display, surface, P_EGL_HEIGHT, height);
    }
}

static void gl_free()
{
    capture_stop();
}

static bool gl_capture_disabled()
{
    static int disabled = 0;

    if (disabled != 0) {
        return disabled == 1;
    }

    disabled = 1;

    // Always use Vulkan capture with zink
    const char *renderer = (const char *)gl_f.GetString(GL_RENDERER);
    if (renderer && strncmp(renderer, "zink", 4) == 0) {
        hlog("GL capture disabled with zink");
        return true;
    }

    disabled = -1;
    return false;
}

#pragma GCC visibility push(hidden)
#include "overlay_gl_bridge.h"
#pragma GCC visibility pop

static struct at_gl_fb_funcs overlay_fb_funcs;
static void *overlay_get_current_context(void)
{
    if (data.glx) {
        return glx_f.GetCurrentContext ? glx_f.GetCurrentContext() : NULL;
    }
    return egl_f.GetCurrentContext ? egl_f.GetCurrentContext() : NULL;
}

static void at_overlay_draw_for(void *display, void *drawable)
{
    static int disabled = -1;
    if (disabled == -1) {
        disabled = getenv("AT_OVERLAY_DISABLE_DRAW") ? 1 : 0;
    }
    if (disabled) {
        return;
    }

    int w = 0, h = 0;
    querySurfaceArgs(display, drawable, &w, &h);
    if (w <= 0 || h <= 0) {
        return;
    }

    overlay_fb_funcs.GetIntegerv       = (void (*)(unsigned, int *))gl_f.GetIntegerv;
    overlay_fb_funcs.BindFramebuffer   = (void (*)(unsigned, unsigned))gl_f.BindFramebuffer;
    overlay_fb_funcs.GetCurrentContext = overlay_get_current_context;

    at_overlay_gl_draw(w, h, &overlay_fb_funcs);
}

// ===============================================================
// Frame capture
// ===============================================================

static uint8_t *cpu_readbuf = NULL;
static uint8_t *cpu_flipbuf = NULL;
static size_t   cpu_bufsize = 0;

static bool gl_cpu_copy(int w, int h)
{
    const size_t stride = (size_t)w * 4;
    const size_t bytes  = stride * (size_t)h;
    if (bytes > cpu_bufsize) {
        uint8_t *rb = realloc(cpu_readbuf, bytes);
        uint8_t *fb = realloc(cpu_flipbuf, bytes);
        if (!rb || !fb) {
            return false;
        }
        cpu_readbuf = rb;
        cpu_flipbuf = fb;
        cpu_bufsize = bytes;
    }

    gl_f.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl_f.ReadBuffer(GL_BACK);
    gl_f.ReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, cpu_readbuf);

    for (int y = 0; y < h; ++y) {
        memcpy(cpu_flipbuf + (size_t)y * stride,
               cpu_readbuf + (size_t)(h - 1 - y) * stride, stride);
    }

    return capture_write_frame(w, h, cpu_flipbuf, (int)stride);
}

static void gl_capture(void *display, void *surface)
{
    if (gl_capture_disabled()) {
        data.valid = false;
        return;
    }

    capture_update_socket();

    int width = 0, height = 0;
    querySurfaceArgs(display, surface, &width, &height);

    if (capture_should_stop()) {
        gl_free();
    }
    if (capture_should_init()) {
        capture_frame_capture_started();
    }
    if (capture_ready() && capture_frame_requested()) {
        if (width > 0 && height > 0 && gl_cpu_copy(width, height)) {
            capture_frame_delivered();
        }
    }
}

/* ======================================================================== */

void *eglGetProcAddress(const char *procName);
unsigned eglDestroyContext(void *display, void *context);
unsigned eglSwapBuffers(void *display, void *surface);

static struct {
    void *func;
    const char *name;
} egl_hooks_map[] = {
#define ADD_HOOK(fn) { (void*)fn, #fn }
    ADD_HOOK(eglGetProcAddress),
    ADD_HOOK(eglSwapBuffers),
    ADD_HOOK(eglDestroyContext)
#undef ADD_HOOK
};

void *obs_vkcapture_eglGetProcAddress(const char *name)
{
    for (int i = 0; i < sizeof(egl_hooks_map) / sizeof(egl_hooks_map[0]); ++i) {
        if (strcmp(name, egl_hooks_map[i].name) == 0) {
            return egl_hooks_map[i].func;
        }
    }
    return NULL;
}

void *eglGetProcAddress(const char *procName)
{
    if (!gl_init_funcs(/*glx*/false)) {
        return NULL;
    }
    void *func = obs_vkcapture_eglGetProcAddress(procName);
    return func ? func : egl_f.GetProcAddress(procName);
}

unsigned eglDestroyContext(void *display, void *context)
{
    if (!gl_init_funcs(/*glx*/false)) {
        return 0;
    }

    at_overlay_gl_context_destroyed();
    gl_free();

    return egl_f.DestroyContext(display, context);
}

unsigned eglSwapBuffers(void *display, void *surface)
{
    if (!gl_init_funcs(/*glx*/false)) {
        return 0;
    }

    if (data.valid) {
        gl_capture(display, surface);
    }

    at_overlay_draw_for(display, surface);

    return egl_f.SwapBuffers(display, surface);
}

/* ======================================================================== */

void *glXGetProcAddress(const char *procName);
void *glXGetProcAddressARB(const char *procName);
void glXDestroyContext(void *display, void *context);
unsigned long glXCreateWindow(void *display, void *config, unsigned long win, const int *attribList);
void glXSwapBuffers(void *display, void *surface);
int64_t glXSwapBuffersMscOML(void *display, void *drawable, int64_t target_msc, int64_t divisor, int64_t remainder);

static struct {
    void *func;
    const char *name;
} glx_hooks_map[] = {
#define ADD_HOOK(fn) { (void*)fn, #fn }
    ADD_HOOK(glXGetProcAddress),
    ADD_HOOK(glXGetProcAddressARB),
    ADD_HOOK(glXSwapBuffers),
    ADD_HOOK(glXSwapBuffersMscOML),
    ADD_HOOK(glXDestroyContext),
    ADD_HOOK(glXCreateWindow)
#undef ADD_HOOK
};

void *obs_vkcapture_glXGetProcAddress(const char *name)
{
    for (int i = 0; i < sizeof(glx_hooks_map) / sizeof(glx_hooks_map[0]); ++i) {
        if (strcmp(name, glx_hooks_map[i].name) == 0) {
            return glx_hooks_map[i].func;
        }
    }
    return NULL;
}

void *glXGetProcAddress(const char *procName)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return NULL;
    }
    void *func = obs_vkcapture_glXGetProcAddress(procName);
    return func ? func : glx_f.GetProcAddress(procName);
}

void *glXGetProcAddressARB(const char *procName)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return NULL;
    }
    void *func = obs_vkcapture_glXGetProcAddress(procName);
    return func ? func : glx_f.GetProcAddressARB(procName);
}

void glXDestroyContext(void *display, void *context)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return;
    }

    at_overlay_gl_context_destroyed();
    gl_free();

    glx_f.DestroyContext(display, context);
}

unsigned long glXCreateWindow(void *display, void *config, unsigned long win, const int *attribList)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return 0;
    }

    gl_free();

    return glx_f.CreateWindow(display, config, win, attribList);
}

void glXSwapBuffers(void *display, void *drawable)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return;
    }

    if (data.valid) {
        gl_capture(display, drawable);
    }

    at_overlay_draw_for(display, drawable);

    glx_f.SwapBuffers(display, drawable);
}

int64_t glXSwapBuffersMscOML(void *display, void *drawable, int64_t target_msc, int64_t divisor, int64_t remainder)
{
    if (!gl_init_funcs(/*glx*/true)) {
        return 0;
    }

    if (data.valid) {
        gl_capture(display, drawable);
    }

    at_overlay_draw_for(display, drawable);

    return glx_f.SwapBuffersMscOML(display, drawable, target_msc, divisor, remainder);
}

/* ======================================================================== */
/* at_overlay_resolve_hook — bridge for the shim loader (see at_overlay_resolve.h) */
/* ======================================================================== */

#include "at_overlay_resolve.h"
#include <string.h>

void *at_overlay_resolve_hook(const char *name)
{
    if (!name) return NULL;
#define M(s) if (strcmp(name, #s) == 0) return (void *)&s
    M(dlsym);
    M(dlvsym);
    M(glXGetProcAddress);
    M(glXGetProcAddressARB);
    M(glXSwapBuffers);
    M(glXSwapBuffersMscOML);
    M(glXDestroyContext);
    M(glXCreateWindow);
    M(eglGetProcAddress);
    M(eglSwapBuffers);
    M(eglDestroyContext);
#undef M
    return NULL;
}
