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

template<typename R, typename... A>
inline R runtime_call(void* fn, A... a)
{
#if defined(__linux__) && defined(__x86_64__)
    if (g_is_wine)
        return reinterpret_cast<R (__attribute__((ms_abi)) *)(A...)>(fn)(a...);
#endif
    return reinterpret_cast<R (*)(A...)>(fn)(a...);
}

#endif // RUNTIME_MODULE_H
