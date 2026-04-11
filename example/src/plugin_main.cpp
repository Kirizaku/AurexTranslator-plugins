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
        shm->send(msg);
    }
    return original_fwrite_trampoline(ptr, size, nmemb, stream);
}

static void* make_trampoline(void* orig, size_t original_len)
{
#if defined(__x86_64__) || defined(_M_X64)
#if defined(_WIN32)
    const size_t tramp_len = original_len + 15;
#else
    const size_t tramp_len = original_len + 12;
#endif
    void* tramp = nullptr;

#if defined(_WIN32)
    tramp = VirtualAlloc(nullptr,
        tramp_len,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
#else
    tramp = mmap(nullptr,
        tramp_len,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);
    if (tramp == MAP_FAILED) return nullptr;
#endif

    memcpy(tramp, orig, original_len);

    unsigned char jmp_abs[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    memcpy(static_cast<char*>(tramp) + original_len, jmp_abs, sizeof(jmp_abs));

    uint64_t target = reinterpret_cast<uint64_t>(orig) + original_len;
    memcpy(static_cast<char*>(tramp) + original_len + sizeof(jmp_abs), &target, sizeof(target));

    return tramp;
#else
    const size_t tramp_len = original_len + 5;
    void* tramp = nullptr;

#if defined(_WIN32)
    tramp = VirtualAlloc(nullptr,
        tramp_len,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
#else
    tramp = mmap(nullptr,
        tramp_len,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);
    if (tramp == MAP_FAILED) return nullptr;
#endif

    memcpy(tramp, orig, original_len);

    uint8_t* p = static_cast<uint8_t*>(tramp) + original_len;
    p[0] = 0xE9;
    uint32_t rel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(orig) + original_len - (reinterpret_cast<uintptr_t>(p) + 5));
    *reinterpret_cast<uint32_t*>(p + 1) = rel;

    return tramp;
#endif
}

#if defined(_WIN32)
constexpr std::size_t HOOK_SIZE = (sizeof(void*) == 8) ? 15U : 5U;
#else
constexpr std::size_t HOOK_SIZE = (sizeof(void*) == 8) ? 12U : 5U;
#endif
static uint8_t original_code[HOOK_SIZE];

// Cleanup
static void cleanup() {
    restore_hook(reinterpret_cast<uintptr_t>(original_fwrite_addr), original_code, HOOK_SIZE);

    shm->cleanup();
#if defined(__linux__)
    shm->unlink();
#endif
    delete shm;
}

// Init
static void init() {
#if defined(_WIN32)
    const std::string SHM_NAME = "AurexTranslator_test_target.exe";
#else
    const std::string SHM_NAME = "AurexTranslator_test_target";
#endif
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
        cleanup();
        return;
    }

    original_fwrite_trampoline = reinterpret_cast<fwrite_func_t>(make_trampoline(original_fwrite_addr, HOOK_SIZE));
    std::memcpy(original_code, original_fwrite_addr, HOOK_SIZE);
    install_hook(reinterpret_cast<uintptr_t>(original_fwrite_addr), reinterpret_cast<void*>(my_fwrite), HOOK_SIZE);

    shm->send("success");
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
