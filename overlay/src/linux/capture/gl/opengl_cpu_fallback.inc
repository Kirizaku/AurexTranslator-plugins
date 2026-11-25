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

struct gl_cpu_async_state {
    void *context;
    GLuint buffer;
    GLsync fence;
    int width;
    int height;
    bool initialized;
    bool disabled;
    PFNGLGENBUFFERSPROC GenBuffers;
    PFNGLDELETEBUFFERSPROC DeleteBuffers;
    PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBUFFERDATAPROC BufferData;
    PFNGLGETBUFFERPARAMETERIVPROC GetBufferParameteriv;
    PFNGLMAPBUFFERRANGEPROC MapBufferRange;
    PFNGLUNMAPBUFFERPROC UnmapBuffer;
    PFNGLFENCESYNCPROC FenceSync;
    PFNGLCLIENTWAITSYNCPROC ClientWaitSync;
    PFNGLDELETESYNCPROC DeleteSync;
};

static uint8_t *cpu_readbuf;
static uint8_t *cpu_flipbuf;
static size_t cpu_bufsize;
static PFNGLREADPIXELSPROC cpu_gl_read_pixels;
static PFNGLPIXELSTOREIPROC cpu_gl_pixel_store_i;
static PFNGLBINDBUFFERPROC cpu_gl_bind_buffer;
static struct gl_cpu_async_state cpu_async;

struct gl_cpu_read_state {
    GLint framebuffer;
    GLint pixel_pack_buffer;
    GLint read_buffer;
    GLint alignment;
    GLint row_length;
    GLint skip_rows;
    GLint skip_pixels;
};

static void *gl_cpu_current_context(bool glx)
{
    if (!glx)
        return egl_f.GetCurrentContext ? egl_f.GetCurrentContext() : NULL;
    void *(*get_context)(void) = glx_f.GetProcAddress ?
        (void *(*)(void))glx_f.GetProcAddress("glXGetCurrentContext") : NULL;
    return get_context ? get_context() : NULL;
}

static bool gl_cpu_capture_size(int width, int height, size_t *bytes)
{
    if (width <= 0 || height <= 0 || (uint64_t)width * (uint64_t)height * 4u > INT32_MAX)
        return false;

    *bytes = (size_t)width * (size_t)height * 4u;
    return true;
}

static void gl_cpu_async_free(void)
{
    void *current = gl_cpu_current_context(data.glx);
    if (cpu_async.context && cpu_async.context == current) {
        if (cpu_async.fence && cpu_async.DeleteSync)
            cpu_async.DeleteSync(cpu_async.fence);
        if (cpu_async.buffer && cpu_async.DeleteBuffers)
            cpu_async.DeleteBuffers(1, &cpu_async.buffer);
    }
    memset(&cpu_async, 0, sizeof(cpu_async));
}

static void gl_cpu_context_destroyed(void *context)
{
    if (context && cpu_async.context == context)
        gl_cpu_async_free();
}

static bool gl_cpu_async_init(bool glx)
{
    void *context = gl_cpu_current_context(glx);
    if (!context) return false;
    if (cpu_async.initialized && cpu_async.context == context)
        return !cpu_async.disabled;

    gl_cpu_async_free();
    cpu_async.context = context;
    cpu_async.initialized = true;
    cpu_async.GenBuffers = (PFNGLGENBUFFERSPROC)gl_f.GetProcAddress("glGenBuffers");
    cpu_async.DeleteBuffers = (PFNGLDELETEBUFFERSPROC)gl_f.GetProcAddress("glDeleteBuffers");
    cpu_async.BindBuffer = (PFNGLBINDBUFFERPROC)gl_f.GetProcAddress("glBindBuffer");
    cpu_async.BufferData = (PFNGLBUFFERDATAPROC)gl_f.GetProcAddress("glBufferData");
    cpu_async.GetBufferParameteriv = (PFNGLGETBUFFERPARAMETERIVPROC)gl_f.GetProcAddress("glGetBufferParameteriv");
    cpu_async.MapBufferRange = (PFNGLMAPBUFFERRANGEPROC)gl_f.GetProcAddress("glMapBufferRange");
    cpu_async.UnmapBuffer = (PFNGLUNMAPBUFFERPROC)gl_f.GetProcAddress("glUnmapBuffer");
    cpu_async.FenceSync = (PFNGLFENCESYNCPROC)gl_f.GetProcAddress("glFenceSync");
    cpu_async.ClientWaitSync = (PFNGLCLIENTWAITSYNCPROC)gl_f.GetProcAddress("glClientWaitSync");
    cpu_async.DeleteSync = (PFNGLDELETESYNCPROC)gl_f.GetProcAddress("glDeleteSync");

    const char *version = (const char *)gl_f.GetString(GL_VERSION);
    int major = 0, minor = 0;
    const bool version_ok = version && sscanf(version, "%d.%d", &major, &minor) == 2 &&
                            (major > 3 || (major == 3 && minor >= 2));
    cpu_async.disabled = !version_ok || !cpu_async.GenBuffers ||
        !cpu_async.DeleteBuffers || !cpu_async.BindBuffer || !cpu_async.BufferData ||
        !cpu_async.GetBufferParameteriv || !cpu_async.MapBufferRange ||
        !cpu_async.UnmapBuffer || !cpu_async.FenceSync ||
        !cpu_async.ClientWaitSync || !cpu_async.DeleteSync;
    if (!cpu_async.disabled)
        hlog("OpenGL CPU fallback: asynchronous PBO readback enabled");
    else
        hlog("OpenGL CPU fallback: PBO readback unavailable; using synchronous readback");
    return !cpu_async.disabled;
}

static void gl_cpu_read_begin(struct gl_cpu_read_state *saved, GLuint pbo)
{
    gl_f.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved->framebuffer);
    gl_f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved->pixel_pack_buffer);
    gl_f.GetIntegerv(GL_PACK_ALIGNMENT, &saved->alignment);
    gl_f.GetIntegerv(GL_PACK_ROW_LENGTH, &saved->row_length);
    gl_f.GetIntegerv(GL_PACK_SKIP_ROWS, &saved->skip_rows);
    gl_f.GetIntegerv(GL_PACK_SKIP_PIXELS, &saved->skip_pixels);
    gl_f.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl_f.GetIntegerv(GL_READ_BUFFER, &saved->read_buffer);
    cpu_gl_bind_buffer(GL_PIXEL_PACK_BUFFER, pbo);
    cpu_gl_pixel_store_i(GL_PACK_ALIGNMENT, 1);
    cpu_gl_pixel_store_i(GL_PACK_ROW_LENGTH, 0);
    cpu_gl_pixel_store_i(GL_PACK_SKIP_ROWS, 0);
    cpu_gl_pixel_store_i(GL_PACK_SKIP_PIXELS, 0);
    gl_f.ReadBuffer(GL_BACK);
}

static void gl_cpu_read_end(const struct gl_cpu_read_state *saved)
{
    gl_f.ReadBuffer((GLenum)saved->read_buffer);
    cpu_gl_pixel_store_i(GL_PACK_ALIGNMENT, saved->alignment);
    cpu_gl_pixel_store_i(GL_PACK_ROW_LENGTH, saved->row_length);
    cpu_gl_pixel_store_i(GL_PACK_SKIP_ROWS, saved->skip_rows);
    cpu_gl_pixel_store_i(GL_PACK_SKIP_PIXELS, saved->skip_pixels);
    gl_f.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved->framebuffer);
    cpu_gl_bind_buffer(GL_PIXEL_PACK_BUFFER, (GLuint)saved->pixel_pack_buffer);
}

static bool gl_cpu_async_collect(void)
{
    if (!cpu_async.fence) return false;
    const GLenum status = cpu_async.ClientWaitSync(cpu_async.fence, 0, 0);
    if (status == GL_TIMEOUT_EXPIRED) return false;

    cpu_async.DeleteSync(cpu_async.fence);
    cpu_async.fence = NULL;
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
        cpu_async.disabled = true;
        return false;
    }

    GLint old_pbo = 0;
    gl_f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pbo);
    cpu_async.BindBuffer(GL_PIXEL_PACK_BUFFER, cpu_async.buffer);
    const size_t stride = (size_t)cpu_async.width * 4u;
    const size_t bytes = stride * (size_t)cpu_async.height;
    const uint8_t *pixels = cpu_async.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                                    (GLsizeiptr)bytes, GL_MAP_READ_BIT);
    bool delivered = false;
    if (pixels) {
        delivered = capture_write_frame(cpu_async.width, cpu_async.height, pixels + (size_t)(cpu_async.height - 1) * stride, -(int)stride);
        if (!cpu_async.UnmapBuffer(GL_PIXEL_PACK_BUFFER)) delivered = false;
    } else {
        cpu_async.disabled = true;
    }
    cpu_async.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)old_pbo);
    if (delivered) capture_frame_delivered();
    return delivered;
}

static bool gl_cpu_async_submit(int width, int height)
{
    size_t bytes;
    if (!gl_cpu_capture_size(width, height, &bytes)) return false;
    if (!cpu_async.buffer) cpu_async.GenBuffers(1, &cpu_async.buffer);
    if (!cpu_async.buffer) return false;

    struct gl_cpu_read_state saved = {0};
    gl_cpu_read_begin(&saved, cpu_async.buffer);
    if (cpu_async.width != width || cpu_async.height != height) {
        cpu_async.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_READ);
        GLint actual_size = 0;
        cpu_async.GetBufferParameteriv(GL_PIXEL_PACK_BUFFER, GL_BUFFER_SIZE, &actual_size);
        if (actual_size < 0 || (size_t)actual_size != bytes) {
            gl_cpu_read_end(&saved);
            return false;
        }
    }
    cpu_gl_read_pixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    cpu_async.fence = cpu_async.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    cpu_async.width = width;
    cpu_async.height = height;
    gl_cpu_read_end(&saved);
    return cpu_async.fence != NULL;
}

static bool gl_cpu_sync_copy(int width, int height)
{
    size_t bytes;
    if (!gl_cpu_capture_size(width, height, &bytes)) return false;
    const size_t stride = (size_t)width * 4u;
    if (bytes > cpu_bufsize) {
        uint8_t *readbuf = realloc(cpu_readbuf, bytes);
        uint8_t *flipbuf = realloc(cpu_flipbuf, bytes);
        if (!readbuf || !flipbuf) {
            if (readbuf) cpu_readbuf = readbuf;
            if (flipbuf) cpu_flipbuf = flipbuf;
            return false;
        }
        cpu_readbuf = readbuf;
        cpu_flipbuf = flipbuf;
        cpu_bufsize = bytes;
    }

    GLint previous_fbo = 0, previous_alignment = 4;
    GLint previous_read_buffer = GL_BACK, previous_row_length = 0;
    GLint previous_skip_rows = 0, previous_skip_pixels = 0;
    GLint previous_pbo = 0;
    gl_f.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_fbo);
    gl_f.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous_pbo);
    gl_f.GetIntegerv(GL_PACK_ALIGNMENT, &previous_alignment);
    gl_f.GetIntegerv(GL_PACK_ROW_LENGTH, &previous_row_length);
    gl_f.GetIntegerv(GL_PACK_SKIP_ROWS, &previous_skip_rows);
    gl_f.GetIntegerv(GL_PACK_SKIP_PIXELS, &previous_skip_pixels);
    gl_f.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl_f.GetIntegerv(GL_READ_BUFFER, &previous_read_buffer);
    cpu_gl_bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
    cpu_gl_pixel_store_i(GL_PACK_ALIGNMENT, 1);
    cpu_gl_pixel_store_i(GL_PACK_ROW_LENGTH, 0);
    cpu_gl_pixel_store_i(GL_PACK_SKIP_ROWS, 0);
    cpu_gl_pixel_store_i(GL_PACK_SKIP_PIXELS, 0);
    gl_f.ReadBuffer(GL_BACK);
    cpu_gl_read_pixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, cpu_readbuf);
    cpu_gl_bind_buffer(GL_PIXEL_PACK_BUFFER, (GLuint)previous_pbo);
    gl_f.ReadBuffer((GLenum)previous_read_buffer);
    gl_f.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_fbo);
    cpu_gl_pixel_store_i(GL_PACK_ALIGNMENT, previous_alignment);
    cpu_gl_pixel_store_i(GL_PACK_ROW_LENGTH, previous_row_length);
    cpu_gl_pixel_store_i(GL_PACK_SKIP_ROWS, previous_skip_rows);
    cpu_gl_pixel_store_i(GL_PACK_SKIP_PIXELS, previous_skip_pixels);
    for (int y = 0; y < height; ++y)
        memcpy(cpu_flipbuf + (size_t)y * stride,
               cpu_readbuf + (size_t)(height - 1 - y) * stride, stride);
    return capture_write_frame(width, height, cpu_flipbuf, (int)stride);
}

/* Poll a previous PBO readback, then submit one only when AT requests a frame */
static void gl_cpu_capture_frame(int width, int height, bool glx)
{
    if (!cpu_gl_read_pixels)
        cpu_gl_read_pixels = (PFNGLREADPIXELSPROC)gl_f.GetProcAddress("glReadPixels");

    if (!cpu_gl_pixel_store_i)
        cpu_gl_pixel_store_i = (PFNGLPIXELSTOREIPROC)gl_f.GetProcAddress("glPixelStorei");

    if (!cpu_gl_bind_buffer)
        cpu_gl_bind_buffer = (PFNGLBINDBUFFERPROC)gl_f.GetProcAddress("glBindBuffer");
        
    if (!cpu_gl_read_pixels || !cpu_gl_pixel_store_i || !cpu_gl_bind_buffer) return;

    const bool async_available = gl_cpu_async_init(glx);
    if (cpu_async.fence) {
        gl_cpu_async_collect();
        if (cpu_async.fence) return;
    }
    if (!capture_frame_requested()) return;

    if (async_available && gl_cpu_async_submit(width, height))
        return;
        
    cpu_async.disabled = true;
    if (gl_cpu_sync_copy(width, height)) capture_frame_delivered();
}
