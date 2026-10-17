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

#ifdef __cplusplus
extern "C" {
#endif

struct at_gl_fb_funcs {
    void (*GetIntegerv)(unsigned pname, int *params);
    void (*BindFramebuffer)(unsigned target, unsigned framebuffer);
    void *(*GetCurrentContext)(void);
};

void at_overlay_gl_draw(int width, int height, const struct at_gl_fb_funcs *fb);
void at_overlay_gl_context_destroyed(void);

#ifdef __cplusplus
}
#endif
