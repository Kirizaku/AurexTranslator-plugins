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

#ifndef HOOK_ENGINE_H
#define HOOK_ENGINE_H

#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
struct HookRegs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t rflags;
    uint64_t ret;
};
#else
struct HookRegs {
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    uint32_t eflags;
    uint32_t ret;
};
#endif

inline const uintptr_t* hook_stack_args(const HookRegs* r)
{
    return reinterpret_cast<const uintptr_t*>(&r->ret) + 1;
}

#if defined(__x86_64__) || defined(_M_X64)

inline uintptr_t hook_arg(const HookRegs* r, unsigned i, bool ms_abi)
{
    if (ms_abi) {
        switch (i) {
        case 0: return static_cast<uintptr_t>(r->rcx);
        case 1: return static_cast<uintptr_t>(r->rdx);
        case 2: return static_cast<uintptr_t>(r->r8);
        case 3: return static_cast<uintptr_t>(r->r9);
        default: return hook_stack_args(r)[i]; // 4 slots of shadow space come first
        }
    }
    switch (i) {
    case 0: return static_cast<uintptr_t>(r->rdi);
    case 1: return static_cast<uintptr_t>(r->rsi);
    case 2: return static_cast<uintptr_t>(r->rdx);
    case 3: return static_cast<uintptr_t>(r->rcx);
    case 4: return static_cast<uintptr_t>(r->r8);
    case 5: return static_cast<uintptr_t>(r->r9);
    default: return hook_stack_args(r)[i - 6];
    }
}
#endif

using HookHandler = void (*)(const HookRegs* regs, uint32_t user_id);

enum class HookResult {
    Ok,
    AlreadyHooked,
    NotInitialized,
    TableFull,
    PrologUnsafe, // the first bytes cannot be relocated (or the function is too short to patch)
    NoNearMemory, // no executable memory within rel32 reach of the target
    PatchFailed,
};

bool hook_engine_init(HookHandler handler);
HookResult hook_engine_install(void* target, uint32_t user_id);
bool hook_engine_shutdown(unsigned timeout_ms = 500);

#endif // HOOK_ENGINE_H
