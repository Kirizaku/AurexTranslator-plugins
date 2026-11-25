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

#include "overlay_ui.h"
#include "overlay_text.h"

#include "imgui.h"
#include <atomic>

namespace {
std::atomic<const char *> g_capture_method{"Waiting"};
}

void at_overlay_set_capture_method(const char *capture_method)
{
    g_capture_method.store(capture_method ? capture_method : "Waiting",
                           std::memory_order_relaxed);
}

void at_overlay_draw_ui(const char *backend, int width, int height,
                        const char *capture_method)
{
    if (at_session_is_active())
        at_text_draw();

#ifndef NDEBUG
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##at-hud", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    if (backend && *backend)
        ImGui::Text("at-overlay (%s) %dx%d", backend, width, height);
    else
        ImGui::Text("at-overlay %dx%d", width, height);
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Text("Capture: %s", capture_method ? capture_method :
                 g_capture_method.load(std::memory_order_relaxed));
    ImGui::End();
#endif
}
