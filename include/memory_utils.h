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

// Linux-only

#if defined(__linux__)
void* find_export(void* module_base, const char* target);
void set_protection(void* addr, size_t size, int prot, int* old_prot = nullptr);
#endif

// Disassembler / trampoline

#ifndef DISABLE_GET_PATCH_LENGTH
void* create_trampoline_with_prolog(uintptr_t target_func, size_t prolog_size);
size_t get_patch_length(void* target, size_t min_size);
#endif

// Hook install / restore

void install_hook(uintptr_t addr, void* handler, size_t size);
void restore_hook(uintptr_t addr, const uint8_t* orig, size_t size);

#endif // MEMORY_UTILS_H
