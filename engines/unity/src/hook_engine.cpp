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

#include "hook_engine.h"
#include "hook/trampoline.h"
#include "hook/code_patch.h"

#include <atomic>
#include <cstring>
#include <new>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace {

constexpr int    kMaxHooks  = 4096;
constexpr size_t kMaxPatch  = 32;
constexpr size_t kJmpSize   = 5;
constexpr size_t kStubSize  = 512; // x64 stubs are ~370 bytes

struct HookRecord {
    uintptr_t target;
    uint8_t orig[kJmpSize];
};

struct EngineState {
    std::atomic<int32_t> inflight;
    std::atomic<int32_t> disabled;
};
static_assert(sizeof(std::atomic<int32_t>) == 4, "stubs assume 4-byte counters");

HookRecord g_hooks[kMaxHooks];
int g_count = 0;

EngineState* g_state   = nullptr;
HookHandler g_handler = nullptr;

struct Code {
    uint8_t* p;
    uint8_t* end;
    bool overflow;

    void b(uint8_t v)             { if (p < end) *p++ = v; else overflow = true; }
    template<typename... T>
    void b(uint8_t v, T... rest)  { b(v); b(static_cast<uint8_t>(rest)...); }
    void d32(uint32_t v)          { for (int i = 0; i < 4; ++i) b(static_cast<uint8_t>(v >> (8 * i))); }
    void d64(uint64_t v)          { for (int i = 0; i < 8; ++i) b(static_cast<uint8_t>(v >> (8 * i))); }
};

#if defined(__x86_64__) || defined(_M_X64)

// movups [rsp+i*16], xmm_i / movups xmm_i, [rsp+i*16]
void emit_xmm_access(Code& c, int i, bool store)
{
    if (i >= 8) c.b(0x44);
    c.b(0x0F, store ? 0x11 : 0x10, 0x84 | ((i & 7) << 3), 0x24);
    c.d32(static_cast<uint32_t>(i * 16));
}

bool emit_stub(uint8_t* mem, uint32_t id, uintptr_t tramp)
{
    Code c{ mem, mem + kStubSize, false };
    const uint64_t state   = reinterpret_cast<uintptr_t>(g_state);
    const uint64_t handler = reinterpret_cast<uintptr_t>(g_handler);

    // Frame: rflags, rax, rcx, rdx, rbx, rbp, rsi, rdi, r8..r15 (matches HookRegs, last pushed = lowest)
    c.b(0x9C);
    c.b(0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57);
    for (int r = 0x50; r <= 0x57; ++r) c.b(0x41, r);

    c.b(0x48, 0x89, 0xE3);                          // mov rbx, rsp ; rbx = HookRegs*
    c.b(0x48, 0x81, 0xEC); c.d32(0x100);            // sub rsp, 0x100
    for (int i = 0; i < 16; ++i) emit_xmm_access(c, i, true);

    c.b(0x48, 0x83, 0xE4, 0xF0);                    // and rsp, -16
    c.b(0x48, 0x83, 0xEC, 0x20);                    // sub rsp, 32 ; shadow space (harmless for System V)

    c.b(0x48, 0xB8); c.d64(state);                  // mov rax, state
    c.b(0xF0, 0xFF, 0x00);                          // lock inc dword [rax]
    c.b(0x83, 0x78, 0x04, 0x00);                    // cmp dword [rax+4], 0 ; disabled?
    c.b(0x75, 0x14);                                // jne skip
#if defined(_WIN32)
    c.b(0x48, 0x89, 0xD9);                          // mov rcx, rbx
    c.b(0xBA);                                      // mov edx, id
#else
    c.b(0x48, 0x89, 0xDF);                          // mov rdi, rbx
    c.b(0xBE);                                      // mov esi, id
#endif
    c.d32(id);
    c.b(0x48, 0xB8); c.d64(handler);                // mov rax, handler
    c.b(0xFF, 0xD0);                                // call rax
                                                    // skip:
    c.b(0x48, 0xB8); c.d64(state);                  // mov rax, state
    c.b(0xF0, 0xFF, 0x08);                          // lock dec dword [rax]

    c.b(0x48, 0x8D, 0xA3); c.d32(0xFFFFFF00);       // lea rsp, [rbx-0x100]
    for (int i = 0; i < 16; ++i) emit_xmm_access(c, i, false);
    c.b(0x48, 0x81, 0xC4); c.d32(0x100);            // add rsp, 0x100 ; rsp == rbx

    for (int r = 0x5F; r >= 0x58; --r) c.b(0x41, r);// pop r15..r8
    c.b(0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58);  // pop rdi rsi rbp rbx rdx rcx rax
    c.b(0x9D);                                      // popfq

    c.b(0xFF, 0x25); c.d32(0);                      // jmp [rip+0]
    c.d64(tramp);                                   // dq trampoline
    return !c.overflow;
}

#else   // x86

bool emit_stub(uint8_t* mem, uint32_t id, uintptr_t tramp)
{
    Code c{ mem, mem + kStubSize, false };
    const uint32_t state   = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_state));
    const uint32_t handler = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_handler));

    c.b(0x9C);                                      // pushfd
    c.b(0x60);                                      // pushad ; matches HookRegs
    c.b(0x89, 0xE3);                                // mov ebx, esp ; ebx = HookRegs*
    c.b(0x83, 0xE4, 0xF0);                          // and esp, -16

    c.b(0xB8); c.d32(state);                        // mov eax, state
    c.b(0xF0, 0xFF, 0x00);                          // lock inc dword [eax]
    c.b(0x83, 0x78, 0x04, 0x00);                    // cmp dword [eax+4], 0 ; disabled?
    c.b(0x75, 0x13);                                // jne skip
    c.b(0x83, 0xEC, 0x08);                          // sub esp, 8 ; keeps esp 16-aligned at the call
    c.b(0x68); c.d32(id);                           // push id
    c.b(0x53);                                      // push ebx
    c.b(0xB8); c.d32(handler);                      // mov eax, handler
    c.b(0xFF, 0xD0);                                // call eax ; cdecl
    c.b(0x83, 0xC4, 0x10);                          // add esp, 16
                                                    // skip:
    c.b(0xB8); c.d32(state);                        // mov eax, state
    c.b(0xF0, 0xFF, 0x08);                          // lock dec dword [eax]

    c.b(0x89, 0xDC);                                // mov esp, ebx
    c.b(0x61);                                      // popad
    c.b(0x9D);                                      // popfd

    c.b(0xE9);                                      // jmp tramp
    const uint32_t rel = static_cast<uint32_t>(tramp - (reinterpret_cast<uintptr_t>(c.p) + 4));
    c.d32(rel);
    return !c.overflow;
}

#endif

} // namespace

bool hook_engine_init(HookHandler handler)
{
    if (!handler || g_state)
        return false;

    void* mem = alloc_exec_near(reinterpret_cast<uintptr_t>(handler), sizeof(EngineState));
    if (!mem)
        return false;

    g_state = new (mem) EngineState{};
    g_state->inflight.store(0);
    g_state->disabled.store(0);

    g_handler = handler;
    return true;
}

HookResult hook_engine_install(void* target, uint32_t user_id)
{
    if (!g_state || !g_handler)
        return HookResult::NotInitialized;
    
    if (g_count >= kMaxHooks)
        return HookResult::TableFull;

    const uintptr_t addr = reinterpret_cast<uintptr_t>(target);
    for (int i = 0; i < g_count; ++i)
        if (g_hooks[i].target == addr)
            return HookResult::AlreadyHooked;

    if ((addr & 15) == 15)
        return HookResult::PatchFailed;

    const size_t patch = get_patch_length(target, 5);
    if (patch == 0 || patch > kMaxPatch)
        return HookResult::PrologUnsafe;

    void* tramp = create_trampoline_with_prolog(addr, patch);
    if (!tramp)
        return HookResult::PrologUnsafe;

    uint8_t* stub = static_cast<uint8_t*>(alloc_exec_near(addr, kStubSize));
    if (!stub)
        return HookResult::NoNearMemory;
    
    if (!emit_stub(stub, user_id, reinterpret_cast<uintptr_t>(tramp)))
        return HookResult::PatchFailed;

    HookRecord& rec = g_hooks[g_count];
    rec.target = addr;
    std::memcpy(rec.orig, target, kJmpSize);

    if (!install_hook_rel32(addr, reinterpret_cast<uintptr_t>(stub)))
        return HookResult::PatchFailed;

    ++g_count;
    return HookResult::Ok;
}

bool hook_engine_shutdown(unsigned timeout_ms)
{
    if (!g_state)
        return true;

    g_state->disabled.store(1);

    for (int i = 0; i < g_count; ++i) {
        write_code(g_hooks[i].target, g_hooks[i].orig, kJmpSize);
    }
    g_count = 0;

    bool drained = true;
    for (unsigned waited = 0; g_state->inflight.load() != 0; ++waited) {
        if (waited >= timeout_ms) {
            drained = false;
            break;
        }
#if defined(_WIN32)
        Sleep(1);
#else
        usleep(1000);
#endif
    }

    g_state   = nullptr;
    g_handler = nullptr;
    return drained;
}
