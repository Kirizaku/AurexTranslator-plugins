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

Derived in structure from third_party/obs-vkcapture/capture.c, including
its split of control_data.capturing (text session) from
control_data.allow_frame_capture (frame delivery) into independent bits -
see that file's header for why.
*/

#include "win_capture.h"
#include "win_common.h"
#include "overlay_text.h"

#include <dxgiformat.h>
#include <cstring>
#include <vector>

namespace at {
namespace {

struct State {
    HANDLE  pipe      = INVALID_HANDLE_VALUE;
    bool    accepted  = false; /* app wants the text overlay session active */
    bool    frame_capture_allowed = false; /* app permits frame delivery */
    bool    force_cpu_capture = false;
    bool    capturing = false; /* we have announced a texture */
    bool    gpu_capture = false;
    bool    cpu_fallback_supported = true;
    bool    cpu_switch_logged = false;
    int64_t last_check = 0;
    HANDLE  ev_request = nullptr; /* consumer -> game: "send a frame now" */
    HANDLE  ev_ready   = nullptr; /* game -> consumer: "fresh frame copied" */

    HANDLE  frame_mapping = nullptr;
    void*   frame_view    = nullptr;
    uint32_t shared_generation = 0;
};

State g;

bool try_connect()
{
    HANDLE h = CreateFileA(AT_OVERLAY_CAPTURE_PIPE,
                           GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(h, &mode, nullptr, nullptr)) {
        hlog("SetNamedPipeHandleState failed: %lu", GetLastError());
        CloseHandle(h);
        return false;
    }

    g.pipe = h;

    capture_client_data cd = {};
    cd.type = CAPTURE_CLIENT_DATA_TYPE;
    strncpy_s(cd.exe, sizeof(cd.exe), process_name(), _TRUNCATE);
    cd.pid = GetCurrentProcessId();

    DWORD written = 0;
    if (!WriteFile(g.pipe, &cd, CAPTURE_CLIENT_DATA_SIZE, &written, nullptr) ||
        written != CAPTURE_CLIENT_DATA_SIZE) {
        hlog("pipe write (client data) failed: %lu", GetLastError());
        CloseHandle(g.pipe);
        g.pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    hlog("connected to the app as \"%s\"", cd.exe);
    return true;
}

void disconnect()
{
    if (g.pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g.pipe);
        g.pipe = INVALID_HANDLE_VALUE;
    }
    g.accepted = false;
    g.frame_capture_allowed = false;
    g.force_cpu_capture = false;
    at_text_clear();
    at_session_set_active(0);
}

bool handle_message(const uint8_t* buf, size_t len)
{
    if (len < 1) return true;

    if (buf[0] == CAPTURE_GPU_FALLBACK_DATA_TYPE) {
        g.force_cpu_capture = true;
        hlog("CPU capture requested by the app");
        return true;
    }

    if (buf[0] == CAPTURE_TEXT_DATA_TYPE) {
        if (len < CAPTURE_TEXT_DATA_HEADER_SIZE) {
            hlog("short text message (%zu bytes)", len);
            return false;
        }
        capture_text_data td = {};
        memcpy(&td, buf, CAPTURE_TEXT_DATA_HEADER_SIZE);

        if (td.src_len > CAPTURE_TEXT_SRC_MAX ||
            td.text_len > CAPTURE_TEXT_TEXT_MAX ||
            len < CAPTURE_TEXT_DATA_HEADER_SIZE + (size_t)td.src_len + td.text_len) {
            hlog("malformed text message (src=%u text=%u len=%zu)", td.src_len, td.text_len, len);
            return false;
        }

        const char* src = (const char*)(buf + CAPTURE_TEXT_DATA_HEADER_SIZE);
        at_text_set(td.visible, src, td.src_len, src + td.src_len, td.text_len);
        return true;
    }

    if (buf[0] == CAPTURE_STYLE_DATA_TYPE) {
        if (len < CAPTURE_STYLE_DATA_SIZE) {
            hlog("short style message (%zu bytes)", len);
            return false;
        }
        capture_style_data sd = {};
        memcpy(&sd, buf, CAPTURE_STYLE_DATA_SIZE);
        at_text_set_style(sd.align, sd.text_color, sd.bg_color, sd.font_size,
                          sd.zone_x, sd.zone_y, sd.zone_w, sd.zone_h,
                          sd.font_size_auto, sd.corner_radius,
                          sd.margin_top, sd.margin_bottom,
                          sd.margin_left, sd.margin_right);
        return true;
    }

    if (buf[0] == CAPTURE_FONT_DATA_TYPE) {
        if (len < CAPTURE_FONT_DATA_HEADER_SIZE) {
            hlog("short font message (%zu bytes)", len);
            return false;
        }
        capture_font_data fd = {};
        memcpy(&fd, buf, CAPTURE_FONT_DATA_HEADER_SIZE);

        if (fd.path_len > CAPTURE_FONT_PATH_MAX ||
            len < CAPTURE_FONT_DATA_HEADER_SIZE + (size_t)fd.path_len) {
            hlog("malformed font message (path_len=%u len=%zu)", fd.path_len, len);
            return false;
        }

        const char* path = (const char*)(buf + CAPTURE_FONT_DATA_HEADER_SIZE);
        at_text_set_font_path(path, fd.path_len);
        return true;
    }

    if (buf[0] <= 1) {
        if (len < CAPTURE_CONTROL_DATA_SIZE) {
            hlog("short control message (%zu bytes)", len);
            return false;
        }
        capture_control_data ctl = {};
        memcpy(&ctl, buf, CAPTURE_CONTROL_DATA_SIZE);
        g.accepted = (ctl.capturing == 1);
        g.frame_capture_allowed = (ctl.allow_frame_capture == 1);
        if (!g.frame_capture_allowed)
            g.force_cpu_capture = false;
        hlog("capture control received: session=%s frame_capture=%s",
             g.accepted ? "active" : "inactive",
             g.frame_capture_allowed ? "allowed" : "denied");
        at_session_set_active(ctl.capturing == 1);
        return true;
    }

    hlog("unknown message type %u from the app", buf[0]);
    return false;
}

} // namespace

void capture_init()
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    g = State{};
}

void capture_shutdown()
{
    disconnect();
    if (g.frame_view) { UnmapViewOfFile(g.frame_view); g.frame_view = nullptr; }
    if (g.frame_mapping) { CloseHandle(g.frame_mapping); g.frame_mapping = nullptr; }
    if (g.ev_request) { CloseHandle(g.ev_request); g.ev_request = nullptr; }
    if (g.ev_ready) { CloseHandle(g.ev_ready); g.ev_ready = nullptr; }
}

void capture_update()
{
    const int64_t now = time_get_nano();
    if (now - g.last_check < 50000000LL) return; /* ~50ms: subtitle latency, not connect polling */
    g.last_check = now;

    if (g.pipe == INVALID_HANDLE_VALUE && !try_connect()) return;

    std::vector<uint8_t> buf;

    for (;;) {
        DWORD avail = 0, next_msg = 0;
        if (!PeekNamedPipe(g.pipe, nullptr, 0, nullptr, &avail, &next_msg)) {
            const DWORD err = GetLastError();
            if (err != ERROR_BROKEN_PIPE)
                hlog("PeekNamedPipe failed: %lu", err);
            disconnect();
            return;
        }
        if (next_msg == 0) break; /* no complete message waiting */

        const DWORD cap = CAPTURE_TEXT_DATA_HEADER_SIZE +
                          CAPTURE_TEXT_SRC_MAX + CAPTURE_TEXT_TEXT_MAX;
        if (next_msg > cap) {
            hlog("oversized message (%lu bytes), dropping peer", next_msg);
            disconnect();
            return;
        }

        buf.resize(next_msg);
        DWORD got = 0;
        if (!ReadFile(g.pipe, buf.data(), next_msg, &got, nullptr) ||
            got != next_msg) {
            hlog("ReadFile failed: %lu", GetLastError());
            disconnect();
            return;
        }

        if (!handle_message(buf.data(), got)) {
            hlog("win_capture: handle_message rejected %lu bytes (disconnecting)", got);
            disconnect();
            return;
        }
    }
}

void capture_stop()
{
    g.capturing = false;
    if (g.frame_view) {
        auto* hdr = reinterpret_cast<at_overlay_frame_header*>(g.frame_view);
        if (hdr->format == AT_OVERLAY_FRAME_FORMAT_D3D11_SHARED) {
            hdr->width = hdr->height = hdr->stride = 0;
            hdr->format = AT_OVERLAY_FRAME_FORMAT_BGRA8;
            hdr->reserved[0] = hdr->reserved[1] = hdr->reserved[2] = 0;
        }
    }
}

bool capture_should_stop()
{
    const bool client_stopped = g.pipe == INVALID_HANDLE_VALUE || !g.frame_capture_allowed;
    const bool switch_to_cpu = g.force_cpu_capture && g.gpu_capture &&
                               g.cpu_fallback_supported;
    if (switch_to_cpu && !g.cpu_switch_logged) {
        hlog("switching active GPU capture to CPU readback");
        g.cpu_switch_logged = true;
    }
    return g.capturing && (client_stopped || switch_to_cpu);
}

bool capture_should_init()
{
    return !g.capturing && g.pipe != INVALID_HANDLE_VALUE && g.frame_capture_allowed;
}

bool capture_ready()
{
    return g.capturing;
}

bool capture_force_cpu_requested()
{
    return g.force_cpu_capture;
}

static void ensure_events()
{
    if (!g.ev_request)
        g.ev_request = CreateEventA(nullptr, FALSE, FALSE, AT_OVERLAY_CAPTURE_REQUEST_EVENT);
    if (!g.ev_ready)
        g.ev_ready   = CreateEventA(nullptr, FALSE, FALSE, AT_OVERLAY_CAPTURE_READY_EVENT);
}

bool capture_frame_requested()
{
    ensure_events();
    if (!g.ev_request) return true; /* events unavailable: fall back to always-copy */
    return WaitForSingleObject(g.ev_request, 0) == WAIT_OBJECT_0;
}

void capture_frame_requeue()
{
    ensure_events();
    if (g.ev_request) SetEvent(g.ev_request);
}

void capture_frame_delivered()
{
    if (g.ev_ready) SetEvent(g.ev_ready);
}

static bool ensure_frame_mapping();

bool capture_cpu_format_supported(uint32_t format)
{
    return format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8X8_UNORM ||
           format == DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool capture_publish_d3d11_texture(uint32_t width, uint32_t height,
                                   uint32_t format, uint64_t shared_handle,
                                   bool keyed_mutex, bool flip_y,
                                   bool cpu_fallback_supported)
{
    if (width == 0 || height == 0 ||
        width > AT_OVERLAY_FRAME_MAX_WIDTH || height > AT_OVERLAY_FRAME_MAX_HEIGHT ||
        shared_handle == 0) {
        return false;
    }
    if (!capture_cpu_format_supported(format) && format != DXGI_FORMAT_R10G10B10A2_UNORM) {
        hlog("unsupported shared texture format %u", format);
        return false;
    }
    if (!ensure_frame_mapping()) return false;

    auto* hdr = reinterpret_cast<at_overlay_frame_header*>(g.frame_view);
    hdr->width = width;
    hdr->height = height;
    hdr->stride = (keyed_mutex ? AT_OVERLAY_D3D11_FLAG_KEYED_MUTEX : 0) |
                  (flip_y ? AT_OVERLAY_D3D11_FLAG_FLIP_Y : 0);
    hdr->format = AT_OVERLAY_FRAME_FORMAT_D3D11_SHARED;
    g.gpu_capture = true;
    g.cpu_fallback_supported = cpu_fallback_supported;
    if (++g.shared_generation == 0) ++g.shared_generation;
    hdr->frame_counter = g.shared_generation;
    hdr->reserved[0] = static_cast<uint32_t>(shared_handle & 0xffffffffu);
    hdr->reserved[1] = static_cast<uint32_t>(shared_handle >> 32);
    hdr->reserved[2] = format;
    return true;
}

static bool ensure_frame_mapping()
{
    if (g.frame_view) return true;

    const uint64_t size = AT_OVERLAY_FRAME_MAPPING_SIZE;
    g.frame_mapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        (DWORD)(size >> 32), (DWORD)(size & 0xFFFFFFFFu),
        AT_OVERLAY_FRAME_MAPPING);
    if (!g.frame_mapping) {
        hlog("CreateFileMapping(frame) failed: %lu", GetLastError());
        return false;
    }

    g.frame_view = MapViewOfFile(g.frame_mapping, FILE_MAP_WRITE, 0, 0, 0);
    if (!g.frame_view) {
        hlog("MapViewOfFile(frame) failed: %lu", GetLastError());
        CloseHandle(g.frame_mapping);
        g.frame_mapping = nullptr;
        return false;
    }
    return true;
}

bool capture_write_frame(int width, int height, const void* pixels_top_down, int src_pitch,
                         bool rgba_order, bool flip_y)
{
    if (width <= 0 || height <= 0 ||
        width > AT_OVERLAY_FRAME_MAX_WIDTH || height > AT_OVERLAY_FRAME_MAX_HEIGHT ||
        !pixels_top_down || src_pitch < width * 4) {
        return false;
    }
    if (!ensure_frame_mapping()) return false;

    auto* hdr = reinterpret_cast<at_overlay_frame_header*>(g.frame_view);
    uint8_t* dst = reinterpret_cast<uint8_t*>(g.frame_view) + AT_OVERLAY_FRAME_HEADER_SIZE;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(pixels_top_down);
    const int dst_pitch = width * 4;

    for (int y = 0; y < height; ++y) {
        const int src_y = flip_y ? height - 1 - y : y;
        const uint8_t* src_row = src + (size_t)src_y * src_pitch;
        uint8_t* dst_row = dst + (size_t)y * dst_pitch;
        if (rgba_order) {
            for (int x = 0; x < width; ++x) {
                dst_row[x * 4 + 0] = src_row[x * 4 + 2];
                dst_row[x * 4 + 1] = src_row[x * 4 + 1];
                dst_row[x * 4 + 2] = src_row[x * 4 + 0];
                dst_row[x * 4 + 3] = src_row[x * 4 + 3];
            }
        } else {
            memcpy(dst_row, src_row, (size_t)dst_pitch);
        }
    }

    hdr->width  = (uint32_t)width;
    hdr->height = (uint32_t)height;
    hdr->stride = (uint32_t)dst_pitch;
    hdr->format = AT_OVERLAY_FRAME_FORMAT_BGRA8;
    g.gpu_capture = false;
    hdr->frame_counter++;
    hdr->reserved[0] = hdr->reserved[1] = hdr->reserved[2] = 0;
    return true;
}

void capture_frame_capture_started()
{
    g.capturing = true;
}

void capture_mark_cpu_capture()
{
    g.gpu_capture = false;
}

void capture_disable_cpu_fallback()
{
    g.cpu_fallback_supported = false;
}

} // namespace at
