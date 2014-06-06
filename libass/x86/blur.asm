;******************************************************************************
;* blur.asm: SSE2/AVX2 tile blur
;******************************************************************************
;* Copyright (C) 2014 Vabishchevich Nikolay <vabnick@gmail.com>
;*
;* This file is part of libass.
;*
;* Permission to use, copy, modify, and distribute this software for any
;* purpose with or without fee is hereby granted, provided that the above
;* copyright notice and this permission notice appear in all copies.
;*
;* THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
;* WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
;* MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
;* ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
;* WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
;* ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
;* OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
;******************************************************************************

%include "utils.asm"

SECTION_RODATA 32

words_one: dw 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1

SECTION .text

;------------------------------------------------------------------------------
; UPDATE_FLAG 1:m_flag1, 2:[m_flag2],
;             3:m_src1, 4:m_src2, 5:m_cmp1, 6:m_cmp2,
;             7:m_tmp, 8:index1, 9:index2
; Compare two vectors with their corresponding templates and update flag
;------------------------------------------------------------------------------

%macro UPDATE_FLAG 9
    pxor m%3, m%5
    pxor m%4, m%6

%if ARCH_X86_64

%if %8 == 1
    por m%1, m%3
%else
    por m%2, m%3
%endif
%if %9 == 1
    por m%1, m%4
%else
    por m%2, m%4
%endif

%else

%if %8 == %9
    por m%3, m%4
    pxor m%4, m%4
%endif
%if %8 == 1
    punpcklwd m%7, m%3, m%4
    punpckhwd m%3, m%4,
    por m%1, m%7
    por m%1, m%3
%else
    punpcklwd m%7, m%4, m%3
    punpckhwd m%4, m%3,
    por m%1, m%7
    por m%1, m%4
%endif

%endif
%endmacro

;------------------------------------------------------------------------------
; EXPAND_LINE_HORZ 1:m_dst1, 2:m_dst2, 3:m_prev, 4:m_src, 5:m_next, 6:m_one
; Expand line by factor of 2 with kernel [1, 5, 10, 10, 5, 1]
;------------------------------------------------------------------------------

%macro EXPAND_LINE_HORZ 6
    pslldq m%2, m%4, 2
%if mmsize == 32
    vperm2i128 m%3, m%3, m%4, 0x21
%endif
    psrldq m%3, 14
    por m%2, m%3  ; prev
    psrldq m%1, m%4, 2
%if mmsize == 32
    vperm2i128 m%3, m%4, m%5, 0x21
    pslldq m%3, 14
%else
    pslldq m%3, m%5, 14
%endif
    por m%3, m%1  ; next
    paddw m%1, m%2, m%3
    psrlw m%1, 1
    paddw m%1, m%4
    psrlw m%1, 1
    paddw m%2, m%1
    psrlw m%2, 1
    paddw m%3, m%1
    psrlw m%3, 1
    paddw m%2, m%4
    psrlw m%2, 1
    paddw m%3, m%4
    psrlw m%3, 1
    punpcklwd m%1, m%2, m%3
    punpckhwd m%2, m%3,
%if mmsize == 32
    mova m%3, m%1  ; XXX: swizzle
    vperm2i128 m%1, m%3, m%2, 0x20
    vperm2i128 m%2, m%3, m%2, 0x31
%endif
%endmacro

;------------------------------------------------------------------------------
; EXPAND_HORZ_TILE 1:tile_order, 2:suffix
; int expand_horz_tile%2(int16_t *dst1, int16_t *dst2,
;                        const int16_t *side1, const int16_t *src, const int16_t *side2);
;------------------------------------------------------------------------------

%macro EXPAND_HORZ_TILE 2
%if ARCH_X86_64
cglobal expand_horz_tile%2, 5,6,9
    pxor m8, m8
%else
cglobal expand_horz_tile%2, 5,6,8
%endif
    mova m6, [words_one]
    pxor m7, m7

    xor r5d, r5d
.main_loop

%if (2 << %1) == mmsize
    mova m5, [r2 + r5]
    mova m0, m5  ; XXX: swizzle
    mova m1, [r3 + r5]
    mova m2, [r4 + r5]
    EXPAND_LINE_HORZ 3,4, 0,1,2, 6
    mova [r0 + r5], m3
    mova [r1 + r5], m4
    UPDATE_FLAG 7,8, 3,4,5,2, 2, 1,2
%else
    mova m5, [r2 + r5 + (2 << %1) - mmsize]
    mova m0, m5
    mova m1, [r3 + r5]
    %assign %%i 0
%rep (1 << %1) / mmsize
    mova m2, [r3 + r5 + %%i + mmsize]
    EXPAND_LINE_HORZ 3,4, 0,1,2, 6
    SWAP 0, 1, 2
    mova [r0 + r5 + 2 * %%i], m3
    mova [r0 + r5 + 2 * %%i + mmsize], m4
    UPDATE_FLAG 7,8, 3,4,5,5, 2, 1,1
    %assign %%i %%i + mmsize
%endrep
    mova m5, [r4 + r5]
%rep (1 << %1) / mmsize
%if %%i + mmsize == (2 << %1)
    EXPAND_LINE_HORZ 3,4, 0,1,5, 6
%else
    mova m2, [r3 + r5 + %%i + mmsize]
    EXPAND_LINE_HORZ 3,4, 0,1,2, 6
%endif
    SWAP 0, 1, 2
    mova [r1 + r5 + 2 * %%i - (2 << %1)], m3
    mova [r1 + r5 + 2 * %%i - (2 << %1) + mmsize], m4
    UPDATE_FLAG 7,8, 3,4,5,5, 2, 2,2
    %assign %%i %%i + mmsize
%endrep
%endif

    add r5d, 2 << %1
    cmp r5d, 2 << (2 * %1)
    jnz .main_loop

    xor eax, eax
    pxor m6, m6
    pcmpeqw m7, m6
    pmovmskb r0d, m7
%if ARCH_X86_64
    cmp r0d, 0xFFFFFFFF >> (32 - mmsize)
%else
    cmp r0d, (0xFFFFFFFF >> (32 - mmsize)) & 0x33333333
%endif
    sete al
%if ARCH_X86_64
    pcmpeqw m8, m6
    pmovmskb r0d, m8
    cmp r0d, 0xFFFFFFFF >> (32 - mmsize)
%else
    cmp r0d, (0xFFFFFFFF >> (32 - mmsize)) & 0xCCCCCCCC
%endif
    sete r0b
    add r0d, r0d
    or al, r0b
    RET
%endmacro

INIT_XMM sse2
EXPAND_HORZ_TILE 4,16
EXPAND_HORZ_TILE 5,32
INIT_YMM avx2
EXPAND_HORZ_TILE 4,16
EXPAND_HORZ_TILE 5,32

;------------------------------------------------------------------------------
; EXPAND_LINE_VERT 1:m_dst1, 2:m_dst1, 3:m_prev, 4:m_src, 5:m_next, 6:m_one
; Expand line by factor of 2 with kernel [1, 5, 10, 10, 5, 1]
;------------------------------------------------------------------------------

%macro EXPAND_LINE_VERT 6
    mova m%1, m%3
    mova m%2, m%5
    paddw m%3, m%5
    psrlw m%3, 1
    paddw m%3, m%4
    psrlw m%3, 1
    paddw m%1, m%3
    paddw m%2, m%3
    psrlw m%1, 1
    psrlw m%2, 1
    paddw m%3, m%4, m%6
    paddw m%1, m%3
    paddw m%2, m%3
    psrlw m%1, 1
    psrlw m%2, 1
%endmacro

;------------------------------------------------------------------------------
; EXPAND_VERT_TILE 1:tile_order, 2:suffix
; int expand_vert_tile%2(int16_t *dst1, int16_t *dst2,
;                        const int16_t *side1, const int16_t *src, const int16_t *side2);
;------------------------------------------------------------------------------

%macro EXPAND_VERT_TILE 2
%if ARCH_X86_64
cglobal expand_vert_tile%2, 5,6,9
    pxor m8, m8
%else
cglobal expand_vert_tile%2, 5,6,8
%endif
    add r2, (2 << (2 * %1)) - (2 << %1)
    mova m6, [words_one]
    pxor m7, m7

    %assign %%i 0
%rep (2 << %1) / mmsize

    mova m5, [r2 + %%i]
    mova m0, m5  ; XXX: swizzle
    mova m1, [r3 + %%i]
    xor r5d, r5d
.row_loop1_ %+ %%i
    mova m2, [r3 + %%i + r5 + (2 << %1)]
    EXPAND_LINE_VERT 3,4, 0,1,2, 6
    mova m0, m1
    mova m1, m2
    mova [r0 + 2 * r5 + %%i], m3
    mova [r0 + 2 * r5 + %%i + (2 << %1)], m4
    UPDATE_FLAG 7,8, 3,4,5,5, 2, 1,1
    add r5d, 2 << %1
    cmp r5d, 1 << (2 * %1)
    jl .row_loop1_ %+ %%i

    mova m5, [r4 + %%i]
    xor r5d, r5d
.row_loop2_ %+ %%i
    mova m2, [r3 + r5 + %%i + (1 << (2 * %1)) + (2 << %1)]
    EXPAND_LINE_VERT 3,4, 0,1,2, 6
    mova m0, m1
    mova m1, m2
    mova [r1 + 2 * r5 + %%i], m3
    mova [r1 + 2 * r5 + %%i + (2 << %1)], m4
    UPDATE_FLAG 7,8, 3,4,5,5, 2, 2,2
    add r5d, 2 << %1
    cmp r5d, (1 << (2 * %1)) - (2 << %1)
    jl .row_loop2_ %+ %%i
    EXPAND_LINE_VERT 3,4, 0,1,5, 6
    mova [r1 + 2 * r5 + %%i], m3
    mova [r1 + 2 * r5 + %%i + (2 << %1)], m4
    UPDATE_FLAG 7,8, 3,4,5,5, 2, 2,2

    %assign %%i %%i + mmsize
%endrep

    xor eax, eax
    pxor m6, m6
    pcmpeqw m7, m6
    pmovmskb r0d, m7
%if ARCH_X86_64
    cmp r0d, 0xFFFFFFFF >> (32 - mmsize)
%else
    cmp r0d, (0xFFFFFFFF >> (32 - mmsize)) & 0x33333333
%endif
    sete al
%if ARCH_X86_64
    pcmpeqw m8, m6
    pmovmskb r0d, m8
    cmp r0d, 0xFFFFFFFF >> (32 - mmsize)
%else
    cmp r0d, (0xFFFFFFFF >> (32 - mmsize)) & 0xCCCCCCCC
%endif
    sete r0b
    add r0d, r0d
    or al, r0b
    RET
%endmacro

INIT_XMM sse2
EXPAND_VERT_TILE 4,16
EXPAND_VERT_TILE 5,32
INIT_YMM avx2
EXPAND_VERT_TILE 4,16
EXPAND_VERT_TILE 5,32
