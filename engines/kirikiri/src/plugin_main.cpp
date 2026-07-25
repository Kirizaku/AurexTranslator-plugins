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

static IpcPipe* g_pipe = nullptr;
static uint8_t  g_orig_bytes[3][64];

struct HookSlot {
    void*           addr;
    size_t          patch_size;
    void*           trampoline;
    uintptr_t*      jump_slot;
    void            (*stub)();
    const uint8_t*  pattern;
    size_t          pattern_size;
    const char*     mask;
};

static const int kHookCount = 3;
static HookSlot  g_hooks[kHookCount];
static ModuleInfo g_module;

enum { H_KRKR_1 = 0, H_KRKR_2, H_KRKRZ };

std::string wide_char_to_utf8(wchar_t wc) {
    std::string result;
    uint32_t code = static_cast<uint16_t>(wc);

    if (code < 0x80) {
        result.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        result.push_back(static_cast<char>(0xC0 | (code >> 6)));
        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        result.push_back(static_cast<char>(0xE0 | (code >> 12)));
        result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    return result;
}

static std::string addr_source(uint32_t addr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", addr);
    return std::string(buf);
}

static std::string addr_source_with_ecx(uintptr_t hook_addr, uint32_t ecx_value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08llX-0x%X", static_cast<unsigned long long>(hook_addr), ecx_value);
    return std::string(buf);
}

extern "C" void Hook_KrkrChar1(uint32_t eax_value) {
    if (!eax_value) return;
    wchar_t ch = *reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(eax_value) + 0x14);
    if (!ch) return;
    if (g_pipe) g_pipe->send(MsgType::Text, wide_char_to_utf8(ch),
                     StatusCode::Success, addr_source(eax_value), /*variant=*/1);
}

extern "C" void Hook_KrkrChar2(uint32_t ebx_value, uint32_t ecx_value) {
    if (!ebx_value) return;
    wchar_t ch = *reinterpret_cast<const wchar_t*>(ebx_value - 2);
    if (!ch) return;
    uintptr_t hook_addr = reinterpret_cast<uintptr_t>(g_hooks[H_KRKR_2].addr);
    if (g_pipe) g_pipe->send(MsgType::Text, wide_char_to_utf8(ch),
                     StatusCode::Success, addr_source_with_ecx(hook_addr, ecx_value), /*variant=*/2);
}

extern "C" void Hook_KrkrzChar(uint32_t ecx_value) {
    if (!ecx_value) return;
    const uint8_t* obj = reinterpret_cast<const uint8_t*>(ecx_value);
    wchar_t ch   = *reinterpret_cast<const wchar_t*>(obj + 0x14);
    uint8_t flag = *(obj + 0x21); // 0x21 || 0x1c
    if (flag != 0 || !ch) return;
    if (g_pipe) g_pipe->send(MsgType::Text, wide_char_to_utf8(ch),
                     StatusCode::Success, addr_source(ecx_value), /*variant=*/3);
}

extern "C" {
    uintptr_t g_jump_krkr_1;
    uintptr_t g_jump_krkr_2;
    uintptr_t g_jump_krkrz;
    void hook_krkr_1();
    void hook_krkr_2();
    void hook_krkrz();
}

static bool install_one(HookSlot& s, void* base, size_t size, uint8_t* orig_out) {
    s.addr = find_pattern(base, size, s.pattern, s.mask);

    if (!s.addr) return false;

    s.patch_size = get_patch_length(s.addr, MIN_HOOK_SIZE);

    if (s.patch_size == 0 || s.patch_size > 64) return false;

    std::memcpy(orig_out, s.addr, s.patch_size);
    s.trampoline = create_trampoline_with_prolog(reinterpret_cast<uintptr_t>(s.addr), s.patch_size);

    if (!s.trampoline) return false;

    *s.jump_slot = reinterpret_cast<uintptr_t>(s.trampoline);
    install_hook(reinterpret_cast<uintptr_t>(s.addr), reinterpret_cast<void*>(s.stub), s.patch_size);

    return true;
}

// Patterns

static const uint8_t pat_krkr_1[] = {
    0x55, 0x8B, 0xEC, 0x81, 0xC4, 0x44, 0xFF, 0xFF, 0xFF, 0x53,
    0x56, 0x57, 0x89, 0x4D, 0xC0, 0x89, 0x55, 0xC4, 0x8B, 0xD8
};

static const uint8_t pat_krkr_2[] = {
    0x89, 0x7E, 0x70, 0x8B, 0x4E, 0x04, 0x8B, 0xC1, 0x99, 0x33,
    0xC2, 0x2B, 0xC2, 0x89, 0x46, 0x74, 0x83, 0xC4, 0x14, 0x5D,
    0x5F
};

static const uint8_t pat_krkrz[] = {
    0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x00, 0x00, 0x00, 0x00,
    0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xEC, 0x14,
    0x53, 0x56, 0x57, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC5,
    0x50, 0x8D, 0x45, 0xF4, 0x64, 0xA3, 0x00, 0x00, 0x00, 0x00,
    0x89, 0x65, 0xF0, 0x8B, 0xD9, 0x80, 0x3D, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x75, 0x17
};

static void init() {
    g_pipe = new IpcPipe(PIPE_NAME, false);

    g_module = get_main_module();

    if (g_module.base == 0) {
        g_pipe->send(MsgType::Status, "main module not found", StatusCode::Failure);
        return;
    }

    g_hooks[H_KRKR_1] = {  nullptr, 0, nullptr,
                           &g_jump_krkr_1, hook_krkr_1,
                           pat_krkr_1, sizeof(pat_krkr_1),
                           "xxxxxxxxxxxxxxxxxxxx"};

    g_hooks[H_KRKR_2] = {  nullptr, 0, nullptr,
                           &g_jump_krkr_2, hook_krkr_2,
                           pat_krkr_2, sizeof(pat_krkr_2),
                           "xxxxxxxxxxxxxxxxxxxxx"};

    g_hooks[H_KRKRZ]  = {  nullptr, 0, nullptr,
                           &g_jump_krkrz, hook_krkrz,
                           pat_krkrz, sizeof(pat_krkrz),
                           "xxxxxx????xx????xxxxxxxx????xxxxxxxx????xxxxxxx????xxx"};

    int installed = 0;
    for (int i = 0; i < kHookCount; ++i) {
        if (install_one(g_hooks[i], g_module.base, g_module.size, g_orig_bytes[i])
            && g_hooks[i].addr) {
            ++installed;
        }
    }

    if (installed == 0) {
        g_pipe->send(MsgType::Status, "KiriKiri: no hook patterns found", StatusCode::Failure);
        return;
    }

    g_pipe->send(MsgType::Status, "", StatusCode::Success);
}

static void cleanup() {
    for (int i = 0; i < kHookCount; ++i) {
        if (g_hooks[i].addr && g_hooks[i].patch_size > 0) {
            restore_hook(reinterpret_cast<uintptr_t>(g_hooks[i].addr),
                         g_orig_bytes[i], g_hooks[i].patch_size);
        }
    }
    g_pipe->unlink();
    g_pipe->close();
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
