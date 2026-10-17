/******************************************************************************
    at-overlay - in-game overlay for AurexTranslator

    Copyright (C) 2026 by Daniil Nabiulin
    Copyright (C) by Lain Bailey and the OBS Studio contributors (obs-studio)

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
D3D10 swapchain overlay + capture (interface)

Structure parallels obs-studio/plugins/win-capture/graphics-hook
(d3d10-capture.c), rewritten for this project
*/

#pragma once

struct IDXGISwapChain;

namespace at {

bool d3d10_try_present(IDXGISwapChain* swap);
void d3d10_on_resize();
void d3d10_shutdown();

} // namespace at
