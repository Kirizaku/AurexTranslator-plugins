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
#include <cstring>

#if defined(__linux__)
#include <cstdio>

int perms_to_prot(const char* perms) {
    int prot = 0;
    if (perms[0] == 'r') prot |= PROT_READ;
    if (perms[1] == 'w') prot |= PROT_WRITE;
    if (perms[2] == 'x') prot |= PROT_EXEC;
    return prot;
}

int get_page_protection(void* addr) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return PROT_READ | PROT_EXEC;

    char line[256], perms[5];
    unsigned long start, end;
    uintptr_t target = reinterpret_cast<uintptr_t>(addr);
    int prot = PROT_READ | PROT_EXEC;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3) {
            if (start <= target && target < end) {
                prot = perms_to_prot(perms);
                break;
            }
        }
    }
    fclose(fp);
    return prot;
}

void set_protection(void* addr, size_t size, int prot, int* old_prot) {
    void* page = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(addr) & ~(PAGE_SIZE - 1));
    size_t total = (reinterpret_cast<uintptr_t>(addr) + size -
                    reinterpret_cast<uintptr_t>(page));

    if (old_prot) *old_prot = get_page_protection(addr);
    mprotect(page, total, prot);
}
#else
#include <windows.h>
#endif

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
    // mov rax, handler
    p[0] = 0x48;
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t *>(p + 2) =
        reinterpret_cast<uint64_t>(handler);

    // jmp
    p[10] = 0xFF;
    p[11] = 0xE0;

    for (size_t i = 12; i < size; ++i)
        p[i] = 0x90; // NOP
#else
    // jmp
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