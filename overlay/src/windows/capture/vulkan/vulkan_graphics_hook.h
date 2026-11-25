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
#include "capture_bridge.h"
struct shtex_data { int unused; };
struct obs_vk_settings { bool allow_srgb_alias; bool force_shmem; };
extern struct obs_vk_settings *global_hook_info;
bool at_obs_vk_ready(void);
bool at_obs_vk_init_shtex(struct shtex_data **out, HWND window, uint32_t width,
                          uint32_t height, uint32_t format, bool flip, uintptr_t handle);
#undef capture_ready
#define capture_ready() at_obs_vk_ready()
#define capture_init_shtex at_obs_vk_init_shtex
