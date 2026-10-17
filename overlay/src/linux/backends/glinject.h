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
NOTE: Derived from obs-vkcapture/src, reduced to the entry points
the hooks and the CPU frame capture use; glx_funcs::CreateWindow's return
type is fixed (see below)
*/

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <GL/glcorearb.h>
#include <GL/glext.h>

struct gl_funcs {
    void *(*GetProcAddress)(const char*);
    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLREADBUFFERPROC ReadBuffer;
    PFNGLGETSTRINGPROC GetString;
    PFNGLREADPIXELSPROC ReadPixels;
    PFNGLPIXELSTOREIPROC PixelStorei;
};

#define P_EGL_HEIGHT 0x3056
#define P_EGL_WIDTH 0x3057

struct egl_funcs {
    void *(*GetProcAddress)(const char*);
    unsigned (*DestroyContext)(void *display, void *context);
    void *(*GetCurrentContext)();
    unsigned (*QuerySurface)(void *display, void *surface, int attribute, int *value);
    unsigned (*SwapBuffers)(void *display, void *surface);

    bool valid;
};

#define P_GLX_WIDTH 0x801D
#define P_GLX_HEIGHT 0x801E

struct glx_funcs {
    void *(*GetProcAddress)(const char*);
    void *(*GetProcAddressARB)(const char*);
    void (*DestroyContext)(void *display, void *context);
    // unsigned long (GLXWindow/XID), not bool as upstream has it - bool
    // truncates the drawable handle to 0/1 and breaks GLX 1.3 apps
    unsigned long (*CreateWindow)(void *display, void *config, unsigned long win, const int *attribList);
    void (*SwapBuffers)(void *display, void *drawable);
    int64_t (*SwapBuffersMscOML)(void *display, void *drawable, int64_t target_msc, int64_t divisor, int64_t remainder);
    void (*QueryDrawable)(void *display, void *drawable, int attribute, unsigned *value);
    void *(*GetCurrentContext)(void);

    bool valid;
};
