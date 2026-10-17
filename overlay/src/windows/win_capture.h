/******************************************************************************
    at-overlay - in-game overlay for AurexTranslator

    Copyright (C) 2026 by Daniil Nabiulin
    Copyright (C) 2021 by David Rosca <nowrep@gmail.com> (obs-vkcapture)

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
Windows capture/control channel (game side)

Windows counterpart of src/linux/capture.c: the same state machine and
128-byte messages over a named pipe instead of an abstract Unix socket
Frames go through shared memory (see overlay_protocol.h)
*/

#pragma once

#include <cstdint>

extern "C" {
#include "host/overlay_protocol.h"
}

namespace at {

void capture_init();
void capture_shutdown();
void capture_update();
void capture_force_disconnect();
void capture_stop();

bool capture_should_stop();
bool capture_should_init();
bool capture_ready();

bool capture_frame_requested();
void capture_frame_delivered();
bool capture_write_frame(int width, int height, const void* bgra_top_down, int src_pitch);
void capture_frame_capture_started();

} // namespace at
