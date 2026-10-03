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

#include "hook/trampoline.h"
#include <atomic>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include "hook/hde/hde64.h"
using hde_t = hde64s;
#define hde_disasm hde64_disasm
#else
#include "hook/hde/hde32.h"
using hde_t = hde32s;
#define hde_disasm hde32_disasm
#endif

// ---------------------------------------------------------------
// Executable memory reachable with rel32
// ---------------------------------------------------------------
//

namespace {

constexpr size_t    kPoolSize  = 0x10000;     // 64 KiB, also the Windows allocation granularity
constexpr uintptr_t kNearRange = 0x70000000;  // a bit under 2 GiB: leaves room for the body and branch targets
constexpr int       kMaxPools  = 64;

struct ExecPool {
    uintptr_t base;
    size_t    used;
};

ExecPool g_pools[kMaxPools];
int g_pool_count = 0;
std::atomic_flag g_pool_lock = ATOMIC_FLAG_INIT;

struct PoolLock {
    PoolLock()  { while (g_pool_lock.test_and_set(std::memory_order_acquire)) {} }
    ~PoolLock() { g_pool_lock.clear(std::memory_order_release); }
};

#if defined(_WIN32)

// Maps kPoolSize bytes of RWX memory as close to `target` as the address space allows
void* map_pool_near(uintptr_t target)
{
#if defined(_M_X64)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t min_addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t max_addr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    const uintptr_t lo = target > min_addr + kNearRange ? target - kNearRange : min_addr;
    const uintptr_t hi = target + kNearRange < max_addr ? target + kNearRange : max_addr;

    // Upwards from the target
    for (uintptr_t a = (target + gran - 1) & ~(gran - 1); a < hi; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
        const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State == MEM_FREE && a + kPoolSize <= region_end) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), kPoolSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return p;
        }
        a = (region_end + gran - 1) & ~(gran - 1);
    }

    // Downwards from the target
    for (uintptr_t a = target & ~(gran - 1); a > lo; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a - 1), &mbi, sizeof(mbi))) break;
        const uintptr_t region_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t region_end  = region_base + mbi.RegionSize;
        if (mbi.State == MEM_FREE && region_end >= kPoolSize) {
            const uintptr_t c = (region_end - kPoolSize) & ~(gran - 1);
            if (c >= region_base && c >= lo) {
                if (void* p = VirtualAlloc(reinterpret_cast<void*>(c), kPoolSize,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                    return p;
            }
        }
        if (region_base <= lo) break;
        a = region_base;
    }
    return nullptr;
#else
    (void)target; // every address is reachable with rel32 in a 32-bit process
    return VirtualAlloc(nullptr, kPoolSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#endif
}

#else // Linux

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

void* map_pool_near(uintptr_t target)
{
#if defined(__x86_64__)
    const uintptr_t lo = target > kNearRange + kPoolSize ? target - kNearRange : kPoolSize;
    const uintptr_t hi = target + kNearRange;

    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return nullptr;

    uintptr_t best = 0, best_dist = ~static_cast<uintptr_t>(0);
    bool      have = false;

    // The free gap [gap_lo, gap_hi) -> the pool-aligned address inside it closest to the target
    auto consider = [&](uintptr_t gap_lo, uintptr_t gap_hi) {
        const uintptr_t mask = ~(static_cast<uintptr_t>(kPoolSize) - 1);
        gap_lo = (gap_lo + kPoolSize - 1) & mask;
        if (gap_lo < lo) gap_lo = (lo + kPoolSize - 1) & mask;
        if (gap_hi > hi) gap_hi = hi;
        if (gap_hi < gap_lo + kPoolSize) return;
        const uintptr_t last = (gap_hi - kPoolSize) & mask;
        if (last < gap_lo) return;
        const uintptr_t c = target < gap_lo ? gap_lo : (target > last ? last : (target & mask));
        const uintptr_t d = c > target ? c - target : target - c;
        if (d < best_dist) { best_dist = d; best = c; have = true; }
    };

    uintptr_t prev_end = kPoolSize;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long s = 0, e = 0;
        if (sscanf(line, "%lx-%lx", &s, &e) != 2) continue;
        if (s > prev_end) consider(prev_end, s);
        if (e > prev_end) prev_end = e;
        if (s >= hi) break;
    }
    consider(prev_end, static_cast<uintptr_t>(0x7ffffffff000ULL));
    fclose(fp);

    if (!have) return nullptr;

    void* p = mmap(reinterpret_cast<void*>(best), kPoolSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    if (reinterpret_cast<uintptr_t>(p) != best) {
        munmap(p, kPoolSize);
        return nullptr;
    }
    return p;
#else
    (void)target; // every address is reachable with rel32 in a 32-bit process
    void* p = mmap(nullptr, kPoolSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

#endif  // _WIN32

uint8_t* pool_reserve(uintptr_t target, size_t size, int* idx)
{
    if (size > kPoolSize) return nullptr;

    for (int i = 0; i < g_pool_count; ++i) {
        const ExecPool& p = g_pools[i];
        if (kPoolSize - p.used < size) continue;
#if defined(__x86_64__) || defined(_M_X64)
        const uintptr_t d = p.base > target ? p.base - target : target - p.base;
        if (d >= kNearRange) continue;
#endif
        *idx = i;
        return reinterpret_cast<uint8_t*>(p.base + p.used);
    }

    if (g_pool_count == kMaxPools) return nullptr;
    void* mem = map_pool_near(target);
    if (!mem) return nullptr;

    g_pools[g_pool_count] = { reinterpret_cast<uintptr_t>(mem), 0 };
    *idx = g_pool_count++;
    return static_cast<uint8_t*>(mem);
}

void pool_commit(int idx, size_t size)
{
    g_pools[idx].used += (size + 15) & ~static_cast<size_t>(15);
}

// ---------------------------------------------------------------
// Branch emitters
// ---------------------------------------------------------------

inline bool rel32_fits(intptr_t d)
{
#if defined(__x86_64__) || defined(_M_X64)
    return d >= INT32_MIN && d <= INT32_MAX;
#else
    (void)d;
    return true;
#endif
}

inline intptr_t rel_to(uintptr_t dest, const uint8_t* next_ip)
{
    return static_cast<intptr_t>(dest) - static_cast<intptr_t>(reinterpret_cast<uintptr_t>(next_ip));
}

constexpr size_t kMaxInsnExpansion = 16;
constexpr size_t kJumpBackSize = 16;

uint8_t* emit_jmp(uint8_t* o, uintptr_t dest)
{
    const intptr_t rel = rel_to(dest, o + 5);
    if (rel32_fits(rel)) { // E9 rel32
        const int32_t r = static_cast<int32_t>(rel);
        o[0] = 0xE9;
        memcpy(o + 1, &r, 4);
        return o + 5;
    }
#if defined(__x86_64__) || defined(_M_X64)
    o[0] = 0xFF; o[1] = 0x25; // jmp [rip+0]; dq dest
    memset(o + 2, 0, 4);
    memcpy(o + 6, &dest, 8);
    return o + 14;
#else
    return o;
#endif
}

uint8_t* emit_call(uint8_t* o, uintptr_t dest)
{
    const intptr_t rel = rel_to(dest, o + 5);
    if (rel32_fits(rel)) { // E8 rel32
        const int32_t r = static_cast<int32_t>(rel);
        o[0] = 0xE8;
        memcpy(o + 1, &r, 4);
        return o + 5;
    }
#if defined(__x86_64__) || defined(_M_X64)
    static const uint8_t head[8] = { 0xFF, 0x15, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x08 };
    memcpy(o, head, sizeof(head)); // call [rip+2]; jmp +8; dq dest
    memcpy(o + 8, &dest, 8);
    return o + 16;
#else
    return o;
#endif
}

// cc is the low nibble of the condition: 0x70 + cc is the short form of the opcode
uint8_t* emit_jcc(uint8_t* o, uint8_t cc, uintptr_t dest)
{
    const intptr_t rel = rel_to(dest, o + 6);
    if (rel32_fits(rel)) { // 0F 80+cc rel32
        const int32_t r = static_cast<int32_t>(rel);
        o[0] = 0x0F;
        o[1] = static_cast<uint8_t>(0x80 + cc);
        memcpy(o + 2, &r, 4);
        return o + 6;
    }
#if defined(__x86_64__) || defined(_M_X64)
    o[0] = static_cast<uint8_t>(0x70 + (cc ^ 1)); // jNcc +14 (over the absolute jump)
    o[1] = 0x0E;
    o[2] = 0xFF; o[3] = 0x25; // jmp [rip+0]; dq dest
    memset(o + 4, 0, 4);
    memcpy(o + 8, &dest, 8);
    return o + 16;
#else
    return o;
#endif
}

bool build_trampoline(uint8_t* out, uintptr_t src, size_t prolog_size, size_t* out_len)
{
    const uint8_t* code = reinterpret_cast<const uint8_t*>(src);
    uint8_t* o = out;
    [[maybe_unused]] bool prev_was_call = false; // x86 PIC prologue detection only
    bool ended = false;

    for (size_t off = 0; off < prolog_size; ) {
        hde_t hs;
        const size_t len = hde_disasm(code + off, &hs);
        if ((hs.flags & F_ERROR) || len == 0 || off + len > prolog_size) return false;

        const uintptr_t next = src + off + len;
        const uint8_t op = hs.opcode;
        const bool is_jcc8 = op >= 0x70 && op <= 0x7F;
        const bool is_jcc32 = op == 0x0F && hs.opcode2 >= 0x80 && hs.opcode2 <= 0x8F;
        bool is_call = false;

        if (op == 0xE8 || op == 0xE9 || op == 0xEB || is_jcc8 || is_jcc32) {
            if (hs.flags & F_PREFIX_ANY) return false;

            const bool rel8 = op == 0xEB || is_jcc8;
            const intptr_t rel = rel8 ? static_cast<int8_t>(hs.imm.imm8)
                                      : static_cast<int32_t>(hs.imm.imm32);
            const uintptr_t dest = static_cast<uintptr_t>(static_cast<intptr_t>(next) + rel);

            if (dest >= src && dest < src + prolog_size) return false;
            if (op == 0xE8) { o = emit_call(o, dest); is_call = true; }
            else if (rel8 && op == 0xEB) o = emit_jmp(o, dest);
            else if (op == 0xE9) o = emit_jmp(o, dest);
            else o = emit_jcc(o, static_cast<uint8_t>((is_jcc8 ? op : hs.opcode2) & 0x0F), dest);
        }
        else if (op >= 0xE0 && op <= 0xE3) {
            return false;
        }
        else {
            memcpy(o, code + off, len);
#if defined(__x86_64__) || defined(_M_X64)
            // RIP-relative operand: keep pointing at the same absolute address
            if ((hs.flags & F_MODRM) && (hs.flags & F_DISP32) && hs.modrm_mod == 0 && hs.modrm_rm == 5) {
                size_t imm_size = 0;
                if (hs.flags & F_IMM64)      imm_size = 8;
                else if (hs.flags & F_IMM32) imm_size = 4;
                else if (hs.flags & F_IMM16) imm_size = 2;
                else if (hs.flags & F_IMM8)  imm_size = 1;
                const size_t field = len - imm_size - 4;

                int32_t old_disp;
                memcpy(&old_disp, o + field, 4);
                const intptr_t abs_target = static_cast<intptr_t>(next) + old_disp;
                const intptr_t new_disp   = abs_target - static_cast<intptr_t>(reinterpret_cast<uintptr_t>(o) + len);
                if (!rel32_fits(new_disp)) return false;

                const int32_t d = static_cast<int32_t>(new_disp);
                memcpy(o + field, &d, 4);
            }
#else
            // call __x86.get_pc_thunk.xx ; add eax, _GLOBAL_OFFSET_TABLE_ (PIC prologue)
            if (prev_was_call && op == 0x05 && !(hs.flags & F_PREFIX_ANY)) {
                uint32_t imm;
                memcpy(&imm, o + 1, 4);
                imm += static_cast<uint32_t>(next - (reinterpret_cast<uintptr_t>(o) + len));
                memcpy(o + 1, &imm, 4);
            }
#endif
            o += len;
        }

        prev_was_call = is_call;
        off += len;

        const bool terminator = op == 0xC3 || op == 0xC2 || op == 0xE9 || op == 0xEB ||
                               (op == 0xFF && hs.modrm_reg == 4) ||
                               (op == 0x0F && hs.opcode2 == 0x0B);
        if (terminator) {
            for (size_t i = off; i < prolog_size; ++i) {
                if (code[i] != 0xCC && code[i] != 0x90 && code[i] != 0x00) return false;
            }
            ended = true;
            break;
        }
    }

    if (!ended) o = emit_jmp(o, src + prolog_size);
    *out_len = static_cast<size_t>(o - out);
    return true;
}

}   // namespace

void* alloc_exec_near(uintptr_t target, size_t size)
{
    PoolLock lock;
    int idx = 0;
    uint8_t* p = pool_reserve(target, size, &idx);
    if (!p) return nullptr;
    pool_commit(idx, size);
    return p;
}

void* create_trampoline_with_prolog(uintptr_t target_func, size_t prolog_size)
{
    if (!target_func || prolog_size == 0 || prolog_size > 64)
        return nullptr;

    const size_t cap = prolog_size * kMaxInsnExpansion + kJumpBackSize;

    PoolLock lock;
    int idx = 0;
    uint8_t* tramp = pool_reserve(target_func, cap, &idx);
    if (!tramp)
        return nullptr;

    size_t len = 0;
    if (!build_trampoline(tramp, target_func, prolog_size, &len))
        return nullptr;

    pool_commit(idx, len);
    return tramp;
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
