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

/* AT symbol/overlay adapter around the pristine obs-vkcapture GL injector */
#define gl_capture at_obs_gl_capture
#define eglGetProcAddress at_obs_eglGetProcAddress
#define eglDestroyContext at_obs_eglDestroyContext
#define eglSwapBuffers at_obs_eglSwapBuffers
#define eglCreateWindowSurface at_obs_eglCreateWindowSurface
#define glXGetProcAddress at_obs_glXGetProcAddress
#define glXGetProcAddressARB at_obs_glXGetProcAddressARB
#define glXDestroyContext at_obs_glXDestroyContext
#define glXCreateWindow at_obs_glXCreateWindow
#define glXSwapBuffers at_obs_glXSwapBuffers
#define glXSwapBuffersMscOML at_obs_glXSwapBuffersMscOML
#define obs_vkcapture_eglGetProcAddress at_obs_egl_proc_map
#define obs_vkcapture_glXGetProcAddress at_obs_glx_proc_map
#include "../../../../third_party/obs-vkcapture/glinject.c"
#undef gl_capture
#undef eglGetProcAddress
#undef eglDestroyContext
#undef eglSwapBuffers
#undef eglCreateWindowSurface
#undef glXGetProcAddress
#undef glXGetProcAddressARB
#undef glXDestroyContext
#undef glXCreateWindow
#undef glXSwapBuffers
#undef glXSwapBuffersMscOML
#undef obs_vkcapture_eglGetProcAddress
#undef obs_vkcapture_glXGetProcAddress

#include "../../backends/overlay_gl_bridge.h"

void *glXGetProcAddressARB(const char *name);
static void at_obs_gl_capture(void *display, void *surface, bool glx);
extern void at_overlay_gl_context_destroyed(void);
extern void capture_gpu_frame_delivered(void);
extern bool capture_frame_requested(void);
extern bool capture_write_frame(int, int, const void *, int);
extern void capture_frame_delivered(void);
extern void capture_frame_capture_started(void);
extern void capture_set_method(const char *method);
extern bool capture_gpu_fallback_requested(void);

static bool at_gl_cpu_active = false;
static bool at_gl_gpu_request = false;
static bool at_glx_dri3_unavailable = false;
#include "opengl_cpu_fallback.inc"

static unsigned at_egl_swap_passthrough(void *display, void *surface)
{
    typedef unsigned (*swap_fn)(void *, void *);
    static swap_fn real_swap;
    static bool resolved;
    if (!resolved) {
        real_swap = (swap_fn)dlsym(RTLD_NEXT, "eglSwapBuffers");
        resolved = true;
    }
    return real_swap ? real_swap(display, surface) : 0;
}

static void at_glx_swap_passthrough(void *display, void *drawable)
{
    typedef void (*swap_fn)(void *, void *);
    static swap_fn real_swap;
    static bool resolved;
    if (!resolved) {
        real_swap = (swap_fn)dlsym(RTLD_NEXT, "glXSwapBuffers");
        resolved = true;
    }
    if (real_swap) real_swap(display, drawable);
}

static int64_t at_glx_swap_msc_passthrough(void *display, void *drawable,
        int64_t target_msc, int64_t divisor, int64_t remainder)
{
    typedef int64_t (*swap_fn)(void *, void *, int64_t, int64_t, int64_t);
    static swap_fn real_swap;
    static bool resolved;
    if (!resolved) {
        real_swap = (swap_fn)dlsym(RTLD_NEXT, "glXSwapBuffersMscOML");
        resolved = true;
    }
    return real_swap ? real_swap(display, drawable, target_msc, divisor, remainder) : -1;
}

static bool at_gl_uses_zink(void)
{
    static int uses_zink = -1;
    if (uses_zink < 0) {
        const GLubyte *renderer = gl_f.GetString ? gl_f.GetString(GL_RENDERER) : NULL;
        uses_zink = renderer && strncmp((const char *)renderer, "zink", 4) == 0;
        if (uses_zink)
            hlog("OpenGL is running on Zink; deferring capture and HUD to Vulkan");
    }
    return uses_zink != 0;
}

static void at_prepare_glx_capture(void)
{
    /* obs-vkcapture's GLX exporter requires the X11 DRI3 extension */
    static bool prepared;
    if (prepared) return;

    /* This adapter owns the OpenGL route; do not let a global vkcapture
     * setting redirect OpenGL through Vulkan */
    unsetenv("OBS_VKCAPTURE_GLVULKAN");

    typedef void *(*x_open_display_fn)(const char *);
    typedef int (*x_query_extension_fn)(void *, const char *, int *, int *, int *);
    typedef int (*x_close_display_fn)(void *);
    static void *x11 = NULL;
    static x_open_display_fn x_open_display = NULL;
    static x_query_extension_fn x_query_extension = NULL;
    static x_close_display_fn x_close_display = NULL;
    static int dri3_available = -1;

    if (dri3_available < 0) {
        x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (x11) {
            x_open_display = (x_open_display_fn)real_dlsym(x11, "XOpenDisplay");
            x_query_extension = (x_query_extension_fn)real_dlsym(x11, "XQueryExtension");
            x_close_display = (x_close_display_fn)real_dlsym(x11, "XCloseDisplay");
        }
        if (x_open_display && x_query_extension && x_close_display) {
            void *display = x_open_display(NULL);
            if (display) {
                int opcode = 0, event = 0, error = 0;
                dri3_available = x_query_extension(display, "DRI3", &opcode, &event, &error);
                x_close_display(display);
            }
        }
        if (dri3_available < 0) dri3_available = 0;
    }

    if (dri3_available) {
        hlog("GLX capture: DRI3 available; using upstream GLX GPU export");
    } else {
        at_glx_dri3_unavailable = true;
        hlog("GLX capture: DRI3 unavailable; using AT CPU fallback");
    }
    prepared = true;
}

static void at_capture_gl(void *display, void *surface, bool glx)
{
    capture_update_socket();
    if (at_gl_cpu_active && capture_should_stop()) {
        gl_cpu_async_free();
        capture_stop();
        at_gl_cpu_active = false;
        return;
    }

    if (!at_gl_cpu_active && capture_should_stop()) {
        at_gl_gpu_request = false;
        at_obs_gl_capture(display, surface, glx);
        return;
    }

    if (!at_gl_cpu_active && (capture_should_init() || capture_ready()) &&
        ((glx && at_glx_dri3_unavailable) ||
         capture_gpu_fallback_requested() || !data.valid)) {
        gl_free();
        at_gl_cpu_active = true;
        hlog("OpenGL GPU capture failed; switching to CPU readback");
    }

    if (at_gl_cpu_active) {
        if (capture_should_init()) {
            capture_frame_capture_started();
            capture_set_method("CPU");
        }
        if (capture_ready()) {
            data.display = display;
            data.surface = surface;
            int width = 0, height = 0;
            querySurface(&width, &height);
            if (width > 0 && height > 0)
                gl_cpu_capture_frame(width, height, glx);
        }
        return;
    }

    if (!at_gl_gpu_request && capture_ready())
        at_gl_gpu_request = capture_frame_requested();
    const bool requested = at_gl_gpu_request;
    if (!data.valid || !(capture_should_init() || requested))
        return;

    at_obs_gl_capture(display, surface, glx);
    if (capture_ready() && requested) {
        /* Finish the export copy before the separate AT process imports it
         * The next copy waits for AT's next frame request */
        void (*finish)(void) = glx ? (void (*)(void))glx_f.GetProcAddress("glFinish")
                                  : (void (*)(void))egl_f.GetProcAddress("glFinish");
        if (finish) {
            finish();
            capture_gpu_frame_delivered();
            at_gl_gpu_request = false;
        }
    }
}

static void *at_glx_current_context(void)
{
    void *(*get_context)(void) = (void *(*)(void))glx_f.GetProcAddress(
            "glXGetCurrentContext");
    return get_context ? get_context() : NULL;
}

static void at_draw(void *display, void *surface, bool glx)
{
    if (getenv("AT_OVERLAY_DISABLE_DRAW")) return;
    data.display = display;
    data.surface = surface;
    int width = 0, height = 0;
    querySurface(&width, &height);
    if (width <= 0 || height <= 0) return;

    struct at_gl_fb_funcs fb = {0};
    fb.GetIntegerv = (void (*)(unsigned, int *))gl_f.GetIntegerv;
    fb.BindFramebuffer = (void (*)(unsigned, unsigned))gl_f.BindFramebuffer;
    fb.GetCurrentContext = glx ? NULL : egl_f.GetCurrentContext;
    if (glx) {
        /* The upstream injector does not need this function; the AT renderer does */
        fb.GetCurrentContext = at_glx_current_context;
    }
    at_overlay_gl_draw(width, height, &fb);
}

unsigned eglSwapBuffers(void *display, void *surface)
{
    if (!gl_init_funcs(false)) {
        static bool logged;
        if (!logged) { hlog("EGL function initialization failed; passing swap through"); logged = true; }
        return at_egl_swap_passthrough(display, surface);
    }
    if (!at_gl_uses_zink()) {
        at_capture_gl(display, surface, false);
        at_draw(display, surface, false);
    }
    return egl_f.SwapBuffers(display, surface);
}

void glXSwapBuffers(void *display, void *drawable)
{
    at_prepare_glx_capture();
    if (!gl_init_funcs(true)) {
        static bool logged;
        if (!logged) { hlog("GLX function initialization failed; passing swap through"); logged = true; }
        at_glx_swap_passthrough(display, drawable);
        return;
    }
    if (!at_gl_uses_zink()) {
        at_capture_gl(display, drawable, true);
        at_draw(display, drawable, true);
    }
    glx_f.SwapBuffers(display, drawable);
}

int64_t glXSwapBuffersMscOML(void *display, void *drawable,
                            int64_t target_msc, int64_t divisor,
                            int64_t remainder)
{
    at_prepare_glx_capture();
    if (!gl_init_funcs(true)) {
        static bool logged;
        if (!logged) { hlog("GLX MSC function initialization failed; passing swap through"); logged = true; }
        return at_glx_swap_msc_passthrough(display, drawable, target_msc, divisor, remainder);
    }
    if (!at_gl_uses_zink()) {
        at_capture_gl(display, drawable, true);
        at_draw(display, drawable, true);
    }
    return glx_f.SwapBuffersMscOML(display, drawable, target_msc, divisor,
                                   remainder);
}

unsigned eglDestroyContext(void *display, void *context)
{
    gl_cpu_context_destroyed(context);
    at_overlay_gl_context_destroyed();
    return at_obs_eglDestroyContext(display, context);
}

void glXDestroyContext(void *display, void *context)
{
    gl_cpu_context_destroyed(context);
    at_overlay_gl_context_destroyed();
    at_obs_glXDestroyContext(display, context);
}

unsigned long glXCreateWindow(void *display, void *config, unsigned long win,
                              const int *attribList)
{
    if (!gl_init_funcs(true)) return 0;
    gl_free();

    /* obs-vkcapture's internal GLX function table declares this as bool,
     * but GLX returns an XID-sized GLXWindow. Call the real function with
     * the correct ABI so the handle is not truncated */
    typedef unsigned long (*create_window_fn)(void *, void *, unsigned long,
                                               const int *);
    create_window_fn create_window = (create_window_fn)glx_f.CreateWindow;
    return create_window ? create_window(display, config, win, attribList) : 0;
}

void *eglGetProcAddress(const char *name)
{
    if (!strcmp(name, "eglSwapBuffers")) return (void *)eglSwapBuffers;
    if (!strcmp(name, "eglGetProcAddress")) return (void *)eglGetProcAddress;
    if (!strcmp(name, "eglDestroyContext")) return (void *)eglDestroyContext;
    return at_obs_eglGetProcAddress(name);
}

void *glXGetProcAddress(const char *name)
{
    at_prepare_glx_capture();
    if (!strcmp(name, "glXSwapBuffers")) return (void *)glXSwapBuffers;
    if (!strcmp(name, "glXSwapBuffersMscOML")) return (void *)glXSwapBuffersMscOML;
    if (!strcmp(name, "glXDestroyContext")) return (void *)glXDestroyContext;
    if (!strcmp(name, "glXCreateWindow")) return (void *)glXCreateWindow;
    if (!strcmp(name, "glXGetProcAddress")) return (void *)glXGetProcAddress;
    if (!strcmp(name, "glXGetProcAddressARB")) return (void *)glXGetProcAddressARB;
    return at_obs_glXGetProcAddress(name);
}

void *glXGetProcAddressARB(const char *name)
{
    at_prepare_glx_capture();
    return glXGetProcAddress(name);
}

void *obs_vkcapture_eglGetProcAddress(const char *name)
{
    if (!strcmp(name, "eglGetProcAddress")) return (void *)eglGetProcAddress;
    if (!strcmp(name, "eglSwapBuffers")) return (void *)eglSwapBuffers;
    if (!strcmp(name, "eglDestroyContext")) return (void *)eglDestroyContext;
    return at_obs_egl_proc_map(name);
}

void *obs_vkcapture_glXGetProcAddress(const char *name)
{
    if (!strcmp(name, "glXGetProcAddress")) return (void *)glXGetProcAddress;
    if (!strcmp(name, "glXGetProcAddressARB")) return (void *)glXGetProcAddressARB;
    if (!strcmp(name, "glXSwapBuffers")) return (void *)glXSwapBuffers;
    if (!strcmp(name, "glXSwapBuffersMscOML")) return (void *)glXSwapBuffersMscOML;
    if (!strcmp(name, "glXDestroyContext")) return (void *)glXDestroyContext;
    if (!strcmp(name, "glXCreateWindow")) return (void *)glXCreateWindow;
    return at_obs_glx_proc_map(name);
}

void *at_overlay_resolve_hook(const char *name)
{
    if (!strcmp(name, "eglGetProcAddress")) return (void *)eglGetProcAddress;
    if (!strcmp(name, "eglSwapBuffers")) return (void *)eglSwapBuffers;
    if (!strcmp(name, "eglDestroyContext")) return (void *)eglDestroyContext;
    if (!strcmp(name, "glXGetProcAddress")) return (void *)glXGetProcAddress;
    if (!strcmp(name, "glXGetProcAddressARB")) return (void *)glXGetProcAddressARB;
    if (!strcmp(name, "glXSwapBuffers")) return (void *)glXSwapBuffers;
    if (!strcmp(name, "glXSwapBuffersMscOML")) return (void *)glXSwapBuffersMscOML;
    if (!strcmp(name, "glXDestroyContext")) return (void *)glXDestroyContext;
    if (!strcmp(name, "glXCreateWindow")) return (void *)glXCreateWindow;
    return NULL;
}
