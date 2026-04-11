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

#include "memory.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>

#if defined(__linux__)
#include <dirent.h>
#include <dlfcn.h>

#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#endif

#define LOG_ERROR(fmt, ...) do { \
std::cerr << "[ERROR] "; \
    fprintf(stderr, fmt, ##__VA_ARGS__); \
    std::cerr << " (errno: " << errno << ")" << std::endl; \
} while(0)

#if defined(__linux__)
    static bool ptrace_attach(mem::mem_pid_t pid)
    {
        if (ptrace(PTRACE_ATTACH, pid, 0, 0) == -1) {
            LOG_ERROR("Failed to attach to process");
            return false;
        }

        int status;
        wait(&status);

        if (!WIFSTOPPED(status)) {
            LOG_ERROR("Process not stopped after attach");
            ptrace(PTRACE_DETACH, pid, 0, 0);
            return false;
        }

        return true;
    }

static bool ptrace_get_regs(mem::mem_pid_t pid, struct user_regs_struct* regs)
{
    if (ptrace(PTRACE_GETREGS, pid, 0, regs) == -1) {
        LOG_ERROR("Failed to get registers");
        return false;
    }
    return true;
}

static bool ptrace_set_regs(mem::mem_pid_t pid, struct user_regs_struct* regs)
{
    if (ptrace(PTRACE_SETREGS, pid, 0, regs) == -1) {
        LOG_ERROR("Failed to set registers");
        return false;
    }
    return true;
}

static uintptr_t ptrace_peek_text(mem::mem_pid_t pid, void* addr)
{
    uintptr_t value = ptrace(PTRACE_PEEKTEXT, pid, addr, 0);
    if (value == -1) {
        LOG_ERROR("Failed to peek text");
    }
    return value;
}

static bool ptrace_poke_text(mem::mem_pid_t pid, void* addr, uintptr_t value)
{
    if (ptrace(PTRACE_POKETEXT, pid, addr, (void*)value) == -1) {
        LOG_ERROR("Failed to poke text");
        return false;
    }
    return true;
}

static bool ptrace_single_step(mem::mem_pid_t pid)
{
    if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0) == -1) {
        LOG_ERROR("Failed to single step");
        return false;
    }

    int status;
    waitpid(pid, &status, WSTOPPED);

    if (!WIFSTOPPED(status)) {
        LOG_ERROR("Process not stopped after single step");
        return false;
    }

    return true;
}

static bool ptrace_continue(mem::mem_pid_t pid)
{
    if (ptrace(PTRACE_CONT, pid, 0, 0) == -1) {
        LOG_ERROR("Failed to continue process");
        return false;
    }

    int status;
    waitpid(pid, &status, WSTOPPED);

    if (!WIFSTOPPED(status)) {
        LOG_ERROR("Process not stopped after execution");
        return false;
    }

    return true;
}

static void ptrace_detach(mem::mem_pid_t pid)
{
    if (ptrace(PTRACE_DETACH, pid, 0, 0) == -1) {
        LOG_ERROR("Failed to detach from process");
    }
}

void* mem::inject_syscall(mem_pid_t pid, int syscall_id, void* arg0, void* arg1, void* arg2, void* arg3, void* arg4, void* arg5)
{
    void* result = nullptr;

    if (!ptrace_attach(pid)) return result;

    struct user_regs_struct regs, regs_backup;
    if (!ptrace_get_regs(pid, &regs_backup)) {
        ptrace_detach(pid);
        return result;
    }

    void* code_addr;
    regs = regs_backup;

#if defined(__x86_64__)
    regs.rax = (uintptr_t)syscall_id;
    regs.rdi = (uintptr_t)arg0;
    regs.rsi = (uintptr_t)arg1;
    regs.rdx = (uintptr_t)arg2;
    regs.r10 = (uintptr_t)arg3;
    regs.r8  = (uintptr_t)arg4;
    regs.r9  = (uintptr_t)arg5;

    code_addr = (void*)regs.rip;
#else
    regs.eax = (uintptr_t)syscall_id;
    regs.ebx = (uintptr_t)arg0;
    regs.ecx = (uintptr_t)arg1;
    regs.edx = (uintptr_t)arg2;
    regs.esi = (uintptr_t)arg3;
    regs.edi = (uintptr_t)arg4;
    regs.ebp = (uintptr_t)arg5;

    code_addr = (void*)regs.eip;
#endif

    const uint8_t syscall_buffer[] = {
#if defined(__x86_64__)
        0x0F,0x05
#else
        0xcd,0x80
#endif
    }; //Fast System Call

    uintptr_t injection_syscall = 0;
    std::memcpy(&injection_syscall, syscall_buffer, 2);

    uintptr_t original_code = ptrace_peek_text(pid, code_addr);
    if (original_code == -1) {
        ptrace_detach(pid);
        return result;
    }

    if (!ptrace_poke_text(pid, code_addr, injection_syscall)) {
        ptrace_detach(pid);
        return result;
    }

    if (!ptrace_set_regs(pid, &regs)) {
        ptrace_poke_text(pid, code_addr, original_code);
        ptrace_detach(pid);
        return result;
    }

    if (!ptrace_single_step(pid)) {
        ptrace_poke_text(pid, code_addr, original_code);
        ptrace_detach(pid);
        return result;
    }

    ptrace_get_regs(pid, &regs);

#if defined(__x86_64__)
    result = (void*)regs.rax;
#else
    result = (void*)regs.eax;
#endif

    ptrace_poke_text(pid, code_addr, original_code);
    ptrace_set_regs(pid, &regs_backup);
    ptrace_detach(pid);

    return result;
}

mem::inject_call_result_t mem::inject_call_function(mem_pid_t pid, void* code_addr, void* dlopen_addr, void* arg0, void* arg1)
{
    inject_call_result_t result = {false, nullptr};

    if (!ptrace_attach(pid)) return result;

    struct user_regs_struct regs, regs_backup;
    if (!ptrace_get_regs(pid, &regs_backup)) {
        ptrace_detach(pid);
        return result;
    }

    regs = regs_backup;

#if defined(__x86_64__)
    regs.rip = (uintptr_t)code_addr;
    regs.rsp -= (regs.rsp) % 16;
    regs.rbp = regs.rsp - 8;
    regs.rax = (uintptr_t)dlopen_addr;
    regs.rsi = (uintptr_t)arg1;
    regs.rdi = (uintptr_t)arg0;
#else
    regs.esp -= (regs.esp % 16);
    regs.esp -= 4;
    regs.eip = (uintptr_t)code_addr;
    regs.eax = (uintptr_t)dlopen_addr;
    regs.esp -= 4;
    ptrace(PTRACE_POKEDATA, pid, (void*)regs.esp, (void*)arg1);
    regs.esp -= 4;
    ptrace(PTRACE_POKEDATA, pid, (void*)regs.esp, arg0);
#endif

    const uint8_t call_function_buffer[] = {
        0xff, 0xd0, 0xcc
    }; // Call Function

    uintptr_t original_code = ptrace_peek_text(pid, code_addr);
    if (original_code == -1) {
        ptrace_detach(pid);
        return result;
    }

    if (!ptrace_set_regs(pid, &regs)) {
        ptrace_poke_text(pid, code_addr, original_code);
        ptrace_detach(pid);
        return result;
    }

    uintptr_t injection_callfunc = 0;
    std::memcpy(&injection_callfunc, call_function_buffer, 3);
    if (!ptrace_poke_text(pid, code_addr, injection_callfunc)) {
        ptrace_detach(pid);
        return result;
    }

    if (!ptrace_continue(pid)) {
        ptrace_poke_text(pid, code_addr, original_code);
        ptrace_detach(pid);
        return result;
    }

    ptrace_get_regs(pid, &regs);
    ptrace_set_regs(pid, &regs_backup);

    result.success = true;
#if defined(__x86_64__)
    result.value = (void*)regs.rax;
#else
    result.value = (void*)regs.eax;
#endif

    ptrace_poke_text(pid, code_addr, original_code);
    ptrace_detach(pid);

    return result;
}

static uintptr_t get_function_offset(const char* func_name)
{
    void* handle = dlopen("libc.so.6", RTLD_LAZY);
    if (!handle) {
        LOG_ERROR("Failed to open libc");
        return 0;
    }

    dlerror();

    void* dlopen_addr = dlsym(handle, func_name);
    const char* error = dlerror();

    if (error) {
        LOG_ERROR("Failed to find %s", func_name);
        dlclose(handle);
        return 0;
    }

    Dl_info info;
    if (dladdr(dlopen_addr, &info)) {
        uintptr_t base_addr = (uintptr_t)info.dli_fbase;
        uintptr_t offset = (uintptr_t)dlopen_addr - base_addr;

        dlclose(handle);
        return offset;
    }

    dlclose(handle);
    return 0;
}
#endif

uintptr_t mem::get_module(mem_pid_t pid, string_t module_name)
{
    uintptr_t module_base = 0;
#if defined(_WIN32)
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 module_entry;
        module_entry.dwSize = sizeof(module_entry);
        if (Module32First(hSnap, &module_entry)) {
            do {
                if (!mem_cmp(module_entry.szModule, module_name.c_str()) || !mem_cmp(module_entry.szExePath, module_name.c_str())) {
                    module_base = (uintptr_t)module_entry.modBaseAddr;
                    break;
                }
            } while (Module32Next(hSnap, &module_entry));
        }
    }
    CloseHandle(hSnap);
#else
    std::stringstream maps_file;
    maps_file << "/proc/" << pid << "/maps";
    std::ifstream maps_ifst(maps_file.str());

    if(!maps_ifst.is_open()) return module_base;

    maps_file.str(std::string());
    maps_file << maps_ifst.rdbuf();

    size_t module_base_path = maps_file.str().find(module_name);
    size_t module_base_start = maps_file.str().rfind('\n', module_base_path);
    if (module_base_start == maps_file.str().npos) { module_base_start = 0; }

    size_t module_base_end = maps_file.str().find('-', module_base_path);
    if(module_base_end == maps_file.str().npos) return module_base;

    module_base = std::stoull(maps_file.str().substr(module_base_start, module_base_end - module_base_start), nullptr, 16);
    maps_ifst.close();
#endif
    return module_base;
}

mem::module_handle_t mem::load_module(mem_pid_t pid, string_t path)
{
    module_handle_t result;
#if defined(_WIN32)
    size_t path_size = mem_len(path.c_str()) + 1;
    void* path_address = allocate(pid, 0, path_size, PAGE_EXECUTE_READWRITE);
    if (!path_address) return result;
    write(pid, path_address, (LPCVOID*)path.c_str(), path_size);

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) {
        deallocate(pid, path_address, path_size);
        return result;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, 0, 0, (LPTHREAD_START_ROUTINE)LoadLibraryA, path_address, 0, 0);
    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        DWORD remote_mod_handle = 0;
        GetExitCodeThread(hThread, &remote_mod_handle);
        result.handle = (void*)(uintptr_t)remote_mod_handle;
        CloseHandle(hThread);
    }

    CloseHandle(hProcess);
    deallocate(pid, path_address, path_size);
#else
    uintptr_t glibc_module = mem::get_module(pid, "libc.so.6");
    uintptr_t dlopen_offset = get_function_offset("dlopen");
    uintptr_t dlopen_addr = glibc_module + dlopen_offset;

    size_t path_size = mem_len(path.c_str()) + 1;
    size_t text_size = sysconf(_SC_PAGESIZE);
    size_t stack_size = 2 * 1024 * 1024;

    void* path_address = allocate(pid, 0, text_size + stack_size, protection::READ_WRITE_EXECUTE);
    if (!path_address) return result;
    mem::write(pid, path_address, (void*)path.c_str(), path_size);

    void* code_addr = (void*)((uintptr_t)path_address + path_size);
    inject_call_result_t res = inject_call_function(pid, (void*)code_addr, (void*)dlopen_addr, path_address, (void*)RTLD_LAZY);
    result.handle = res.value;
    mem::deallocate(pid, path_address, text_size + stack_size);
#endif
    result.base = get_module(pid, path);
    return result;
}

bool mem::unload_module(mem_pid_t pid, module_handle_t module)
{
    bool result = 0;
#if defined(_WIN32)
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return result;

    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)FreeLibrary, (void*)module.base, 0, NULL);
    if (!hThread) {
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    result = (exitCode != 0);

    CloseHandle(hThread);
    CloseHandle(hProcess);
#else
    uintptr_t glibc_module = mem::get_module(pid, "libc.so.6");
    uintptr_t dlclose_offset = get_function_offset("dlclose");
    uintptr_t dlclose_addr = glibc_module + dlclose_offset;

    size_t path_size = sysconf(_SC_PAGESIZE);
    void* path_address = allocate(pid, 0, path_size, protection::READ_WRITE_EXECUTE);
    if (!path_address) return result;

    inject_call_result_t res = inject_call_function(pid, (void*)path_address, (void*)dlclose_addr, module.handle, nullptr);
    result = (res.success && res.value == 0) ? true : false;
    mem::deallocate(pid, path_address, path_size);
#endif
    return result;
}

void* mem::allocate(mem_pid_t pid, void* src, size_t size, uintptr_t protection)
{
    void* result = nullptr;
#if defined(_WIN32)
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return result;
    result = VirtualAllocEx(hProcess, 0, size, MEM_COMMIT | MEM_RESERVE, protection);
    if (!result) result = FALSE;
    CloseHandle(hProcess);
#else
    int syscall_id = 0;
#if defined(__x86_64__)
    syscall_id = __NR_mmap;
#else
    syscall_id = __NR_mmap2;
#endif
    result = inject_syscall(pid, syscall_id, 0, (void*)size, (void*)protection, (void*)(MAP_PRIVATE | MAP_ANON), 0, 0);
    if (result == ((void*)(uintptr_t)syscall_id) || result == MAP_FAILED) result = 0;
#endif
    return result;
}

bool mem::deallocate(mem_pid_t pid, void* src, size_t size)
{
    bool result = 0;
#if defined(_WIN32)
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return result;
    result = VirtualFreeEx(hProcess, src, 0, MEM_RELEASE) != 0 ? true : false;
    CloseHandle(hProcess);
#else
    result = inject_syscall(pid, __NR_munmap, src, (void*)size, 0, 0, 0, 0) == 0 ? true : false;
#endif
    return result;
}

bool mem::protect(mem_pid_t pid, void* src, size_t size, uintptr_t protection, uintptr_t *old_protection)
{
    bool result = 0;
#if defined(_WIN32)
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    DWORD old_protect = 0;
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return result;
    result = VirtualProtectEx(hProcess, src, size, protection, &old_protect) != 0 ? true : false;
    if (old_protection) *old_protection = old_protect;
    CloseHandle(hProcess);
#else
    if (old_protection) {
        std::stringstream maps_file;
        maps_file << "/proc/" << pid << "/maps";
        std::ifstream maps_ifst(maps_file.str());

        if(!maps_ifst.is_open()) return result;

        maps_file.str(std::string());
        maps_file << maps_ifst.rdbuf();

        std::stringstream strm;
        strm << src;
        std::string addr = strm.str();
        addr.erase(addr.find("0x"), 2);
        addr = addr + "-";

        size_t addr_protection_path = maps_file.str().find(addr);
        if (addr_protection_path == maps_file.str().npos) { *old_protection = 0; return result; }

        size_t addr_protection_start = maps_file.str().find(' ', addr_protection_path);
        if (addr_protection_start == maps_file.str().npos) { *old_protection = 0; return result; }

        size_t end = addr_protection_start + 4;

        intptr_t prot = 0;
        for(size_t i = addr_protection_start; i < end; i++)
        {
            char c = maps_file.str()[i];
            switch(c) {
            case 'r': 	prot |= PROT_READ;		break;
            case 'w':	prot |= PROT_WRITE;		break;
            case 'x':	prot |= PROT_EXEC; 		break;
            }
        }
        *old_protection = prot;
        maps_ifst.close();
    }
    result = inject_syscall(pid, __NR_mprotect, src, (void*)size, (void*)protection, 0, 0, 0) == 0 ? true : false;
#endif
    return result;
}

bool mem::write(mem_pid_t pid, void* src, void* dst, size_t size)
{
    bool result = 0;
#if defined(_WIN32)
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess || hProcess == INVALID_HANDLE_VALUE) return result;
    result = WriteProcessMemory(hProcess, src, (LPCVOID)dst, size, 0) != 0 ? true : false;
    CloseHandle(hProcess);
#else
    struct iovec iosrc;
    struct iovec iodst;
    iosrc.iov_base = src;
    iosrc.iov_len = size;
    iodst.iov_base = dst;
    iodst.iov_len = size;
    result = (size_t)process_vm_writev(pid, &iodst, 1, &iosrc, 1, 0) == size ? true : false;
#endif
    return result;
}