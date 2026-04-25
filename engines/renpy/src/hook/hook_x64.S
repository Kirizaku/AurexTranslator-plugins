# ------------------------------------------------------------
# hook_x64.S - PyUnicode_Format hook (x86_64)
# Supports: Linux native, Wine, Windows
#
# Licensed under the MIT License <http://opensource.org/licenses/MIT>.
# Copyright (c) 2026 Daniil Nabiulin <https://github.com/kirizaku>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
# ------------------------------------------------------------

.intel_syntax noprefix
.text
.extern Hook_PyUnicode_Format
.extern g_jump_addr
.globl hook
.type hook, @function

#define SAVE_CONTEXT    \
    push rax;           \
    push rbx;           \
    push rcx;           \
    push rdx;           \
    push rsi;           \
    push rdi;           \
    push rbp;           \
    push r8;            \
    push r9;            \
    push r10;           \
    push r11;           \
    push r12;           \
    push r13;           \
    push r14;           \
    push r15;           \
    pushfq

#define RESTORE_CONTEXT \
    popfq;              \
    pop  r15;           \
    pop  r14;           \
    pop  r13;           \
    pop  r12;           \
    pop  r11;           \
    pop  r10;           \
    pop  r9;            \
    pop  r8;            \
    pop  rbp;           \
    pop  rdi;           \
    pop  rsi;           \
    pop  rdx;           \
    pop  rcx;           \
    pop  rbx;           \
    pop  rax

hook:
    # Save context first to avoid clobbering callee-saved registers
    SAVE_CONTEXT

    # Read arguments from saved stack slots.
    # After SAVE_CONTEXT, layout (top to bottom):
    #   [rsp+0]   = rflags
    #   [rsp+8]   = r15
    #   [rsp+16]  = r14
    #   [rsp+24]  = r13
    #   [rsp+32]  = r12
    #   [rsp+40]  = r11
    #   [rsp+48]  = r10
    #   [rsp+56]  = r9
    #   [rsp+64]  = r8
    #   [rsp+72]  = rbp
    #   [rsp+80]  = rdi    <- Linux format
    #   [rsp+88]  = rsi    <- Linux args
    #   [rsp+96]  = rdx    <- Wine  args
    #   [rsp+104] = rcx    <- Wine  format
    #   [rsp+112] = rbx
    #   [rsp+120] = rax
    sub  rsp, 40       # 8 alignment + 32 shadow space (Windows ABI)

#if defined(_WIN32) || defined(__MINGW32__)
    # Windows x64: Hook_PyUnicode_Format(format, args) via rcx, rdx
    mov  rcx, [rsp + 40 + 104]   # original rcx = format
    mov  rdx, [rsp + 40 +  96]   # original rdx = args
    call Hook_PyUnicode_Format
#else
    # Linux/Wine: Hook_PyUnicode_Format(linux_fmt, linux_args, wine_fmt, wine_args)
    mov  rdi, [rsp + 40 +  80]   # original rdi = linux format
    mov  rsi, [rsp + 40 +  88]   # original rsi = linux args
    mov  rdx, [rsp + 40 + 104]   # original rcx = wine  format
    mov  rcx, [rsp + 40 +  96]   # original rdx = wine  args
    mov  rax, [rip + Hook_PyUnicode_Format@GOTPCREL]
    call rax
#endif

    add  rsp, 40
    RESTORE_CONTEXT

#if defined(_WIN32) || defined(__MINGW32__)
    jmp  [g_jump_addr]
#else
    mov  rax, qword ptr [rip + g_jump_addr@GOTPCREL]
    mov  rax, [rax]
    jmp  rax
#endif
