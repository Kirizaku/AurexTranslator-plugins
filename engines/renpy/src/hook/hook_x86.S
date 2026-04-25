# ------------------------------------------------------------
# hook_x86.S - PyUnicode_Format hook (x86 32-bit)
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

hook:
    # Save all general‑purpose registers and flags
    pushad
    pushfd

    # After pushad(32) + pushfd(4) = 36 bytes shift,
    # arguments are at [esp+44] and [esp+48] respectively.
    # Push them in reverse order for cdecl call
    mov     eax, [esp + 44]     # args
    push    eax
    mov     eax, [esp + 44]     # format (esp shifted by 4 after previous push)
    push    eax

#if defined(_WIN32) || defined(__MINGW32__)
    # Windows: direct call
    call    Hook_PyUnicode_Format
    add     esp, 8              # cdecl cleanup: 2 args * 4 bytes
#else
    # Linux: PIC call through GOT
    call    .L_getpc1
.L_getpc1:
    pop     ebx
    add     ebx, OFFSET _GLOBAL_OFFSET_TABLE_ + (. - .L_getpc1)
    mov     eax, [ebx + Hook_PyUnicode_Format@GOT]
    call    eax
    add     esp, 8
#endif

    popfd
    popad

    # Jump to trampoline
#if defined(_WIN32) || defined(__MINGW32__)
    jmp     [g_jump_addr]
#else
    call    .L_getpc2
.L_getpc2:
    pop     eax
    add     eax, OFFSET _GLOBAL_OFFSET_TABLE_ + (. - .L_getpc2)
    mov     eax, [eax + g_jump_addr@GOT]
    mov     eax, [eax]
    jmp     eax
#endif
