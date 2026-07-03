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

#include <atomic>

enum class State { WAITING_FIRST, WAITING_SECOND, WAITING_THIRD };

static std::atomic<State> g_state{State::WAITING_FIRST};
static std::atomic<uint32_t> g_saved_ecx{0};
static IpcPipe* g_pipe = nullptr;

std::string utf16le_to_utf8(const char16_t* utf16, size_t length) {
    std::string result;
    result.reserve(length * 3);
    for (size_t i = 0; i < length; ++i) {
        uint32_t code = static_cast<uint32_t>(utf16[i]);

        // High surrogate detected. Combine with following low surrogate (if present)
        // to form a full supplementary code point (U+10000..U+10FFFF).
        // It's unclear whether the game uses surrogate pairs, but handle them just in case.
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (i + 1 < length) {
                uint32_t low = static_cast<uint32_t>(utf16[i + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    ++i;

                    result.push_back(static_cast<char>(0xF0 | (code >> 18)));
                    result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    continue;
                }
            }
            result.push_back('?');
            continue;
        }

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
    }
    return result;
}

std::string parse_strings_from_memory(uint32_t addr) {
    std::string result;
    if (addr == 0) return result;

    uint8_t* mem = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(addr));
    uint8_t* sig_pos = nullptr;

    for (int i = 0; i < 64 - 4; i++) {
        if (mem[i] == 'I' && mem[i+1] == 'M' && mem[i+2] == 'H' && mem[i+3] == '2') {
            sig_pos = &mem[i];
            break;
        }
    }
    if (!sig_pos) return result;

    uint8_t* text_start = sig_pos + 4 + 16;
    uint32_t text_length = *reinterpret_cast<uint32_t*>(text_start);
    uint8_t* text_data = text_start + 4;
    const char16_t* utf16_str = reinterpret_cast<const char16_t*>(text_data);

    result = utf16le_to_utf8(utf16_str, text_length);

    return result;
}

extern "C" {
    void hook_call_first() { g_state = State::WAITING_SECOND; }

    void hook_call_second(uint32_t ecx) {
        g_saved_ecx = ecx;
        g_state = State::WAITING_THIRD;
    }

    void hook_call_third() {
        if (!g_pipe) return;
        std::string parse = parse_strings_from_memory(g_saved_ecx);
        g_pipe->send(MsgType::Text, parse);
        g_state = State::WAITING_FIRST;
    }
}

// HOOK ASM
#ifdef _MSC_VER  // MSVC
__declspec(naked) void hook_first(void)
{
    __asm {
        pushad
        pushfd

        call hook_call_first

        popfd
        popad

        // original instructions
        mov eax, 00407860h
        call eax
        mov eax, 004B2CAAh
        jmp eax
    }
}

__declspec(naked) void hook_second(void)
{
    __asm {
        pushad
        pushfd

        mov ebx, [esp + 1ch]
        push ebx

        call hook_call_second

        add esp, 4

        popfd
        popad

        // original instructions
        mov eax, 0052a011h
        call eax
        mov eax, 00407930h
        jmp eax
    }
}

__declspec(naked) void hook_third(void)
{
    __asm {
        pushad
        pushfd

        call hook_call_third

        popfd
        popad

        // original instructions
        mov ebx, [esi + 80h]
        mov eax, 004B2CB0h
        jmp eax
    }
}

#else // GCC || clang
__attribute__((naked)) void hook_first() {
    asm volatile (
        "pusha\n\t"
        "pushf\n\t"

        "call 1f\n\t"
        "1:\n\t"
        "popl %%eax\n\t"
        "addl $hook_call_first - 1b, %%eax\n\t"
        "call *%%eax\n\t"

        "popf\n\t"
        "popa\n\t"

        // original instructions
        "movl $0x00407860, %%eax\n\t"
        "call *%%eax\n\t"
        "movl $0x004b2caa, %%eax\n\t"
        "jmp *%%eax\n\t"
        ::: "memory");
}

__attribute__((naked)) void hook_second() {
    asm volatile (
        "pushal\n\t"
        "pushfl\n\t"

        "movl 0x1c(%%esp), %%ebx\n\t"
        "pushl %%ebx\n\t"
        "call 1f\n\t"
        "1:\n\t"
        "popl %%eax\n\t"
        "addl $hook_call_second - 1b, %%eax\n\t"
        "call *%%eax\n\t"
        "addl $4, %%esp\n\t"

        "popfl\n\t"
        "popal\n\t"

        // original instructions
        "movl $0x0052a011, %%eax\n\t"
        "call *%%eax\n\t"
        "movl $0x00407930, %%eax\n\t"
        "jmp *%%eax\n\t"
        ::: "memory");
}

__attribute__((naked)) void hook_third() {
    asm volatile (
        "pushal\n\t"
        "pushfl\n\t"

        "call 1f\n\t"
        "1:\n\t"
        "popl %%eax\n\t"
        "addl $hook_call_third - 1b, %%eax\n\t"
        "call *%%eax\n\t"

        "popfl\n\t"
        "popal\n\t"

        // original instructions
        "movl 0x80(%%esi), %%ebx\n\t"
        "movl $0x004B2CB0, %%eax\n\t"
        "jmp *%%eax\n\t"
        ::: "memory");
}
#endif

// HOOK Install
static uint8_t orig_first[FIRST_SIZE];
static uint8_t orig_second[SECOND_SIZE];
static uint8_t orig_third[THIRD_SIZE];

// Init
static void init() {
    g_pipe = new IpcPipe(PIPE_NAME, false);

    std::memcpy(orig_first,  reinterpret_cast<void*>(FIRST_ADDR),  FIRST_SIZE);
    std::memcpy(orig_second, reinterpret_cast<void*>(SECOND_ADDR), SECOND_SIZE);
    std::memcpy(orig_third,  reinterpret_cast<void*>(THIRD_ADDR),  THIRD_SIZE);

    install_hook(FIRST_ADDR,  reinterpret_cast<void*>(hook_first),  FIRST_SIZE);
    install_hook(SECOND_ADDR, reinterpret_cast<void*>(hook_second), SECOND_SIZE);
    install_hook(THIRD_ADDR,  reinterpret_cast<void*>(hook_third),  THIRD_SIZE);

    g_pipe->send(MsgType::Status, "", StatusCode::Success);
}

// Cleanup
static void cleanup() {
    restore_hook(FIRST_ADDR,  orig_first,  FIRST_SIZE);
    restore_hook(SECOND_ADDR, orig_second, SECOND_SIZE);
    restore_hook(THIRD_ADDR,  orig_third,  THIRD_SIZE);

    g_pipe->close();
#if defined(__linux__)
    g_pipe->unlink();
#endif
    delete g_pipe;
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