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
D3D12 swapchain overlay backend (interface)

Structure follows obs-studio/plugins/win-capture/graphics-hook
(d3d12-capture.c), rewritten for this project and retargeted from DrawData
capture to an in-place overlay
*/

#pragma once

struct IDXGISwapChain;

namespace at {

bool d3d12_hook_queue();
bool d3d12_try_present(IDXGISwapChain* swap);
void d3d12_on_resize();
void d3d12_shutdown();

} // namespace at
