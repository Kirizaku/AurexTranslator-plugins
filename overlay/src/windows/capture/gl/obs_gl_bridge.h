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

#ifdef __cplusplus
extern "C" {
#endif

void at_obs_gl_release(void);
void at_obs_gl_abandon(void);
void at_obs_gl_force_cpu(bool force_cpu);
bool at_obs_gl_uses_gpu(void);
HDC at_obs_gl_capture_dc(void);
bool at_obs_gl_hook_install(void);
void at_obs_gl_capture_begin(void);
void at_obs_gl_capture_complete(void);

#ifdef __cplusplus
}
#endif
