/******************************************************************************
    at-overlay - in-game overlay for AurexTranslator

    Copyright (C) 2026 by Daniil Nabiulin
    Copyright (C) 2020 by flightlessmango (MangoHud, MIT)

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
Shim library

NOTE: Structure derived from MangoHud (MIT) — src/gl/shim.c for the
LD_PRELOAD-shim-plus-lazy-load design, the dlsym override that returns our
own trampolines, RTLD_DEEPBIND, and load_adjacent_main_lib(); src/blacklist.cpp
for the process-name detection and the built-in blacklist entries.
Modifications:
  - Hook implementations are resolved through a single exported getter
    (at_overlay_resolve_hook) with per-symbol atomic caching, instead of
    MangoHud's per-call real_dlsym() on the loaded library.
  - The hooked symbol set follows obs-vkcapture's glinject.c, not
    MangoHud's; glXCreateWindow comes from obs-vkcapture commit c7410bb.
    eglCreateWindowSurface is not hooked: it only fed the window id to
    the DMA-BUF export, which this project removed.
  - Added dlvsym override, eager constructor load (AT_OVERLAY_LAZY),
    and the /etc + ~/.config blacklist files.

Small (~30 KB) library suitable for LD_PRELOAD in every child process. It
exports thin trampolines for the GL/EGL swap entry points and lazy-loads
the heavy main library (libat-overlay-opengl.so, ~900 KB with ImGui) only
when something graphics-related actually starts happening.

Design notes:

  - This shim DOES export dlsym / dlvsym (see the bottom of this file and
    shim.version). It has to: some games dlopen libGL.so.1 and dlsym
    "glXSwapBuffers" out of it, bypassing the PLT and therefore bypassing
    our trampolines entirely.

    The obvious hazard is recursion. We are loaded with LD_PRELOAD, so we
    sit *first* in the symbol search order — every dlsym call anywhere in
    the process lands in our hook, and if the hook itself called dlsym to
    find the real dlsym, it would re-enter forever. RTLD_NEXT lookups don't
    save us either; they are fragile on glibc 2.34+ where libdl was folded
    into libc.

    The way out is elfhacks: real_dlsym() walks the ELF symbol tables of
    the loaded objects directly (libdl.so first, then libc.so), with zero
    PLT involvement, so it can find dlsym without calling dlsym. See
    resolve_real_dl_once() below.

    Note the deliberate asymmetry with the main library, which has its own
    elfhacks-based dlsym override in src/dlsym.c for the same purpose. The
    shim's copy exists so the hook is live from process start, before
    anything has pulled the main library in.

  - The main library is loaded with RTLD_DEEPBIND so its own internal
    glXGetProcAddress / eglGetProcAddress calls resolve to libGL/libEGL
    rather than recursively into the shim.

  - A blacklist (built-in plus AT_OVERLAY_BLACKLIST=name1:name2 plus
    /etc/aurextranslator/blacklist.conf plus
    $XDG_CONFIG_HOME/AurexTranslator/blacklist.conf, same app folder the
    at-overlay launcher script itself resolves plugins from) skips the
    main-library load entirely for known non-game processes.
*/

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

extern "C" {
#include "elfhacks.h"
}

extern "C" char* program_invocation_short_name;

#define EXPORT extern "C" __attribute__((visibility("default")))
#define LOG(fmt, ...)                                            \
    do {                                                          \
        if (g_debug)                                              \
            std::fprintf(stderr, "[at-overlay/shim:%d] " fmt "\n",\
                         (int)getpid(), ##__VA_ARGS__);          \
        std::fflush(stderr);                                      \
    } while (0)

static void* real_dlsym(void* handle, const char* name);
static void* real_dlvsym(void* handle, const char* name, const char* version);

// ===============================================================
// Globals
// ===============================================================

static bool g_debug = false;
static std::atomic<bool> g_main_load_attempted{false};
static void* g_main_handle = nullptr;
typedef void* (*resolve_fn_t)(const char*);
static resolve_fn_t g_resolve = nullptr;
static bool g_skip_via_blacklist = false;

static void* g_glXGetProcAddress      = nullptr;
static void* g_glXGetProcAddressARB   = nullptr;
static void* g_glXSwapBuffers         = nullptr;
static void* g_glXSwapBuffersMscOML   = nullptr;
static void* g_glXDestroyContext      = nullptr;
static void* g_glXCreateWindow        = nullptr;
static void* g_eglGetProcAddress      = nullptr;
static void* g_eglSwapBuffers         = nullptr;
static void* g_eglDestroyContext      = nullptr;

static pthread_mutex_t g_load_mutex = PTHREAD_MUTEX_INITIALIZER;

// ===============================================================
// Blacklist
// ===============================================================
// Blacklist
//
// Modelled on MangoHud's src/blacklist.cpp.
//
// Process names go in `kBuiltinBlacklist` (exact match). Additional names can
// be added via the AT_OVERLAY_BLACKLIST=name1:name2 env var, or one-per-line
// in /etc/aurextranslator/blacklist.conf or
// $XDG_CONFIG_HOME/AurexTranslator/blacklist.conf (falls back to
// ~/.config/AurexTranslator/blacklist.conf, same as everywhere else in the app
// ===============================================================

static size_t read_cmdline(char* buf, size_t cap)
{
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return (size_t)n;
}

static const char* path_basename(const char* p)
{
    if (!p) return "";
    const char* slash = std::strrchr(p, '/');
    return slash ? slash + 1 : p;
}

static const char* get_native_exe_basename()
{
    static char exe_path[512] = {0};
    static const char* cached = nullptr;
    if (cached) return cached;
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (n <= 0) { cached = ""; return cached; }
    exe_path[n] = '\0';
    cached = path_basename(exe_path);
    return cached;
}

static const char* get_wine_exe_name()
{
    static char cmdline[4096];
    static char wine_exe[256] = {0};
    static bool inited = false;
    static const char* cached = nullptr;

    if (inited) return cached;
    inited = true;

    size_t n = read_cmdline(cmdline, sizeof(cmdline));
    if (n == 0) return nullptr;

    const char* argv0 = cmdline;
    const char* argv0_base = path_basename(argv0);
    bool is_wine =
        std::strcmp(argv0_base, "wine")           == 0 ||
        std::strcmp(argv0_base, "wine64")         == 0 ||
        std::strcmp(argv0_base, "wine-preloader") == 0 ||
        std::strcmp(argv0_base, "wine64-preloader") == 0;
    if (!is_wine) return nullptr;

    size_t argv0_len = std::strlen(argv0);
    if (argv0_len + 1 >= n) return nullptr;
    const char* argv1 = argv0 + argv0_len + 1;
    const char* argv1_base = path_basename(argv1);

    const char* bs = std::strrchr(argv1_base, '\\');
    if (bs) argv1_base = bs + 1;

    if (argv1_base[0] == '\0') return nullptr;

    std::strncpy(wine_exe, argv1_base, sizeof(wine_exe) - 1);
    cached = wine_exe;
    return cached;
}

static const char* get_proc_name()
{
    if (const char* w = get_wine_exe_name()) return w;
    return get_native_exe_basename();
}

// MangoHud blacklist (src/blacklist.cpp)
static const char* const kBuiltinBlacklist[] = {
    "Amazon Games UI.exe",
    "Battle.net.exe",
    "BethesdaNetLauncher.exe",
    "CypressLauncher.exe",
    "EADesktop.exe",
    "EALauncher.exe",
    "EpicGamesLauncher.exe",
    "EpicWebHelper.exe",
    "explorer.exe",
    "ffxivlauncher.exe",
    "ffxivlauncher64.exe",
    "GalaxyClient.exe",
    "gamescope",
    "GardenGate_Launcher.exe",
    "gldriverquery",
    "halloy",
    "IGOProxy.exe",
    "IGOProxy64.exe",
    "iexplore.exe",
    "InsurgencyEAC.exe",
    "KanvasLauncher.exe",
    "Launcher",                       // Paradox Interactive launcher
    "LeagueClient.exe",
    "LeagueClientUxRender.exe",
    "MarneLauncher.exe",
    "MarvelRivals_Launcher.exe",
    "monado-service",
    "Origin.exe",
    "OriginThinSetupInternal.exe",
    "plutonium.exe",
    "plutonium-launcher-win32.exe",
    "REDlauncher.exe",
    "REDprelauncher.exe",
    "RSI Launcher.exe",
    "rundll32.exe",
    "SocialClubHelper.exe",
    "StarCitizen_Launcher.exe",
    "steam",
    "Steam.exe",
    "steamwebhelper",
    "steamwebhelper.exe",
    "tabtip.exe",
    "UplayWebCore.exe",
    "vrcompositor",
    "vulkandriverquery",
    nullptr,
};

static bool exact_match_in(const char* name, const char* const* list)
{
    if (!name || !*name) return false;
    for (const char* const* p = list; *p; ++p) {
        if (std::strcmp(name, *p) == 0) return true;
    }
    return false;
}

static bool name_in_colon_list(const char* name, const char* list)
{
    if (!list || !*list || !name || !*name) return false;
    const size_t nl = std::strlen(name);
    const char* p = list;
    while (*p) {
        const char* sep = std::strchr(p, ':');
        size_t len = sep ? (size_t)(sep - p) : std::strlen(p);
        if (len == nl && std::memcmp(p, name, nl) == 0) return true;
        if (!sep) break;
        p = sep + 1;
    }
    return false;
}

static bool name_in_file(const char* name, const char* path)
{
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        size_t n = std::strlen(p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == ' ' || p[n - 1] == '\r'))
            p[--n] = '\0';
        if (n == 0) continue;
        if (std::strcmp(p, name) == 0) { found = true; break; }
    }
    std::fclose(f);
    return found;
}

static bool is_blacklisted()
{
    const char* name = get_proc_name();
    if (!name || !*name) return false;

    if (exact_match_in(name, kBuiltinBlacklist)) {
        LOG("blacklist (builtin): %s", name);
        return true;
    }
    const char* env = std::getenv("AT_OVERLAY_BLACKLIST");
    if (env && name_in_colon_list(name, env)) {
        LOG("blacklist (env): %s", name);
        return true;
    }
    if (name_in_file(name, "/etc/aurextranslator/blacklist.conf")) {
        LOG("blacklist (/etc): %s", name);
        return true;
    }

    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if ((xdg && *xdg) || home) {
        char path[1024];
        if (xdg && *xdg)
            std::snprintf(path, sizeof(path), "%s/AurexTranslator/blacklist.conf", xdg);
        else
            std::snprintf(path, sizeof(path), "%s/.config/AurexTranslator/blacklist.conf", home);
        if (name_in_file(name, path)) {
            LOG("blacklist (~/.config): %s", name);
            return true;
        }
    }
    return false;
}

// ===============================================================
// Lazy load of the main library
// ===============================================================

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFULL
static const char* kMainLibCandidates[] = {
    "libat-overlay-opengl_x64.so",
    nullptr,
};
#else
static const char* kMainLibCandidates[] = {
    "libat-overlay-opengl_x86.so",
    nullptr,
};
#endif

#ifdef __GLIBC__
static void* load_adjacent_main_lib(int flags)
{
    Dl_info info = {};
    void* extra_info = nullptr;

    // Any symbol in this shared object will do; g_debug is convenient
    if (!dladdr1(&g_debug, &info, &extra_info, RTLD_DL_LINKMAP)) {
        LOG("dladdr1 failed: %s", dlerror());
        return nullptr;
    }

    const struct link_map* map = (const struct link_map*)extra_info;
    if (!map || !map->l_name || !map->l_name[0]) {
        LOG("dladdr1: no link_map path for self");
        return nullptr;
    }

    char self[PATH_MAX];
    if (!realpath(map->l_name, self)) {
        LOG("realpath(%s) failed: %s", map->l_name, std::strerror(errno));
        return nullptr;
    }

    char* slash = std::strrchr(self, '/');
    if (!slash) return nullptr;
    *slash = '\0';

    for (const char** c = kMainLibCandidates; *c; ++c) {
        char lib[PATH_MAX];
        if (std::snprintf(lib, sizeof(lib), "%s/%s", self, *c) >= (int)sizeof(lib))
            continue;
        void* h = dlopen(lib, flags);
        if (h) {
            LOG("dlopen(%s) ok (adjacent)", lib);
            return h;
        }
        LOG("dlopen(%s) failed: %s", lib, dlerror());
    }
    return nullptr;
}
#endif

static bool load_main_locked()
{
    if (g_main_handle) return true;
    if (g_main_load_attempted.load()) return g_main_handle != nullptr;
    g_main_load_attempted.store(true);

    if (is_blacklisted()) {
        g_skip_via_blacklist = true;
        return false;
    }

    const char* override_path = std::getenv("AT_OVERLAY_OPENGL_LIB");
    const int   flags         = RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND;

    if (override_path && *override_path) {
        g_main_handle = dlopen(override_path, flags);
        if (!g_main_handle)
            LOG("dlopen(%s) failed: %s", override_path, dlerror());
    }
#ifdef __GLIBC__
    if (!g_main_handle) {
        g_main_handle = load_adjacent_main_lib(flags);
    }
#endif
    if (!g_main_handle) {
        for (const char** c = kMainLibCandidates; *c; ++c) {
            g_main_handle = dlopen(*c, flags);
            if (g_main_handle) {
                LOG("dlopen(%s) ok", *c);
                break;
            }
        }
    }
    if (!g_main_handle) {
        LOG("could not load main library: %s", dlerror());
        return false;
    }

    g_resolve = (resolve_fn_t)real_dlsym(g_main_handle, "at_overlay_resolve_hook");
    if (!g_resolve) {
        LOG("main library missing at_overlay_resolve_hook — wrong version?");
        dlclose(g_main_handle);
        g_main_handle = nullptr;
        return false;
    }
    return true;
}

static bool ensure_main()
{
    if (g_main_handle) return true;
    if (g_skip_via_blacklist) return false;
    pthread_mutex_lock(&g_load_mutex);
    bool ok = load_main_locked();
    pthread_mutex_unlock(&g_load_mutex);
    return ok;
}

static void* resolve(const char* name, void** slot)
{
    void* p = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (p) return p;
    if (!ensure_main()) return nullptr;
    p = g_resolve(name);
    __atomic_store_n(slot, p, __ATOMIC_RELEASE);
    return p;
}

// ===============================================================
// Constructor
// ===============================================================

__attribute__((constructor))
static void shim_ctor()
{
    g_debug = (std::getenv("AT_OVERLAY_DEBUG_SHIM") != nullptr);
    LOG("loaded into %s", program_invocation_short_name ? program_invocation_short_name : "?");

    if (!std::getenv("AT_OVERLAY_LAZY")) {
        (void)ensure_main();
    }
}

EXPORT void* glXGetProcAddress(const char* procName)
{
    typedef void* (*F)(const char*);
    void* p = resolve("glXGetProcAddress", &g_glXGetProcAddress);
    if (p) return ((F)p)(procName);
    static F real = (F)real_dlsym(RTLD_NEXT, "glXGetProcAddress");
    return real ? real(procName) : nullptr;
}

EXPORT void* glXGetProcAddressARB(const char* procName)
{
    typedef void* (*F)(const char*);
    void* p = resolve("glXGetProcAddressARB", &g_glXGetProcAddressARB);
    if (p) return ((F)p)(procName);
    static F real = (F)real_dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    return real ? real(procName) : nullptr;
}

EXPORT void glXSwapBuffers(void* display, void* drawable)
{
    typedef void (*F)(void*, void*);
    void* p = resolve("glXSwapBuffers", &g_glXSwapBuffers);
    if (p) {
        ((F)p)(display, drawable);
        return;
    }
    static F real = (F)real_dlsym(RTLD_NEXT, "glXSwapBuffers");
    if (real) real(display, drawable);
}

EXPORT int64_t glXSwapBuffersMscOML(void* display, void* drawable,
                                    int64_t target_msc, int64_t divisor,
                                    int64_t remainder)
{
    typedef int64_t (*F)(void*, void*, int64_t, int64_t, int64_t);
    void* p = resolve("glXSwapBuffersMscOML", &g_glXSwapBuffersMscOML);
    if (p) return ((F)p)(display, drawable, target_msc, divisor, remainder);
    static F real = (F)real_dlsym(RTLD_NEXT, "glXSwapBuffersMscOML");
    return real ? real(display, drawable, target_msc, divisor, remainder) : -1;
}

EXPORT void glXDestroyContext(void* display, void* context)
{
    typedef void (*F)(void*, void*);
    void* p = resolve("glXDestroyContext", &g_glXDestroyContext);
    if (p) {
        ((F)p)(display, context);
        return;
    }
    static F real = (F)real_dlsym(RTLD_NEXT, "glXDestroyContext");
    if (real) real(display, context);
}

EXPORT unsigned long glXCreateWindow(void* display, void* config,
                                     unsigned long win, const int* attribList)
{
    typedef unsigned long (*F)(void*, void*, unsigned long, const int*);
    void* p = resolve("glXCreateWindow", &g_glXCreateWindow);
    if (p) return ((F)p)(display, config, win, attribList);
    static F real = (F)real_dlsym(RTLD_NEXT, "glXCreateWindow");
    return real ? real(display, config, win, attribList) : 0;
}

EXPORT void* eglGetProcAddress(const char* procName)
{
    typedef void* (*F)(const char*);
    void* p = resolve("eglGetProcAddress", &g_eglGetProcAddress);
    if (p) return ((F)p)(procName);
    static F real = (F)real_dlsym(RTLD_NEXT, "eglGetProcAddress");
    return real ? real(procName) : nullptr;
}

EXPORT unsigned eglSwapBuffers(void* display, void* surface)
{
    typedef unsigned (*F)(void*, void*);
    void* p = resolve("eglSwapBuffers", &g_eglSwapBuffers);
    if (p) return ((F)p)(display, surface);
    static F real = (F)real_dlsym(RTLD_NEXT, "eglSwapBuffers");
    return real ? real(display, surface) : 0;
}

EXPORT unsigned eglDestroyContext(void* display, void* context)
{
    typedef unsigned (*F)(void*, void*);
    void* p = resolve("eglDestroyContext", &g_eglDestroyContext);
    if (p) return ((F)p)(display, context);
    static F real = (F)real_dlsym(RTLD_NEXT, "eglDestroyContext");
    return real ? real(display, context) : 0;
}

// ===============================================================
// real_dlsym/real_dlvsym — find libc's true implementations via elfhacks
// ===============================================================

typedef void* (*dlsym_fn_t)(void*, const char*);
typedef void* (*dlvsym_fn_t)(void*, const char*, const char*);

static dlsym_fn_t  s_real_dlsym  = nullptr;
static dlvsym_fn_t s_real_dlvsym = nullptr;
static pthread_once_t s_dl_once  = PTHREAD_ONCE_INIT;

static void resolve_real_dl_once()
{
    // Try libdl.so first (older glibc), fall back to libc.so (glibc 2.34+ folded libdl into libc)
    eh_obj_t obj;
    if (eh_find_obj(&obj, "*libdl.so*") == 0) {
        eh_find_sym(&obj, "dlsym",  (void**)&s_real_dlsym);
        eh_find_sym(&obj, "dlvsym", (void**)&s_real_dlvsym);
        eh_destroy_obj(&obj);
    }
    if (!s_real_dlsym) {
        if (eh_find_obj(&obj, "*libc.so*") == 0) {
            eh_find_sym(&obj, "dlsym",  (void**)&s_real_dlsym);
            eh_find_sym(&obj, "dlvsym", (void**)&s_real_dlvsym);
            eh_destroy_obj(&obj);
        }
    }
    LOG("real_dlsym=%p real_dlvsym=%p",
        (void*)s_real_dlsym, (void*)s_real_dlvsym);
}

static void* real_dlsym(void* handle, const char* name)
{
    pthread_once(&s_dl_once, resolve_real_dl_once);
    return s_real_dlsym ? s_real_dlsym(handle, name) : nullptr;
}

static void* real_dlvsym(void* handle, const char* name, const char* version)
{
    pthread_once(&s_dl_once, resolve_real_dl_once);
    return s_real_dlvsym ? s_real_dlvsym(handle, name, version) : nullptr;
}

static void* our_hook_for(const char* name)
{
    if (!name) return nullptr;
#define M(s) if (std::strcmp(name, #s) == 0) return (void*)&s
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
    return nullptr;
}

EXPORT void* dlsym(void* handle, const char* name)
{
    if (!name) return real_dlsym(handle, name);
    if (void* ours = our_hook_for(name)) return ours;
    return real_dlsym(handle, name);
}

EXPORT void* dlvsym(void* handle, const char* name, const char* version)
{
    if (!name) return real_dlvsym(handle, name, version);
    if (void* ours = our_hook_for(name)) return ours;
    return real_dlvsym(handle, name, version);
}

