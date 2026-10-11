/*
Licensed under the MIT License <http://opensource.org/licenses/MIT>.

Copyright (c) 2026 Daniil Nabiulin <https://github.com/kirizaku>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "host/ipc_pipe.h"
#include "config.h"

#include "hook_engine.h"
#include "il2cpp_runtime.h"
#include "mono_runtime.h"
#include "runtime_module.h"
#include "settings.h"
#include "text_sink.h"

#include <string>

static IpcPipe* g_pipe = nullptr;

void plugin_send_text(const char* source, uint8_t variant, const std::string& utf8) {
    if (IpcPipe* p = g_pipe)
        p->send(MsgType::Text, utf8, StatusCode::Success, source, variant);
}

// ===============================================================
// Entry points
// ===============================================================

static void init() {
#if defined(__linux__)
    g_is_wine = detect_wine();
#endif
    settings_load();
    g_pipe = new IpcPipe(PIPE_NAME, false);

    if (!hook_engine_init(sink_handler)) {
        g_pipe->send(MsgType::Status, "Unity: hook engine failed to start", StatusCode::Failure);
        return;
    }

    const char* why = nullptr;
    bool ok = mono_backend_init(&why);
    if (!ok && il2cpp_module_present())
        ok = il2cpp_backend_init(&why);
    if (!ok) {
        std::string msg = "Unity: ";
        msg += why;
        g_pipe->send(MsgType::Status, msg, StatusCode::Failure);
        return;
    }

    g_pipe->send(MsgType::Status, "", StatusCode::Success);
}

static void cleanup() {
    const bool drained = hook_engine_shutdown();

    IpcPipe* pipe = g_pipe;
    g_pipe = nullptr;
    if (!pipe) return;

    pipe->close();
#if defined(__linux__)
    pipe->unlink();
#endif
    if (drained) delete pipe;
}

#if defined(_WIN32)
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: init(); break;
    case DLL_PROCESS_DETACH: cleanup(); break;
    }
    return TRUE;
}
#else
__attribute__((constructor)) void on_load() { init(); }
__attribute__((destructor)) void on_unload() { cleanup(); }
#endif
