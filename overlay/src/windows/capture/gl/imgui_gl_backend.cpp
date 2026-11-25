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

// Keep the upstream renderer intact and add a context-loss cleanup path
#include "../../../../third_party/imgui/backends/imgui_impl_opengl3.cpp"

void at_imgui_gl_abandon()
{
    ImGui_ImplOpenGL3_Data *backend = ImGui_ImplOpenGL3_GetBackendData();
    if (!backend) return;

    ImGuiIO &io = ImGui::GetIO();
    ImGuiPlatformIO &platform = ImGui::GetPlatformIO();
    for (ImTextureData *texture : platform.Textures) {
        if (texture->RefCount == 1) {
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    platform.ClearRendererHandlers();
    IM_DELETE(backend);
    ImGui_ImplOpenGL3_ShutdownLoader();
}
