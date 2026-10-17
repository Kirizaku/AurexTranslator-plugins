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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void at_text_set(int visible, const char *src,  uint32_t src_len, const char *text, uint32_t text_len);
void at_text_clear(void);
void at_session_set_active(int active);
void at_text_set_style(uint8_t align,
                       uint32_t text_color, uint32_t bg_color, float font_size,
                       float zone_x, float zone_y, float zone_w, float zone_h,
                       uint8_t font_size_auto, uint8_t corner_radius,
                       uint8_t margin_top, uint8_t margin_bottom,
                       uint8_t margin_left, uint8_t margin_right);

void at_text_set_font_path(const char *path, uint32_t path_len);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
void at_text_setup_font();
void at_text_draw();
bool at_text_font_dirty();
void at_text_font_rebuilt();
bool at_session_is_active();
#endif /* __cplusplus */
