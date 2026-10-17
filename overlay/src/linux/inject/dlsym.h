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
NOTE: Derived from obs-vkcapture/src/dlsym.h, unmodified.
*/

#pragma once

#include <stdbool.h>

struct dl_funcs {
    void *(*dlsym)(void *handle, const char *symbol);
    void *(*dlvsym)(void *handle, const char *symbol, const char *version);

    bool valid;
};

void *real_dlsym(void *handle, const char *symbol);
void *real_dlvsym(void *handle, const char *symbol, const char *version);
