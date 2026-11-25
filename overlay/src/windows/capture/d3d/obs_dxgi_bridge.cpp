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

#include "win_capture.h"
#include "obs_capture_bridge.h"
#include "win_common.h"
#undef hlog

#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgiformat.h>
#include <cstdarg>
#include <cstdio>
#include "graphics-hook.h"
#undef capture_should_stop
#undef capture_should_init
#undef capture_ready

namespace {
enum class CaptureMode { None, Gpu, Cpu };

struct BridgeState {
    CaptureMode mode = CaptureMode::None;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint32_t format = 0;
    bool flip_y = false;
    bool mapped[NUM_BUFFERS] = {};
    bool gpu_request = false;
    bool cpu_request = false;
    bool cpu_frame_logged = false;
    ULONGLONG retry_after = 0;
};

BridgeState state;
hook_info hook_settings = {};
shtex_data shtex_token;
shmem_data shmem_token;

void before_capture()
{
    state.gpu_request = false;
    const bool force_cpu = at::capture_force_cpu_requested();
    if (!at::capture_ready() && !at::capture_should_init()) {
        hook_settings.force_shmem = force_cpu;
        state.retry_after = 0;
    } else if (force_cpu) {
        hook_settings.force_shmem = true;
    }
}

void after_capture(bool initializing, bool tried_cpu)
{
    if (state.gpu_request) {
        at::capture_frame_delivered();
        state.gpu_request = false;
    }
    if (initializing && !at::capture_ready()) {
        if (!tried_cpu)
            hook_settings.force_shmem = true;
        else
            state.retry_after = GetTickCount64() + 1000;
    }
}

template<typename Capture>
void capture_dxgi(Capture capture, void *swap, void *backbuffer)
{
    before_capture();
    const bool initializing = at::capture_should_init();
    if (initializing && GetTickCount64() < state.retry_after)
        return;
    const bool tried_cpu = hook_settings.force_shmem;
    capture(swap, backbuffer);
    after_capture(initializing, tried_cpu);
}
}

hook_info *global_hook_info = &hook_settings;

void at_obs_before_capture() { before_capture(); }
void at_obs_after_capture(bool initializing, bool tried_cpu) { after_capture(initializing, tried_cpu); }
void at_obs_dxgi_capture(void (*capture)(void *, void *), void *swap, void *backbuffer)
{
    capture_dxgi(capture, swap, backbuffer);
}
bool at_obs_uses_gpu() { return state.mode == CaptureMode::Gpu && at::capture_ready(); }
bool at_obs_uses_cpu() { return state.mode == CaptureMode::Cpu && at::capture_ready(); }

void hlog(const char *format, ...)
{
    if (!at::log_enabled()) return;
    char message[960] = {};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    char line[1024] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[at-overlay:%lu] %s\n",
                GetCurrentProcessId(), message);
    at::log_write(line);
}

void hlog_hr(const char *message, HRESULT error)
{
    hlog("%s: HRESULT 0x%08lx", message, static_cast<unsigned long>(error));
}

bool obs_capture_should_stop() { return at::capture_should_stop(); }
bool obs_capture_should_init() { return at::capture_should_init(); }
bool obs_capture_ready()
{
    if (!at::capture_ready()) return false;
    if (state.mode == CaptureMode::Cpu) return true;
    if (state.mode != CaptureMode::Gpu) return false;
    state.gpu_request = at::capture_frame_requested();
    return state.gpu_request;
}

void capture_free()
{
    if (state.gpu_request || state.cpu_request)
        at::capture_frame_requeue();
    state.mode = CaptureMode::None;
    state.gpu_request = false;
    state.cpu_request = false;
    for (bool &mapped : state.mapped) mapped = false;
    at::capture_stop();
}

bool capture_init_shtex(shtex_data **out, HWND, uint32_t width, uint32_t height,
                        uint32_t format, bool flip_y, uintptr_t handle)
{
    if (!at::capture_publish_d3d11_texture(width, height, format, handle, false, flip_y))
        return false;
    *out = &shtex_token;
    state.mode = CaptureMode::Gpu;
    state.cpu_frame_logged = false;
    hlog("capture initialized: GPU shared texture (%ux%u)", width, height);
    at::capture_frame_capture_started();
    return true;
}

bool capture_init_shmem(shmem_data **out, HWND, uint32_t width, uint32_t height,
                        uint32_t pitch, uint32_t format, bool flip_y)
{
    if (!at::capture_cpu_format_supported(format) || width == 0 || height == 0 ||
        width > AT_OVERLAY_FRAME_MAX_WIDTH || height > AT_OVERLAY_FRAME_MAX_HEIGHT ||
        pitch < width * 4) {
        hlog("unsupported DXGI CPU capture format %u", format);
        return false;
    }
    *out = &shmem_token;
    state.width = width;
    state.height = height;
    state.pitch = pitch;
    state.format = format;
    state.flip_y = flip_y;
    state.mode = CaptureMode::Cpu;
    state.cpu_frame_logged = false;
    hlog("capture initialized: CPU readback (%ux%u, pitch=%u, format=%u)",
         width, height, pitch, format);
    at::capture_mark_cpu_capture();
    at::capture_frame_capture_started();
    return true;
}

void shmem_copy_data(size_t index, void *volatile pixels)
{
    if (index >= NUM_BUFFERS) return;
    state.mapped[index] = true;
    if (!state.cpu_request)
        state.cpu_request = at::capture_frame_requested();
    if (state.cpu_request &&
        at::capture_write_frame(static_cast<int>(state.width), static_cast<int>(state.height),
                                pixels, static_cast<int>(state.pitch),
                                state.format == DXGI_FORMAT_R8G8B8A8_UNORM, state.flip_y)) {
        if (!state.cpu_frame_logged) {
            hlog("first CPU frame delivered (%ux%u)", state.width, state.height);
            state.cpu_frame_logged = true;
        }
        at::capture_frame_delivered();
        state.cpu_request = false;
    }
}

bool shmem_texture_data_lock(int index)
{
    return index >= 0 && index < NUM_BUFFERS && state.mapped[index];
}

void shmem_texture_data_unlock(int index)
{
    if (index >= 0 && index < NUM_BUFFERS) state.mapped[index] = false;
}

namespace at::d3d10_capture {
bool uses_gpu() { return state.mode == CaptureMode::Gpu && at::capture_ready(); }
bool uses_cpu() { return state.mode == CaptureMode::Cpu && at::capture_ready(); }
}

namespace at::d3d11_capture {
bool uses_gpu() { return state.mode == CaptureMode::Gpu && at::capture_ready(); }
bool uses_cpu() { return state.mode == CaptureMode::Cpu && at::capture_ready(); }
}
