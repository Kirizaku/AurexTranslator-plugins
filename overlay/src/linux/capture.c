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
NOTE: This file is derived from obs-vkcapture/src/capture.c
Modifications:
  - DMA-BUF export removed (capture_init_shtex() and the allocation hints
    read from the control message); frames are delivered through shared
    memory instead, see capture_write_frame().
  - socket name: /com/obsproject/vkcapture -> /com/at_overlay/capture
  - env var:     OBS_VKCAPTURE_NAME       -> AT_OVERLAY_NAME
  - wine-preloader override kept as-is.
  - capture_try_connect() zeroes capture_client_data before filling it;
    upstream leaves the padding uninitialised and sends stack contents.
  - inbound framing rewritten (rx_buf/consume_rx): upstream assumes one
    recv() returns exactly one 32-byte control message, which the
    variable-length text channel makes untenable.
  - text messages dispatched to at_text_set(); at_text_clear() on
    disconnect. No upstream equivalent.
  - control_data.capturing (text session) and control_data.allow_frame_capture
    (frame delivery) are independent bits; capture_should_init()/
    capture_should_stop() gate only on the latter, so enabling the text
    overlay no longer also opens frame delivery. No upstream equivalent -
    upstream has a single capturing flag.
*/

#include "host/overlay_protocol.h"
#include "utils.h"
#include "overlay_text.h"

#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>

static struct {
    int connfd;
    bool accepted;
    bool frame_capture_allowed;
    bool capturing;
} data;

static bool get_wine_exe(char *buf, size_t bufsize)
{
    FILE *f = fopen("/proc/self/comm", "r");
    if (!f) {
        return false;
    }
    size_t n = fread(buf, sizeof(char), bufsize, f);
    fclose(f);
    if (n < 1) {
        return false;
    }
    buf[n - 1] = '\0';
    return true;
}

static bool get_exe(char *buf, size_t bufsize)
{
    char *custom_name = getenv("AT_OVERLAY_NAME");
    if (custom_name) {
        strncpy(buf, custom_name, bufsize);
        return true;
    }
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, PATH_MAX);
    if (n <= 0) {
        return false;
    }
    exe[n] = '\0';
    strncpy(buf, basename(exe), bufsize);
    buf[bufsize - 1] = '\0';
    if (!strcmp(buf, "wine-preloader") || !strcmp(buf, "wine64-preloader")) {
        return get_wine_exe(buf, bufsize);
    }
    return true;
}

static bool capture_try_connect(void)
{
    const char sockname[] = AT_OVERLAY_CAPTURE_SOCKET;

    struct sockaddr_un addr;
    addr.sun_family = PF_LOCAL;
    addr.sun_path[0] = '\0'; /* Abstract socket */
    memcpy(&addr.sun_path[1], sockname, sizeof(sockname) - 1);

    int sock = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int ret = connect(sock, (const struct sockaddr *)&addr,
            sizeof(addr.sun_family) + sizeof(sockname));
    if (ret == -1) {
        close(sock);
        return false;
    }

    data.connfd = sock;

    struct capture_client_data cd;
    memset(&cd, 0, sizeof(cd));
    cd.type = CAPTURE_CLIENT_DATA_TYPE;
    get_exe(cd.exe, sizeof(cd.exe));
    cd.pid = (uint32_t)getpid();

    struct msghdr msg = {0};
    struct iovec io = {
        .iov_base = &cd,
        .iov_len = CAPTURE_CLIENT_DATA_SIZE,
    };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    const ssize_t sent = sendmsg(data.connfd, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        hlog("Socket sendmsg error %s", strerror(errno));
    }

    return true;
}

void capture_init(void)
{
    memset(&data, 0, sizeof(data));
    data.connfd = -1;
}

#define CAPTURE_RX_BUF (CAPTURE_TEXT_DATA_HEADER_SIZE + \
                        CAPTURE_TEXT_SRC_MAX + CAPTURE_TEXT_TEXT_MAX)

static uint8_t rx_buf[CAPTURE_RX_BUF];
static size_t rx_len;

static void handle_control(const struct capture_control_data *control)
{
    data.accepted = control->capturing == 1;
    data.frame_capture_allowed = control->allow_frame_capture == 1;
    at_session_set_active(data.accepted ? 1 : 0);
}

static bool consume_rx(void)
{
    size_t off = 0;

    for (;;) {
        if (rx_len - off < 1) {
            break;
        }
        const uint8_t type = rx_buf[off];

        if (type == CAPTURE_TEXT_DATA_TYPE) {
            if (rx_len - off < CAPTURE_TEXT_DATA_HEADER_SIZE) {
                break;
            }
            struct capture_text_data td;
            memcpy(&td, rx_buf + off, CAPTURE_TEXT_DATA_HEADER_SIZE);

            if (td.src_len > CAPTURE_TEXT_SRC_MAX ||
                td.text_len > CAPTURE_TEXT_TEXT_MAX) {
                hlog("Text message too large (src=%u text=%u), dropping peer", td.src_len, td.text_len);
                return false;
            }

            const size_t total = CAPTURE_TEXT_DATA_HEADER_SIZE +
                                 td.src_len + td.text_len;
            if (rx_len - off < total) {
                break;  /* payload not complete yet */
            }

            const char *src = (const char *)(rx_buf + off +
                                             CAPTURE_TEXT_DATA_HEADER_SIZE);
            at_text_set(td.visible, src, td.src_len,
                        src + td.src_len, td.text_len);
            off += total;
            continue;
        }

        if (type <= 1) {
            if (rx_len - off < CAPTURE_CONTROL_DATA_SIZE) {
                break;
            }
            struct capture_control_data control;
            memcpy(&control, rx_buf + off, CAPTURE_CONTROL_DATA_SIZE);
            handle_control(&control);
            off += CAPTURE_CONTROL_DATA_SIZE;
            continue;
        }

        if (type == CAPTURE_STYLE_DATA_TYPE) {
            if (rx_len - off < CAPTURE_STYLE_DATA_SIZE) {
                break;
            }
            struct capture_style_data style;
            memcpy(&style, rx_buf + off, CAPTURE_STYLE_DATA_SIZE);
            at_text_set_style(style.align, style.text_color, style.bg_color,
                              style.font_size, style.zone_x, style.zone_y,
                              style.zone_w, style.zone_h, style.font_size_auto,
                              style.corner_radius, style.margin_top, style.margin_bottom,
                              style.margin_left, style.margin_right);
            off += CAPTURE_STYLE_DATA_SIZE;
            continue;
        }

        if (type == CAPTURE_FONT_DATA_TYPE) {
            if (rx_len - off < CAPTURE_FONT_DATA_HEADER_SIZE) {
                break;
            }
            struct capture_font_data fd;
            memcpy(&fd, rx_buf + off, CAPTURE_FONT_DATA_HEADER_SIZE);

            if (fd.path_len > CAPTURE_FONT_PATH_MAX) {
                hlog("Font path too large (%u), dropping peer", fd.path_len);
                return false;
            }

            const size_t total = CAPTURE_FONT_DATA_HEADER_SIZE + fd.path_len;
            if (rx_len - off < total) {
                break;
            }

            const char *path = (const char *)(rx_buf + off + CAPTURE_FONT_DATA_HEADER_SIZE);
            at_text_set_font_path(path, fd.path_len);
            off += total;
            continue;
        }

        hlog("Unknown message type %u from the app, dropping connection", type);
        return false;
    }

    if (off > 0) {
        rx_len -= off;
        if (rx_len > 0) {
            memmove(rx_buf, rx_buf + off, rx_len);
        }
    }
    return true;
}

void capture_update_socket(void)
{
    static int64_t last_check = 0;
    const int64_t now = os_time_get_nano();
    if (now - last_check < 1000000000) {
        return;
    }
    last_check = now;

    if (data.connfd < 0 && !capture_try_connect()) {
        return;
    }

    bool closed = false;

    for (;;) {
        if (rx_len >= sizeof(rx_buf)) {
            hlog("Receive buffer full, dropping peer");
            closed = true;
            break;
        }

        ssize_t n = recv(data.connfd, rx_buf + rx_len,
                         sizeof(rx_buf) - rx_len, 0);
        if (n > 0) {
            rx_len += (size_t)n;
            if (!consume_rx()) {
                closed = true;
                break;
            }
            continue;
        }
        if (n == 0) {
            closed = true;
            break;
        }
        /* n == -1 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != ECONNRESET) {
            hlog("Socket recv error %s", strerror(errno));
        }
        closed = true;
        break;
    }

    if (closed) {
        close(data.connfd);
        data.connfd = -1;
        data.accepted = false;
        data.frame_capture_allowed = false;
        rx_len = 0;
        at_text_clear();
        at_session_set_active(0);
    }
}

void capture_stop(void)
{
    data.capturing = false;
}

bool capture_should_stop(void)
{
    return data.capturing && (data.connfd < 0 || !data.frame_capture_allowed);
}

bool capture_should_init(void)
{
    return !data.capturing && data.connfd >= 0 && data.frame_capture_allowed;
}

bool capture_ready(void)
{
    return data.capturing;
}

static struct {
    int   fd;
    void *view;
    sem_t *req_sem;
    sem_t *ready_sem;
} frame;

static bool ensure_frame_mapping(void)
{
    if (frame.view) {
        return true;
    }

    frame.fd = shm_open(AT_OVERLAY_FRAME_MAPPING_POSIX, O_CREAT | O_RDWR, 0600);
    if (frame.fd < 0) {
        hlog("shm_open(frame) failed: %s", strerror(errno));
        return false;
    }
    if (ftruncate(frame.fd, (off_t)AT_OVERLAY_FRAME_MAPPING_SIZE) != 0) {
        hlog("ftruncate(frame) failed: %s", strerror(errno));
        close(frame.fd);
        frame.fd = -1;
        return false;
    }
    frame.view = mmap(NULL, (size_t)AT_OVERLAY_FRAME_MAPPING_SIZE,
                      PROT_READ | PROT_WRITE, MAP_SHARED, frame.fd, 0);
    if (frame.view == MAP_FAILED) {
        hlog("mmap(frame) failed: %s", strerror(errno));
        close(frame.fd);
        frame.fd = -1;
        frame.view = NULL;
        return false;
    }
    return true;
}

bool capture_write_frame(int width, int height, const void *bgra_top_down, int src_pitch)
{
    if (width <= 0 || height <= 0 ||
        width > AT_OVERLAY_FRAME_MAX_WIDTH || height > AT_OVERLAY_FRAME_MAX_HEIGHT) {
        return false;
    }
    if (!ensure_frame_mapping()) {
        return false;
    }

    struct at_overlay_frame_header *hdr = (struct at_overlay_frame_header *)frame.view;
    uint8_t *dst = (uint8_t *)frame.view + AT_OVERLAY_FRAME_HEADER_SIZE;
    const uint8_t *src = (const uint8_t *)bgra_top_down;
    const int dst_pitch = width * 4;

    for (int y = 0; y < height; ++y) {
        memcpy(dst + (size_t)y * dst_pitch, src + (size_t)y * src_pitch, (size_t)dst_pitch);
    }

    hdr->width  = (uint32_t)width;
    hdr->height = (uint32_t)height;
    hdr->stride = (uint32_t)dst_pitch;
    hdr->format = AT_OVERLAY_FRAME_FORMAT_BGRA8;
    hdr->frame_counter++;
    return true;
}

static void ensure_frame_sems(void)
{
    if (!frame.req_sem) {
        frame.req_sem = sem_open(AT_OVERLAY_CAPTURE_REQUEST_SEM, O_CREAT, 0600, 0);
        if (frame.req_sem == SEM_FAILED) {
            frame.req_sem = NULL;
        } else {
            while (sem_trywait(frame.req_sem) == 0) { /* drain */ }
        }
    }
    if (!frame.ready_sem) {
        frame.ready_sem = sem_open(AT_OVERLAY_CAPTURE_READY_SEM, O_CREAT, 0600, 0);
        if (frame.ready_sem == SEM_FAILED) frame.ready_sem = NULL;
    }
}

bool capture_frame_requested(void)
{
    ensure_frame_sems();
    if (!frame.req_sem) {
        return true;
    }
    return sem_trywait(frame.req_sem) == 0;
}

void capture_frame_delivered(void)
{
    if (frame.ready_sem) {
        sem_post(frame.ready_sem);
    }
}

void capture_frame_capture_started(void)
{
    data.capturing = true;
}
