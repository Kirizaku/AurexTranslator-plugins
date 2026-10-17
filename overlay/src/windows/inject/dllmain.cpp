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

#include "win_common.h"
#include "host/ipc_pipe.h"

#include <string>
#include <vector>

namespace at {

bool hook_dxgi_install();
void hook_dxgi_uninstall();

namespace {

std::string pipe_name()
{
    return std::string("AurexTranslator_libat-overlay_") + process_name();
}

DWORD WINAPI init_thread(LPVOID)
{
    hlog("hook loaded into %s (%d-bit)", process_name(),
         (int)(sizeof(void*) * 8));

    if (!process_is_vk_overlay_target()) {
        hlog("not the current overlay target - skipping D3D/GL hook install");
        return 0;
    }

    Sleep(200);

    if (hook_dxgi_install()) {
        const std::string pipeName = pipe_name();
        IpcPipe pipe(pipeName, false);
        if (pipe.valid()) {
            pipe.send(MsgType::Status, "", StatusCode::Success);
        } else {
            hlog("failed to connect to app pipe %s", pipeName.c_str());
        }
    } else {
        hlog("no hooks installed — this process may not use D3D9/D3D10/D3D11/D3D12/OpenGL");
    }
    return 0;
}

} // namespace
} // namespace at

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, at::init_thread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
        break;

    case DLL_PROCESS_DETACH:
        if (reserved == nullptr) {
            at::hook_dxgi_uninstall();
        }
        break;
    }
    return TRUE;
}
