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

#include "memory_utils.h"
#include "config.h"

#include "python_runtime.h"
#include "unicode_api.h"
#include "renpy_text.h"

static IpcPipe* g_pipe = nullptr;
static uint8_t orig_bytes[64];
static size_t patch_size = 0;

// Asm hook

extern "C" {
    void hook();
    uintptr_t g_jump_addr;
}

#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
extern "C" void Hook_PyUnicode_Format(
    PyObject* linux_format, PyObject* linux_args,
    PyObject* wine_format,  PyObject* wine_args)
{
    PyObject* format = g_is_wine ? wine_format : linux_format;
    PyObject* args   = g_is_wine ? wine_args   : linux_args;
#else
extern "C" void Hook_PyUnicode_Format(PyObject* format, PyObject* args) {
#endif
    const PyTypeObject* type = Py_TYPE(args);

    if (!type) return;
    if (!type->tp_name) return;
    if (!strstr(type->tp_name, "TagQuotingDict")) return;

    const char* text = resolve_substitutions(format);
    if (!text) return;

    std::string clean = strip_renpy_markup(text);

    g_pipe->send(MsgType::Text, clean.c_str(), StatusCode::Success, "Textbox");
}

static void init() {
#if defined(__linux__)
    g_is_wine = detect_wine();
#endif
    g_pipe = new IpcPipe(PIPE_NAME, false);

    void* python = find_python_module();
    g_unicode_api = detect_unicode_api(python);

    if (!g_unicode_api.fmt) {
        g_pipe->send(MsgType::Status, "PyUnicode not found", StatusCode::Failure);
        return;
    }
    patch_size = get_patch_length(g_unicode_api.fmt, MIN_HOOK_SIZE);

    std::memcpy(orig_bytes,reinterpret_cast<const void*>(g_unicode_api.fmt), patch_size);
    void* const  trampoline = create_trampoline_with_prolog(reinterpret_cast<uintptr_t>(g_unicode_api.fmt),patch_size);
    g_jump_addr = reinterpret_cast<uintptr_t>(trampoline);
    install_hook(reinterpret_cast<uintptr_t>(g_unicode_api.fmt), reinterpret_cast<void*>(hook), patch_size);

    g_pipe->send(MsgType::Status, "", StatusCode::Success);
}

static void cleanup() {
    if (g_unicode_api.fmt) {
        restore_hook(reinterpret_cast<uintptr_t>(g_unicode_api.fmt), orig_bytes, patch_size);
    }

    g_pipe->close();
#if defined(__linux__)
    g_pipe->unlink();
#endif
    delete g_pipe;
}

// Entry points

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
