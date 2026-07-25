# ------------------------------------------------------------
# hook_x86.S - KiriKiri hook (x86 32-bit)
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
.extern g_jump_krkr_1
.extern g_jump_krkr_2
.extern g_jump_krkrz
.extern Hook_KrkrChar1
.extern Hook_KrkrChar2
.extern Hook_KrkrzChar

# Single-argument hook stub
.macro HOOK_STUB NAME, CALLBACK, JUMP_VAR, REG_OFF, TAG
    .globl  \NAME
    .type   \NAME, @function
\NAME:
    pushad
    pushfd
    mov     eax, [esp + \REG_OFF]
    push    eax

#if defined(_WIN32) || defined(__MINGW32__)
    # Windows: direct call
    call    \CALLBACK
    add     esp, 4
#else
    # Linux: PIC call through GOT
    call    .L_getpc_cb_\TAG
.L_getpc_cb_\TAG:
    pop     ebx
    add     ebx, OFFSET _GLOBAL_OFFSET_TABLE_ + (. - .L_getpc_cb_\TAG)
    mov     eax, [ebx + \CALLBACK@GOT]
    call    eax
    add     esp, 4
#endif
    popfd
    popad

    # Jump to trampoline
#if defined(_WIN32) || defined(__MINGW32__)
    jmp     [\JUMP_VAR]
#else
    push    eax
    call    .L_getpc_jmp_\TAG
.L_getpc_jmp_\TAG:
    pop     eax
    add     eax, OFFSET _GLOBAL_OFFSET_TABLE_ + (. - .L_getpc_jmp_\TAG)
    mov     eax, [eax + \JUMP_VAR@GOT]
    mov     eax, [eax]
    xchg    eax, [esp]
    ret
#endif
    .size   \NAME, . - \NAME
.endm

# Two-argument hook stub
.macro HOOK_STUB2 NAME, CALLBACK, JUMP_VAR, REG_OFF1, REG_OFF2, TAG
    .globl  \NAME
    .type   \NAME, @function
\NAME:
    pushad
    pushfd
    mov     eax, [esp + \REG_OFF2]
    push    eax
    mov     eax, [esp + \REG_OFF1 + 4]
    push    eax

#if defined(_WIN32) || defined(__MINGW32__)
    # Windows: direct call
    call    \CALLBACK
    add     esp, 8
#else
    # Linux: PIC call through GOT
    call    .L_getpc_cb_\TAG
.L_getpc_cb_\TAG:
    pop     ebx
    add     ebx, OFFSET _GLOBAL_OFFSET_TABLE_ + (. - .L_getpc_cb_\TAG)
    mov     eax, [ebx + \CALLBACK@GOT]
    call    eax
    add     esp, 8
#endif
    popfd
    popad

    # Jump to trampoline
#if defined(_WIN32) || defined(__MINGW32__)
    jmp     [\JUMP_VAR]
#else
    push    eax
    call    .L_getpc_jmp_\TAG
.L_getpc_jmp_\TAG:
    pop     eax
    add     eax, OFFSET _GLOBAL_OFFSET_TABLE_ + (. - .L_getpc_jmp_\TAG)
    mov     eax, [eax + \JUMP_VAR@GOT]
    mov     eax, [eax]
    xchg    eax, [esp]
    ret
#endif
    .size   \NAME, . - \NAME
.endm

HOOK_STUB  hook_krkr_1, Hook_KrkrChar1, g_jump_krkr_1, 32, 1        # eax
HOOK_STUB2 hook_krkr_2, Hook_KrkrChar2, g_jump_krkr_2, 20, 28, 2    # ebx, ecx
HOOK_STUB  hook_krkrz,  Hook_KrkrzChar,  g_jump_krkrz,  28, 3       # ecx