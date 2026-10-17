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

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "host/overlay_protocol.h"
}

namespace at {

inline void swizzle_rgba_to_bgra(void* data, int width, int height, int pitch)
{
    for (int y = 0; y < height; ++y) {
        uint8_t* row = (uint8_t*)data + (size_t)y * pitch;
        for (int x = 0; x < width; ++x) {
            uint8_t* px = row + (size_t)x * 4;
            const uint8_t r = px[0];
            px[0] = px[2];
            px[2] = r;
        }
    }
}

// ===============================================================
// Logging
// ===============================================================

inline bool log_enabled()
{
    static int enabled = -1;
    if (enabled == -1) {
        char buf[8];
        const DWORD n = GetEnvironmentVariableA("AT_OVERLAY_LOGFILE", buf, sizeof(buf));
        enabled = (n > 0 && !(n == 1 && buf[0] == '0')) ? 1 : 0;
    }
    return enabled != 0;
}

inline void log_write(const char* line)
{
    OutputDebugStringA(line);

    static FILE* f = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        char path[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("AT_OVERLAY_LOGFILE", path, sizeof(path));
        if (n == 0 || n >= sizeof(path) || (!strchr(path, '\\') && !strchr(path, '/'))) {
            n = GetEnvironmentVariableA("LOCALAPPDATA", path, sizeof(path));
            if (n > 0 && n < sizeof(path) - 64) {
                strcat_s(path, sizeof(path), "\\AurexTranslator\\plugins\\at-overlay-debug.log");
            } else {
                n = 0;
            }
        }
        if (n > 0) {
            WIN32_FILE_ATTRIBUTE_DATA info;
            const bool tooBig = GetFileAttributesExA(path, GetFileExInfoStandard, &info)
                                && (info.nFileSizeHigh > 0 || info.nFileSizeLow > 4u * 1024 * 1024);
            f = fopen(path, tooBig ? "w" : "a");
        }
    }
    if (f) {
        fputs(line, f);
        fflush(f);
    }
}

#define hlog(...)                                                          \
    do {                                                                   \
        if (::at::log_enabled()) {                                         \
            char _msg[960];                                                \
            char _line[1024];                                              \
            _snprintf_s(_msg, sizeof(_msg), _TRUNCATE, __VA_ARGS__);       \
            _snprintf_s(_line, sizeof(_line), _TRUNCATE,                   \
                        "[at-overlay:%lu] %s\n",                           \
                        GetCurrentProcessId(), _msg);                      \
            ::at::log_write(_line);                                        \
        }                                                                  \
    } while (0)

// ===============================================================
// Environment
// ===============================================================

inline bool env_flag(const char* name)
{
    char buf[8];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && buf[0] != '0';
}

/* Cached variant for hot paths (per-frame checks) */
inline bool env_flag_cached(const char* name, int* cache)
{
    if (*cache == -1) *cache = env_flag(name) ? 1 : 0;
    return *cache != 0;
}

// ===============================================================
// Time
// ===============================================================

inline int64_t time_get_nano()
{
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart / freq.QuadPart) * 1000000000LL +
           ((now.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart;
}

inline float frame_delta()
{
    static int64_t last = 0;
    const int64_t now = time_get_nano();
    if (last == 0) { last = now; return 1.0f / 60.0f; }
    const float dt = (float)(now - last) / 1e9f;
    last = now;
    if (dt <= 0.0f) return 1.0f / 60.0f;
    return dt > 0.25f ? 0.25f : dt;
}

// ===============================================================
// Process name
// ===============================================================

inline const char* process_name()
{
    static char name[64] = {0};
    if (name[0]) return name;

    if (GetEnvironmentVariableA("AT_OVERLAY_NAME", name, sizeof(name)) > 0)
        return name;

    char path[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) {
        strcpy_s(name, sizeof(name), "unknown");
        return name;
    }
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    strncpy_s(name, sizeof(name), base, _TRUNCATE);
    return name;
}

template <typename T>
inline void safe_release(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

// ===============================================================
// Target allow-list
// ===============================================================

inline bool process_is_vk_overlay_target()
{
    static int cached = 0;
    static int64_t next_check = 0;
    const int64_t now = time_get_nano();
    if (now < next_check) return cached != 0;
    next_check = now + 1000000000LL;
    cached = 0;

    char appdata[MAX_PATH] = {0};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", appdata, sizeof(appdata));
    if (n == 0 || n >= sizeof(appdata)) return false;

    char path[MAX_PATH] = {0};
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\AurexTranslator\\plugins\\%s",
                appdata, AT_OVERLAY_VK_TARGETS_FILE);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return false;

    const char* me = process_name();
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';
        if (len > 0 && _stricmp(line, me) == 0) { cached = 1; break; }
    }
    fclose(f);
    return cached != 0;
}

inline std::atomic<bool> g_vk_overlay_active{false};

inline void signal_vk_chained_cross_process()
{
    char name[64];
    std::snprintf(name, sizeof(name), "Local\\at_overlay_vk_active_%lu",
                  (unsigned long)GetCurrentProcessId());
    static HANDLE h = CreateEventA(nullptr, TRUE, TRUE, name);
    (void)h;
}

inline void mark_vk_overlay_active()
{
    g_vk_overlay_active.store(true, std::memory_order_relaxed);
    signal_vk_chained_cross_process();
}

inline bool vk_overlay_is_active()
{
    return g_vk_overlay_active.load(std::memory_order_relaxed);
}

inline std::atomic<int>  g_hookRefCount{0};
inline std::atomic<bool> g_hookUnloading{false};

class HookEntry
{
public:
    HookEntry()
    {
        g_hookRefCount.fetch_add(1, std::memory_order_seq_cst);
        if (g_hookUnloading.load(std::memory_order_seq_cst))
            g_hookRefCount.fetch_sub(1, std::memory_order_seq_cst);
        else
            m_entered = true;
    }
    ~HookEntry()
    {
        if (m_entered) g_hookRefCount.fetch_sub(1, std::memory_order_seq_cst);
    }
    HookEntry(const HookEntry&) = delete;
    HookEntry& operator=(const HookEntry&) = delete;
    explicit operator bool() const { return m_entered; }

private:
    bool m_entered = false;
};

inline bool wait_for_hooks_idle(DWORD timeoutMs = 2000)
{
    g_hookUnloading.store(true, std::memory_order_seq_cst);
    const DWORD start = GetTickCount();
    while (g_hookRefCount.load(std::memory_order_seq_cst) > 0) {
        if (GetTickCount() - start > timeoutMs) return false;
        Sleep(1);
    }
    return true;
}

} // namespace at
