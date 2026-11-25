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
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "obs_gl_bridge.h"

#define NUM_BUFFERS 3
#define global_hook_info at_obs_gl_hook_info
#define process_name at_obs_gl_process_name
#define hlog at_obs_gl_log
#define hlog_hr at_obs_gl_log_hr
#define capture_should_stop at_obs_gl_should_stop
#define capture_should_init at_obs_gl_should_init
#define capture_ready at_obs_gl_ready
#define capture_active at_obs_gl_active
#define capture_free at_obs_gl_free_transport
#define capture_init_shtex at_obs_gl_init_shtex
#define capture_init_shmem at_obs_gl_init_shmem
#define shmem_copy_data at_obs_gl_copy_data
#define shmem_texture_data_lock at_obs_gl_data_lock
#define shmem_texture_data_unlock at_obs_gl_data_unlock
#define get_system_module at_obs_gl_system_module
#define load_system_library at_obs_gl_load_library
#define get_process_name at_obs_gl_get_process_name

struct shtex_data;
struct shmem_data;
struct at_obs_gl_settings { bool force_shmem; bool capture_overlay; };
extern struct at_obs_gl_settings *global_hook_info;
extern char process_name[MAX_PATH];
void hlog(const char *format, ...);
void hlog_hr(const char *message, HRESULT error);
bool capture_should_stop(void);
bool capture_should_init(void);
bool capture_ready(void);
bool capture_active(void);
void capture_free(void);
bool capture_init_shtex(struct shtex_data **, HWND, uint32_t, uint32_t, uint32_t, bool, uintptr_t);
bool capture_init_shmem(struct shmem_data **, HWND, uint32_t, uint32_t, uint32_t, uint32_t, bool);
void shmem_copy_data(size_t, void *volatile);
bool shmem_texture_data_lock(int);
void shmem_texture_data_unlock(int);
HMODULE get_system_module(const char *);
HMODULE load_system_library(const char *);
const char *get_process_name(void);

/* AT renderer callbacks called by OBS's GL swap/context hooks */
void at_obs_gl_render_overlay(HDC hdc);
void at_obs_gl_context_destroyed(HGLRC context);
