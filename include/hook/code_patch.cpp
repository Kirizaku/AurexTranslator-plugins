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

#include "hook/code_patch.h"
#include "hook/memory_utils.h"
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#  if defined(_MSC_VER)
#    include <intrin.h>
#  endif
#endif

static bool write_code_atomic(uintptr_t addr, const uint8_t* bytes, size_t n);

void install_hook(uintptr_t addr, void* handler, size_t size)
{
#ifdef _WIN32
    DWORD oldProt = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), size,
                   PAGE_EXECUTE_READWRITE, &oldProt);
#else
    int oldProt = 0;
    set_protection(reinterpret_cast<void*>(addr), size,
                   PROT_READ | PROT_WRITE | PROT_EXEC, &oldProt);
#endif

    uint8_t *p = reinterpret_cast<uint8_t*>(addr);

#if defined(__x86_64__) || defined(_M_X64)
    // MOV RAX, imm64 + JMP RAX - absolute
    p[0] = 0x48;
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t *>(p + 2) =
        reinterpret_cast<uint64_t>(handler);

    p[10] = 0xFF;
    p[11] = 0xE0;

    for (size_t i = 12; i < size; ++i)
        p[i] = 0x90; // NOP
#else
    // JMP rel32
    p[0] = 0xE9;
    uint32_t rel = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(handler) - addr - 5);
    *reinterpret_cast<uint32_t *>(p + 1) = rel;

    for (size_t i = 5; i < size; ++i)
        p[i] = 0x90; // NOP
#endif

#ifdef _WIN32
    VirtualProtect(reinterpret_cast<void*>(addr), size,
                   oldProt, &oldProt);
#else
    set_protection(reinterpret_cast<void*>(addr), size, oldProt);
#endif
}

bool install_hook_rel32(uintptr_t addr, uintptr_t dest)
{
    const intptr_t rel = static_cast<intptr_t>(dest) - static_cast<intptr_t>(addr + 5);
#if defined(__x86_64__) || defined(_M_X64)
    if (rel < INT32_MIN || rel > INT32_MAX)
        return false;
#endif

    uint8_t jmp[5];
    const int32_t r = static_cast<int32_t>(rel);
    jmp[0] = 0xE9;
    memcpy(jmp + 1, &r, 4);
    return write_code(addr, jmp, sizeof(jmp));
}

void restore_hook(uintptr_t addr, const uint8_t* orig, size_t size) {
#ifdef _WIN32
    DWORD old_prot;
    VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &old_prot);
#else
    int old_prot;
    set_protection(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE | PROT_EXEC, &old_prot);
#endif

    std::memcpy(reinterpret_cast<void*>(addr), orig, size);
#ifdef _WIN32
    VirtualProtect(reinterpret_cast<void*>(addr), size, old_prot, &old_prot);
#else
    set_protection(reinterpret_cast<void*>(addr), size, old_prot);
#endif
}

bool write_code(uintptr_t addr, const void* bytes, size_t n)
{
    const bool one_word  = n <= 8 && (addr & 7) + n <= 8;
    const bool three_step = n >= 3 && n <= 16 && (addr & 15) != 15;
    if (!one_word && !three_step)
        return false;

#ifdef _WIN32
    DWORD oldProt = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &oldProt);
#else
    int oldProt = 0;
    set_protection(reinterpret_cast<void*>(addr), n, PROT_READ | PROT_WRITE | PROT_EXEC, &oldProt);
#endif

    const uint8_t* src = static_cast<const uint8_t*>(bytes);
    uint8_t* dst = reinterpret_cast<uint8_t*>(addr);

    if (one_word) {
        write_code_atomic(addr, src, n);
    } else {
        static const uint8_t spin[2] = { 0xEB, 0xFE };
        write_code_atomic(addr, spin, 2);
        memcpy(dst + 2, src + 2, n - 2);
        write_code_atomic(addr, src, 2);
    }

#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    VirtualProtect(reinterpret_cast<void*>(addr), n, oldProt, &oldProt);
#else
    set_protection(reinterpret_cast<void*>(addr), n, oldProt);
#endif
    return true;
}

static bool write_code_atomic(uintptr_t addr, const uint8_t* bytes, size_t n)
{
    if (n == 0 || n > 8 || (addr & 63) + n > 64)
        return false;

    uintptr_t s = addr & ~static_cast<uintptr_t>(7);
    if (addr + n > s + 8) {
        const uintptr_t line = addr & ~static_cast<uintptr_t>(63);
        s = addr < line + 56 ? addr : line + 56;
    }

    uint64_t* w = reinterpret_cast<uint64_t*>(s);
    uint64_t old;
    memcpy(&old, w, sizeof(old));
    for (;;) {
        uint64_t nw = old;
        memcpy(reinterpret_cast<uint8_t*>(&nw) + (addr - s), bytes, n);
#if defined(_MSC_VER)
        const int64_t prev = _InterlockedCompareExchange64(reinterpret_cast<volatile int64_t*>(w),
                                                           static_cast<int64_t>(nw), static_cast<int64_t>(old));
        if (static_cast<uint64_t>(prev) == old) return true;
        old = static_cast<uint64_t>(prev);
#else
        if (__atomic_compare_exchange_n(w, &old, nw, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return true;
#endif
    }
}
