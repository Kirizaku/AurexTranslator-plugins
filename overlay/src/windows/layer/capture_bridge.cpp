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

#include "capture_bridge.h"

#include "overlay_bridge.h"
#include "overlay_ui.h"
#include "win_capture.h"
#include "win_common.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dxgi1_2.h>

extern "C" void at_bridge_log(const char* format, ...)
{
    if (!at::log_enabled()) return;
    char message[960] = {};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    char line[1024] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[at-overlay:%lu] %s\n",
                GetCurrentProcessId(), message);
    at::log_write(line);
}

extern "C" void at_bridge_flog(const char* function, const char* format, ...)
{
    if (!at::log_enabled()) return;
    char body[800] = {};
    va_list args;
    va_start(args, format);
    vsnprintf_s(body, sizeof(body), _TRUNCATE, format, args);
    va_end(args);
    at_bridge_log("%s: %s", function, body);
}

extern "C" HMODULE at_bridge_load_system_library(const char* name)
{
    wchar_t path[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH - 1)
        return nullptr;

    const int nameLength = MultiByteToWideChar(CP_ACP, 0, name, -1,
                                                path + length + 1,
                                                MAX_PATH - length - 1);
    if (nameLength <= 0 || length + 1u + (UINT)nameLength > MAX_PATH)
        return nullptr;
    path[length] = L'\\';
    return LoadLibraryW(path);
}

extern "C" bool at_bridge_capture_should_stop(void) { return at::capture_should_stop(); }
extern "C" bool at_bridge_capture_should_init(void) { return at::capture_should_init(); }
extern "C" bool at_bridge_capture_ready(void) { return at::capture_ready(); }
extern "C" bool at_bridge_capture_force_cpu_requested(void)
{
    return at::capture_force_cpu_requested();
}
extern "C" void at_bridge_capture_stop(void) { at::capture_stop(); }
extern "C" bool at_bridge_capture_publish(uint32_t width, uint32_t height,
                                         uint32_t format, uint64_t handle)
{
    // OBS's Windows Vulkan backend only exports a D3D11 shared texture; it has
    // no shared-memory readback path, so keep that capture running on GPU
    return at::capture_publish_d3d11_texture(width, height, format, handle,
                                             false, false, false);
}
extern "C" bool at_bridge_process_is_target(void)
{
    return at::process_is_vk_overlay_target();
}
extern "C" void at_bridge_mark_vk_active(void) { at::mark_vk_overlay_active(); }
extern "C" void *at_bridge_find_dxgi_adapter(const uint8_t luid[8])
{
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    IDXGIAdapter1 *match = nullptr;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            std::memcmp(&desc.AdapterLuid, luid, sizeof(desc.AdapterLuid)) == 0) {
            match = adapter;
            break;
        }
        adapter->Release();
    }
    factory->Release();
    return match;
}
extern "C" void at_bridge_capture_init(void) { at::capture_init(); }
extern "C" void at_bridge_capture_update(void) { at::capture_update(); }
extern "C" void at_bridge_capture_set_method(const char* method)
{
    at_overlay_set_capture_method(method);
}
extern "C" void at_bridge_capture_frame_started(void) { at::capture_frame_capture_started(); }
extern "C" bool at_bridge_capture_frame_requested(void) { return at::capture_frame_requested(); }
extern "C" void at_bridge_capture_frame_delivered(void) { at::capture_frame_delivered(); }
