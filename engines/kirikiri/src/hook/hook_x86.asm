; ------------------------------------------------------------
; hook_x86.asm (x86 32-bit, MSVC/MASM syntax)
; Windows only
;
; Licensed under the MIT License <http://opensource.org/licenses/MIT>.
; Copyright (c) 2026 Daniil Nabiulin <https://github.com/kirizaku>
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in all
; copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
; SOFTWARE.
; ------------------------------------------------------------
.386
.model flat, C
EXTERN g_jump_krkr_1:DWORD
EXTERN g_jump_krkr_2:DWORD
EXTERN g_jump_krkrz:DWORD
EXTERN Hook_KrkrChar1:PROC
EXTERN Hook_KrkrChar2:PROC
EXTERN Hook_KrkrzChar:PROC
.code

; Single-argument hook stub
HOOK_STUB MACRO Name:REQ, Callback:REQ, JumpVar:REQ, RegOff:REQ
    PUBLIC Name
Name PROC
    pushad
    pushfd
    mov     eax, [esp + RegOff]
    push    eax
    call    Callback
    add     esp, 4
    popfd
    popad
    jmp     DWORD PTR [JumpVar]
Name ENDP
ENDM

; Two-argument hook stub
HOOK_STUB2 MACRO Name:REQ, Callback:REQ, JumpVar:REQ, RegOff1:REQ, RegOff2:REQ
    PUBLIC Name
Name PROC
    pushad
    pushfd
    mov     eax, [esp + RegOff2]
    push    eax
    mov     eax, [esp + RegOff1 + 4]
    push    eax
    call    Callback
    add     esp, 8
    popfd
    popad
    jmp     DWORD PTR [JumpVar]
Name ENDP
ENDM

HOOK_STUB  hook_krkr_1, Hook_KrkrChar1, g_jump_krkr_1, 32
HOOK_STUB2 hook_krkr_2, Hook_KrkrChar2, g_jump_krkr_2, 20, 28
HOOK_STUB  hook_krkrz,  Hook_KrkrzChar, g_jump_krkrz,  28
END
