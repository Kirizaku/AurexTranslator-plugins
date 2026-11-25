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
#include "obs_gl_bridge.h"
#include "win_common.h"
#undef hlog
#include "../d3d/graphics-hook.h"
#undef capture_should_stop
#undef capture_should_init
#undef capture_ready
#include <cstdarg>

extern "C" {
void at_obs_gl_log(const char *format, ...)
{
    if (!at::log_enabled()) return;
    char message[960] = {};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    char line[1024] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[at-overlay:%lu] %s\n", GetCurrentProcessId(), message);
    at::log_write(line);
}
void at_obs_gl_log_hr(const char *message, HRESULT error)
{
    at_obs_gl_log("%s: HRESULT 0x%08lx", message, static_cast<unsigned long>(error));
}
bool at_obs_gl_should_stop()
{
    at_obs_gl_capture_begin();
    return at::capture_should_stop();
}
bool at_obs_gl_should_init() { return at::capture_should_init(); }
bool at_obs_gl_ready() { return obs_capture_ready(); }
bool at_obs_gl_active() { return at::capture_ready(); }
void at_obs_gl_free_transport() { ::capture_free(); }
bool at_obs_gl_init_shtex(shtex_data **out, HWND window, uint32_t width, uint32_t height,
                          uint32_t format, bool flip, uintptr_t handle)
{
    return ::capture_init_shtex(out, window, width, height, format, flip, handle);
}
bool at_obs_gl_init_shmem(shmem_data **out, HWND window, uint32_t width, uint32_t height,
                          uint32_t pitch, uint32_t format, bool flip)
{
    return ::capture_init_shmem(out, window, width, height, pitch, format, flip);
}
void at_obs_gl_copy_data(size_t index, void *volatile pixels) { ::shmem_copy_data(index, pixels); }
bool at_obs_gl_data_lock(int index) { return ::shmem_texture_data_lock(index); }
void at_obs_gl_data_unlock(int index) { ::shmem_texture_data_unlock(index); }
HMODULE at_obs_gl_system_module(const char *name) { return GetModuleHandleA(name); }
HMODULE at_obs_gl_load_library(const char *name) { return LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); }
const char *at_obs_gl_get_process_name() { return at::process_name(); }
}
