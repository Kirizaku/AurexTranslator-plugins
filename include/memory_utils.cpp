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

template<typename T>
static inline T pe_read(const void* addr)
{
    T v;
    memcpy(&v, addr, sizeof(T));
    return v;
}

// ===============================================================
// Main module lookup
// ===============================================================
//

#if defined(_WIN32)
#  include <windows.h>
#  include <tlhelp32.h>
#else
#  include <cstdio>
#  include <unistd.h>
#endif

#if !defined(_WIN32)
static bool ends_with_exe_(const char* s)
{
    size_t n = strlen(s);
    if (n < 4) return false;
    const char* t = s + n - 4;
    return t[0] == '.' &&
           (t[1] == 'e' || t[1] == 'E') &&
           (t[2] == 'x' || t[2] == 'X') &&
           (t[3] == 'e' || t[3] == 'E');
}
#endif

static const char* basename_ptr_(const char* path)
{
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

ModuleInfo get_main_module()
{
    ModuleInfo info = { nullptr, nullptr, 0 };

#if defined(_WIN32)
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return info;

    return get_module(basename_ptr_(path));

#else
    // Wine PE first: find the first ".exe" mapping in /proc/self/maps
    FILE* fp = fopen("/proc/self/maps", "r");
    if (fp) {
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            unsigned long a = 0, b = 0;
            char perms[8] = {0};
            unsigned long off = 0;
            char dev[16] = {0};
            unsigned long inode = 0;
            int path_pos = 0;

            if (sscanf(line, "%lx-%lx %7s %lx %15s %lu %n", &a, &b, perms, &off, dev, &inode, &path_pos) < 6)
                continue;

            char* path = line + path_pos;
            char* nl = strchr(path, '\n');
            if (nl) *nl = 0;
            while (*path == ' ' || *path == '\t') ++path;
            if (*path == 0 || *path == '[')
                continue;

            if (ends_with_exe_(path)) {
                ModuleInfo m = get_module(basename_ptr_(path));
                fclose(fp);
                return m;
            }
        }
        fclose(fp);
    }

    // Native Linux fallback: ELF main via /proc/self/exe symlink
    char buf[1024];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        return get_module(basename_ptr_(buf));
    }
    return info;
#endif
}

// ===============================================================
// Module lookup
// ===============================================================
//

#if !defined(_WIN32)
static inline char ascii_lower_(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

static bool basename_iequals_(const char* path, const char* target)
{
    const char* base = basename_ptr_(path);
    while (*base && *target) {
        if (ascii_lower_(*base) != ascii_lower_(*target))
            return false;
        ++base; ++target;
    }
    return *base == 0 && *target == 0;
}
#endif

ModuleInfo get_module(const char* name)
{
    ModuleInfo info = { nullptr, nullptr, 0 };
    if (!name || !*name)
        return info;

#if defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return info;

    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, name) == 0) {
                info.base = me.modBaseAddr;
                info.size = me.modBaseSize;
                info.end  = static_cast<uint8_t*>(me.modBaseAddr) + me.modBaseSize;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);

#else
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp)
        return info;

    uintptr_t lo = 0, hi = 0;
    bool found = false;
    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        unsigned long start = 0, end = 0;
        char perms[8] = {0};
        unsigned long off = 0;
        char dev[16] = {0};
        unsigned long inode = 0;
        int path_pos = 0;

        if (sscanf(line, "%lx-%lx %7s %lx %15s %lu %n", &start, &end, perms, &off, dev, &inode, &path_pos) < 6)
            continue;

        char* path = line + path_pos;
        char* nl = strchr(path, '\n');
        if (nl) *nl = 0;
        while (*path == ' ' || *path == '\t') ++path;
        if (*path == 0 || *path == '[')
            continue;

        if (basename_iequals_(path, name)) {
            if (!found) {
                lo = start;
                hi = end;
                found = true;
            } else {
                if (start < lo) lo = start;
                if (end   > hi) hi = end;
            }
        }
    }
    fclose(fp);

    if (!found)
        return info;

    info.base = reinterpret_cast<void*>(lo);
    info.size = static_cast<size_t>(hi - lo);
    info.end  = reinterpret_cast<void*>(hi);

    // If this is a PE module (Wine), the file-backed vmas only cover the
    // header and a few resource sections; the actual .text/.data live in
    // anonymous mappings between them. Override size with SizeOfImage from
    // the PE header — that's the authoritative module size
    const uint8_t* b = reinterpret_cast<const uint8_t*>(lo);
    if (hi - lo >= 0x40 && pe_read<uint16_t>(b) == 0x5A4D) {
        const uint32_t e_lfanew = pe_read<uint32_t>(b + 0x3C);
        // Need to reach OptionalHeader+0x38 (SizeOfImage, 4 bytes):
        // e_lfanew + PE sig (4) + FileHeader (20) + 0x38 + 4
        if (e_lfanew && e_lfanew <= 0x1000000u &&
            static_cast<uintptr_t>(e_lfanew) + 24 + 0x38 + 4 <= (hi - lo)) {
            const uint8_t* nt = b + e_lfanew;
            if (pe_read<uint32_t>(nt) == 0x00004550u) {
                // SizeOfImage at offset 0x38 in OptionalHeader (PE32/PE32+)
                // OptionalHeader starts 24 bytes after the PE signature
                const uint32_t size_of_image = pe_read<uint32_t>(nt + 24 + 0x38);
                if (size_of_image > info.size) {
                    info.size = size_of_image;
                    info.end  = reinterpret_cast<void*>(lo + size_of_image);
                }
            }
        }
    }
#endif
    return info;
}

static inline bool pattern_match(const uint8_t* data, const uint8_t* pattern, const char* mask)
{
    for (; *mask; ++mask, ++data, ++pattern) {
        if (*mask == 'x' && *data != *pattern)
            return false;
    }
    return true;
}

void* find_pattern(const void* start, size_t size, const uint8_t* pattern, const char* mask)
{
    if (!start || !pattern || !mask)
        return nullptr;

    const size_t pat_len = strlen(mask);
    if (pat_len == 0 || pat_len > size)
        return nullptr;

    const uint8_t* base = static_cast<const uint8_t*>(start);
    const size_t   last = size - pat_len;

    for (size_t i = 0; i <= last; ++i) {
        if (pattern_match(base + i, pattern, mask))
            return const_cast<uint8_t*>(base + i);
    }
    return nullptr;
}

#if defined(__linux__)
#include <cstdio>
#include <cstdint>

// PE export resolver
// Walks the PE export directory of a module already mapped into memory
// (e.g. a Wine DLL loaded in a Linux process) and returns the virtual address

void* find_export(void* module_base, const char* target)
{
    if (!module_base || !target)
        return nullptr;

    const uint8_t* base = static_cast<const uint8_t*>(module_base);

    // Validate DOS header magic
    if (pe_read<uint16_t>(base) != 0x5A4D)
        return nullptr;

    // e_lfanew — offset from the start of the file to the PE header
    const uint32_t e_lfanew = pe_read<uint32_t>(base + 0x3C);
    if (e_lfanew == 0 || e_lfanew > 0x1000000u)
        return nullptr;

    // Validate PE signature
    const uint8_t* nt = base + e_lfanew;
    if (pe_read<uint32_t>(nt) != 0x00004550u)
        return nullptr;

    const uint8_t* opt = nt + 24;
    const uint16_t magic = pe_read<uint16_t>(opt);

    // Read the export directory RVA and size from the data directory
    // Offsets differ between PE32 (0x10B) and PE32+ (0x20B)
    uint32_t export_rva = 0, export_size = 0;
    if (magic == 0x020Bu) {          // PE32+
        export_rva = pe_read<uint32_t>(opt + 112);
        export_size = pe_read<uint32_t>(opt + 116);
    }
    else if (magic == 0x010Bu) {   // PE32
        export_rva = pe_read<uint32_t>(opt + 96);
        export_size = pe_read<uint32_t>(opt + 100);
    }
    else {
        return nullptr;
    }

    // 40 == sizeof(IMAGE_EXPORT_DIRECTORY)
    if (export_rva == 0 || export_size < 40u)
        return nullptr;

    const uint8_t* exp = base + export_rva;

    // IMAGE_EXPORT_DIRECTORY fields (by offset):
    //   +0x14  NumberOfFunctions
    //   +0x18  NumberOfNames
    //   +0x1C  AddressOfFunctions      - RVA of the EAT (export address table)
    //   +0x20  AddressOfNames          - RVA of the name pointer table
    //   +0x24  AddressOfNameOrdinals   - RVA of the ordinal table
    const uint32_t num_functions = pe_read<uint32_t>(exp + 0x14);
    const uint32_t num_names = pe_read<uint32_t>(exp + 0x18);
    if (num_names == 0 || num_functions == 0)
        return nullptr;

    const uint32_t addr_funcs = pe_read<uint32_t>(exp + 0x1C);
    const uint32_t addr_names = pe_read<uint32_t>(exp + 0x20);
    const uint32_t addr_ords = pe_read<uint32_t>(exp + 0x24);
    if (addr_funcs == 0 || addr_names == 0 || addr_ords == 0)
        return nullptr;

    const uint32_t* names = reinterpret_cast<const uint32_t*>(base + addr_names);
    const uint16_t* ordinals = reinterpret_cast<const uint16_t*>(base + addr_ords);
    const uint32_t* functions = reinterpret_cast<const uint32_t*>(base + addr_funcs);

    const size_t target_len = strlen(target);

    for (uint32_t i = 0; i < num_names; ++i) {
        const uint32_t name_rva = pe_read<uint32_t>(names + i);
        if (name_rva == 0)
            continue;

        const char* name = reinterpret_cast<const char*>(base + name_rva);
        if (strncmp(name, target, target_len + 1) != 0)
            continue;

        const uint16_t ord = pe_read<uint16_t>(ordinals + i);
        if (ord >= num_functions)
            continue;

        const uint32_t func_rva = pe_read<uint32_t>(functions + ord);
        if (func_rva == 0)
            continue;

        // Forwarded export - skip
        if (func_rva >= export_rva && func_rva < export_rva + export_size)
            return nullptr;

        return const_cast<uint8_t*>(base) + func_rva;
    }

    return nullptr;
}
// ===============================================================
// Page-protection helpers
// ===============================================================

// Convert a /proc/self/maps permission string ("rwxp") to mprotect flags
static int perms_to_prot(const char* perms)
{
    int prot = PROT_NONE;
    if (perms[0] == 'r') prot |= PROT_READ;
    if (perms[1] == 'w') prot |= PROT_WRITE;
    if (perms[2] == 'x') prot |= PROT_EXEC;
    return prot;
}


// Return the current mprotect flags for the page containing addr
static int get_page_protection(void* addr)
{
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp)
        return PROT_READ | PROT_EXEC;

    const uintptr_t target = reinterpret_cast<uintptr_t>(addr);
    int prot = PROT_READ | PROT_EXEC;   // safe fallback

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long start = 0, end = 0;
        char perms[5] = {};
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

// Change protection of the page(s) covering [addr, addr+size)
void set_protection(void* addr, size_t size, int prot, int* old_prot)
{
    const uintptr_t page_addr = reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(PAGE_SIZE - 1);
    const size_t    total     = reinterpret_cast<uintptr_t>(addr) + size - page_addr;

    if (old_prot)
        *old_prot = get_page_protection(addr);

    mprotect(reinterpret_cast<void*>(page_addr), total, prot);
}

#else   // _WIN32
#include <windows.h>
#endif  // __linux__

// ===============================================================
// Disassembler / trampoline
// ===============================================================

#ifndef DISABLE_GET_PATCH_LENGTH

#if defined(__x86_64__) || defined(_M_X64)
#include "hde/hde64.h"
using hde_t = hde64s;
#define hde_disasm hde64_disasm
#else
#include "hde/hde32.h"
using hde_t = hde32s;
#define hde_disasm hde32_disasm
#endif

// Patch rel32 / RIP-relative displacement in a copied instruction sequence
static void fix_relative_instructions(uint8_t* tramp,
                                      uintptr_t orig_addr,
                                      size_t    prolog_size)
{
    size_t offset = 0;
    while (offset < prolog_size) {
#if defined(__x86_64__) || defined(_M_X64)
        hde64s hs;
        const size_t insn_len = hde64_disasm(tramp + offset, &hs);
        if (hs.flags & F_ERROR) return;
        uint8_t* p = tramp + offset;
        const uint8_t op = hs.opcode;

        // Recalculate a rel32 field so that the same absolute target is reached
        auto patch_rel32 = [&](size_t field_offset) {
            int32_t  old_rel = pe_read<int32_t>(p + field_offset);
            intptr_t abs_target = static_cast<intptr_t>(orig_addr + offset + insn_len) + old_rel;
            intptr_t new_rel = abs_target - static_cast<intptr_t>(reinterpret_cast<uintptr_t>(p) + insn_len);
            if (new_rel < INT32_MIN || new_rel > INT32_MAX) {
                return;
            }
            memcpy(p + field_offset, &new_rel, 4);
        };

        // call rel32 / jmp rel32 - disp32
        if ((op == 0xE8 || op == 0xE9) && (hs.flags & F_IMM32))
            patch_rel32(insn_len - 4);

        // RIP-relative - disp32
        if ((hs.flags & F_DISP32) && hs.modrm_mod == 0 && hs.modrm_rm == 5) {
            size_t imm_size = 0;
            if (hs.flags & F_IMM32)      imm_size = 4;
            else if (hs.flags & F_IMM16) imm_size = 2;
            else if (hs.flags & F_IMM8)  imm_size = 1;
            patch_rel32(insn_len - imm_size - 4);
        }

        offset += insn_len;
#else   // x86-32
        hde32s hs;
        uint8_t* p = tramp + offset;
        const size_t insn_len = hde32_disasm(p, &hs);
        if (hs.flags & F_ERROR)
            return;

        const uint8_t op = hs.opcode;

        // call rel32 (E8) / jmp rel32 (E9).
        if ((op == 0xE8 || op == 0xE9) && (hs.flags & F_IMM32)) {
            int32_t  old_rel = pe_read<int32_t>(p + insn_len - 4);
            intptr_t abs_target = static_cast<intptr_t>(orig_addr + offset + insn_len) + old_rel;
            int32_t  new_rel = static_cast<int32_t>(abs_target - static_cast<intptr_t>(reinterpret_cast<uintptr_t>(p) + insn_len));
            memcpy(p + insn_len - 4, &new_rel, 4);
        }

        // ADD EAX, imm32 (0x05) — get_pc_thunk / GOT setup in PIC code
        if (op == 0x05) {
            uint32_t old_imm = pe_read<uint32_t>(p + 1);
            uintptr_t got_base = (orig_addr + offset + insn_len) + old_imm;
            uint32_t  new_imm = static_cast<uint32_t>(got_base - (reinterpret_cast<uintptr_t>(p) + insn_len));
            memcpy(p + 1, &new_imm, 4);
        }

        offset += insn_len;
#endif
    }
}

void* create_trampoline_with_prolog(uintptr_t target_func, size_t prolog_size)
{
    if (!target_func || prolog_size == 0)
        return nullptr;

    // prolog copy + absolute jump back
#if defined(__x86_64__) || defined(_M_X64)
    constexpr size_t jmp_size = 12;  // MOV RAX, imm64 (10 bytes) + JMP RAX (2 bytes)
#else
    constexpr size_t jmp_size = 5;   // JMP rel32 (5 bytes)
#endif
    const size_t trampoline_size = prolog_size + jmp_size;

#ifdef _WIN32
    void* trampoline = VirtualAlloc(nullptr, trampoline_size,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
#else
    void* trampoline = mmap(nullptr, trampoline_size,
                            PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED)
        return nullptr;
#endif
    if (!trampoline)
        return nullptr;

    uint8_t* tramp = static_cast<uint8_t*>(trampoline);

    // Copy the original prolog and fix up any PC-relative instructions.
    std::memcpy(tramp, reinterpret_cast<const void*>(target_func), prolog_size);
    fix_relative_instructions(tramp, target_func, prolog_size);

    uint8_t* p = tramp + prolog_size;
    const uintptr_t resume_addr = target_func + prolog_size;

#if defined(__x86_64__) || defined(_M_X64)
    // MOV RAX, imm64
    p[0] = 0x48; p[1] = 0xB8;
    memcpy(p + 2, &resume_addr, 8);
    // JMP RAX
    p[10] = 0xFF; p[11] = 0xE0;
#else
    const int32_t rel = static_cast<int32_t>(resume_addr - (reinterpret_cast<uintptr_t>(p) + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
#endif

    return trampoline;
}

size_t get_patch_length(void* target, size_t min_size)
{
    size_t total = 0;
    while (total < min_size) {
        hde_t  hs;
        size_t len = hde_disasm(static_cast<uint8_t*>(target) + total, &hs);
        if (hs.flags & F_ERROR)
            return 0;
        total += len;
    }
    return total;
}

#endif  // DISABLE_GET_PATCH_LENGTH

// ===============================================================
// Hook install / restore
// ===============================================================

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