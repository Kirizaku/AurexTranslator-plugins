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

#ifndef RUNTIME_MODULE_H
#define RUNTIME_MODULE_H

#include <string>

#include "hook/memory_utils.h"

inline bool game_uses_ms_abi()
{
#if defined(_WIN32)
    return true;
#elif defined(__linux__) && defined(__x86_64__)
    return g_is_wine;
#else
    return false;
#endif
}

void* find_loaded_module(bool (*match)(const std::string& lower_name), std::string* found_name = nullptr);
void* module_symbol(void* module, const char* name);

#if defined(__linux__) && defined(__x86_64__)

inline uintptr_t wine_call(void* fn, uintptr_t a1 = 0, uintptr_t a2 = 0,
                           uintptr_t a3 = 0, uintptr_t a4 = 0)
{
    register uintptr_t r8 asm("r8") = a3;
    register uintptr_t r9 asm("r9") = a4;
    register void*     fp asm("r11") = fn;

    // RSP has to be 16-byte aligned at the call
    uintptr_t ret;
    asm volatile("mov %%rsp, %%rbx\n\t"
                 "sub $128, %%rsp\n\t"
                 "and $-16, %%rsp\n\t"
                 "sub $32, %%rsp\n\t"
                 "call *%[fp]\n\t"
                 "mov %%rbx, %%rsp"
                 : "=a"(ret)
                 : [fp] "r"(fp), "c"(a1), "d"(a2), "r"(r8), "r"(r9)
                 : "rbx", "r10", "memory", "cc");
    return ret;
}

template<typename T>
inline uintptr_t wine_word(T v) { return (uintptr_t)v; }
#endif

template<typename R, typename... A>
inline R runtime_call(void* fn, A... a)
{
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine) {
        if constexpr (std::is_void_v<R>) {
            wine_call(fn, wine_word(a)...);
            return;
        } else {
            return (R)wine_call(fn, wine_word(a)...);
        }
    }
#endif
    return reinterpret_cast<R (*)(A...)>(fn)(a...);
}

#endif // RUNTIME_MODULE_H
