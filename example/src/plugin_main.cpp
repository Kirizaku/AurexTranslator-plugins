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

#include "shared_memory.h"
#include "memory_utils.h"

#if defined(__linux__)
#include <dlfcn.h>
#endif
#include <cstdint>

static SharedMemory* shm = nullptr;
static void* original_fwrite_addr = nullptr;

typedef size_t(*fwrite_func_t)(const void* ptr, size_t size, size_t nmemb, FILE* stream);
static fwrite_func_t original_fwrite_trampoline = nullptr;

extern "C" size_t my_fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (stream == stdout) {
        std::string msg(static_cast<const char*>(ptr), size * nmemb);
        shm->send(MsgType::Text, msg);
    }
    return original_fwrite_trampoline(ptr, size, nmemb, stream);
}

static uint8_t original_code[64];
static size_t hook_size = 0;

// Cleanup
static void cleanup() {
    if (original_fwrite_addr)
        restore_hook(reinterpret_cast<uintptr_t>(original_fwrite_addr), original_code, hook_size);

    shm->cleanup();
#if defined(__linux__)
    shm->unlink();
#endif
    delete shm;
}

// Init
static void init() {
    const std::string SHM_NAME = "AurexTranslator_libat-example";
    const size_t SHM_SIZE = sizeof(SharedData);

    shm = new SharedMemory(SHM_NAME, SHM_SIZE, true);

#if defined(__linux__)
    original_fwrite_addr = dlsym(RTLD_NEXT, "fwrite");
#else
    const wchar_t* crt_libs[] = {
        L"msvcrt.dll",
        L"ucrtbase.dll",
    };

    for (const auto& lib : crt_libs) {
        HMODULE crt_module = GetModuleHandleW(lib);
        if (crt_module) {
            original_fwrite_addr = reinterpret_cast<void*>(GetProcAddress(crt_module, "fwrite"));
            if (original_fwrite_addr) {
                break;
            }
        }
    }
#endif
    if (!original_fwrite_addr) {
        shm->send(MsgType::Status, "fwrite not found", StatusCode::Failure);
        return;
    }

    constexpr std::size_t MIN_HOOK_SIZE = (sizeof(void*) == 8) ? 12U : 5U;

    hook_size = get_patch_length(original_fwrite_addr, MIN_HOOK_SIZE);
    original_fwrite_trampoline = reinterpret_cast<fwrite_func_t>(create_trampoline_with_prolog(reinterpret_cast<uintptr_t>(original_fwrite_addr), hook_size));
    std::memcpy(original_code, original_fwrite_addr, hook_size);
    install_hook(reinterpret_cast<uintptr_t>(original_fwrite_addr), reinterpret_cast<void*>(my_fwrite), hook_size);

    shm->send(MsgType::Status, "", StatusCode::Success);
}

#if defined(_WIN32)
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: init(); break;
    case DLL_PROCESS_DETACH: cleanup(); break;
    }
    return TRUE;
}
#else
__attribute__((constructor)) void on_load() { init(); }
__attribute__((destructor)) void on_unload() { cleanup(); }
#endif
