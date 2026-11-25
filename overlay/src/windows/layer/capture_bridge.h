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

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void at_bridge_log(const char *format, ...);
void at_bridge_flog(const char *function, const char *format, ...);
HMODULE at_bridge_load_system_library(const char *name);
bool at_bridge_capture_should_stop(void);
bool at_bridge_capture_should_init(void);
bool at_bridge_capture_ready(void);
bool at_bridge_capture_force_cpu_requested(void);
void at_bridge_capture_stop(void);
bool at_bridge_capture_publish(uint32_t width, uint32_t height, uint32_t format,
			    uint64_t handle);
bool at_bridge_process_is_target(void);
void at_bridge_mark_vk_active(void);
void *at_bridge_find_dxgi_adapter(const uint8_t luid[8]);
void at_bridge_capture_init(void);
void at_bridge_capture_update(void);
void at_bridge_capture_set_method(const char *method);
void at_bridge_capture_frame_started(void);
bool at_bridge_capture_frame_requested(void);
void at_bridge_capture_frame_delivered(void);

#ifdef __cplusplus
}
#endif

#ifndef __cplusplus
#define hlog(...) at_bridge_log(__VA_ARGS__)
#define flog(...) at_bridge_flog(__FUNCTION__, __VA_ARGS__)
#define flog_hr(text, hr) at_bridge_log("%s: " text " (HRESULT=0x%08lx)", __FUNCTION__, (unsigned long)(hr))
#define load_system_library(name) at_bridge_load_system_library(name)
#define capture_should_stop() at_bridge_capture_should_stop()
#define capture_should_init() at_bridge_capture_should_init()
#define capture_ready() at_bridge_capture_ready()
#define capture_free() at_bridge_capture_stop()
#endif
