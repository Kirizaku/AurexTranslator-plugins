/*
* Memory Hacking Library C++
* by Daniil Nabiulin
* version 1.0.4 (reduced)
* https://github.com/kirizaku/memory

* This is a reduced version of the original library.
* Full version and source code available at the link above.

Licensed under the MIT License <http://opensource.org/licenses/MIT>.

Copyright (c) 2022-2026 Daniil Nabiulin <https://github.com/kirizaku>

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

* Note: This file contains a reduced subset of the original library.
* Some features/components have been removed
*/

#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <Windows.h>
#include <TlHelp32.h>
#endif

namespace mem {

    #if defined(_WIN32)
    typedef DWORD mem_pid_t;
    #else
    typedef pid_t mem_pid_t;
    #endif

    #if defined(_UNICODE)
    typedef std::wstring string_t;
    #define mem_len std::wcslen
    #define mem_cmp std::wcscmp
    #else
    typedef std::string string_t;
    #define mem_len std::strlen
    #define mem_cmp std::strcmp
    #endif

    struct protection {
        #if defined(_WIN32)
        static const uintptr_t NONE = 0x1;
        static const uintptr_t EXECUTE = 0x10;
        static const uintptr_t READ_WRITE = 0x4;
        static const uintptr_t READ_EXECUTE = 0x20;
        static const uintptr_t READ_WRITE_EXECUTE = 0x40;
        #else
        static const uintptr_t NONE = 0x0;
        static const uintptr_t EXECUTE = 0x4;
        static const uintptr_t READ_WRITE = 0x1 | 0x2;
        static const uintptr_t READ_EXECUTE =  0x1 | 0x4;
        static const uintptr_t READ_WRITE_EXECUTE = 0x1 | 0x2 | 0x4;
        #endif
    };

    struct module_handle_t {
        void* handle = nullptr;
        uintptr_t base = 0;
    };

    #if defined(__linux__)

    struct inject_call_result_t {
        bool success = false;
        void* value = nullptr;
    };

    extern void* inject_syscall(mem_pid_t pid, int syscall_id, void* arg0, void* arg1, void* arg2, void* arg3, void* arg4, void* arg5);
    extern inject_call_result_t inject_call_function(mem_pid_t pid, void* code_addr, void* dlopen_addr, void* arg0, void* arg1);
    #endif
    extern uintptr_t get_module(mem_pid_t pid, string_t module_name);
    extern module_handle_t load_module(mem_pid_t pid, string_t path);
    extern bool unload_module(mem_pid_t pid, module_handle_t module);
    extern void* allocate(mem_pid_t pid, void* src, size_t size, uintptr_t protection);
    extern bool deallocate(mem_pid_t pid, void* src, size_t size);
    extern bool protect(mem_pid_t pid, void* src, size_t size, uintptr_t protection, uintptr_t *old_protection);
    extern bool write(mem_pid_t pid, void* src, void* dst, size_t size);
}

#endif	// _MEMORY_H_
