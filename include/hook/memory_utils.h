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

#ifndef MEMORY_UTILS_H
#define MEMORY_UTILS_H

#include <cstdint>
#include <cstddef>

#if defined(__linux__)
#include <sys/mman.h>
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

struct ModuleInfo {
    void*  base;
    void*  end;
    size_t size;
};

ModuleInfo get_main_module();
ModuleInfo get_module(const char* name);

// Pattern / signature scanning

void* find_pattern(const void* start, size_t size,
                   const uint8_t* pattern, const char* mask);

// Linux-only

#if defined(__linux__)
void* find_export(void* module_base, const char* target);
void set_protection(void* addr, size_t size, int prot, int* old_prot = nullptr);

extern bool g_is_wine;
bool detect_wine();
#endif

// Reading untrusted pointers
bool safe_read(const void* src, void* dst, size_t n);

#endif // MEMORY_UTILS_H
