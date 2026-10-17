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
Direct3D9 overlay + capture on Windows (interface)

Structure parallels obs-studio/plugins/win-capture/graphics-hook (d3d9-capture.c),
rewritten for this project.
*/

#pragma once

namespace at {

bool hook_d3d9_install();
void hook_d3d9_uninstall();

} // namespace at
