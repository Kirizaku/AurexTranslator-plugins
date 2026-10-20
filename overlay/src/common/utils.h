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
NOTE: Derived from obs-vkcapture/src/utils.h.
Modifications: log tag and env var renamed; os_dupfd_cloexec() (only used by the removed DMA-BUF export) dropped
*/

#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int64_t os_time_get_nano(void)
{
    struct timespec tv;
    clock_gettime(
#if defined(CLOCK_MONOTONIC_RAW)
        CLOCK_MONOTONIC_RAW
#elif defined(CLOCK_MONOTONIC_FAST)
        CLOCK_MONOTONIC_FAST
#else
        CLOCK_MONOTONIC
#endif
        , &tv);
    return tv.tv_nsec + tv.tv_sec * INT64_C(1000000000);
}

static inline bool hlog_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *v = getenv("AT_OVERLAY_LOGFILE");
        enabled = v && *v && strcmp(v, "0") != 0;
    }
    return enabled;
}

static inline bool hlog_stderr(void)
{
    static int enabled = -1;
    if (enabled == -1) {
        const char *v = getenv("AT_OVERLAY_LOG_STDERR");
        enabled = v && *v && strcmp(v, "0") != 0;
    }
    return enabled;
}

static inline FILE *hlog_file(void)
{
    static FILE *f = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const char *v = getenv("AT_OVERLAY_LOGFILE");
        if (v && *v && strcmp(v, "0") != 0) {
            char path[1024] = {0};
            if (strchr(v, '/')) {
                snprintf(path, sizeof(path), "%s", v);
            } else {
                const char *xdg = getenv("XDG_CONFIG_HOME");
                const char *home = getenv("HOME");
                if (xdg && *xdg)
                    snprintf(path, sizeof(path), "%s/AurexTranslator/plugins/at-overlay-debug.log", xdg);
                else if (home)
                    snprintf(path, sizeof(path), "%s/.config/AurexTranslator/plugins/at-overlay-debug.log", home);
            }
            if (path[0]) f = fopen(path, "a");
        }
    }
    return f;
}

#define hlog(msg, ...) do { \
        if (hlog_enabled()) { \
            if (hlog_stderr()) \
                fprintf(stderr, "[at-overlay:%d] " msg "\n", (int)getpid(), ##__VA_ARGS__); \
            FILE *_at_lf = hlog_file(); \
            if (_at_lf) { \
                fprintf(_at_lf, "[at-overlay:%d] " msg "\n", (int)getpid(), ##__VA_ARGS__); \
                fflush(_at_lf); \
            } \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif
