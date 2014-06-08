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

words_one: times 16 dw 1
dwords_round: times 8 dd 32768

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

;------------------------------------------------------------------------------
; FILTER_LINE_HORZ 1:type, 2:m_dst/m_prev, 3:m_src, 4:m_next,
;                  5:m_mul, 6:[m_round], 7-10:m_tmp
; Apply generic 9-tap filter
;------------------------------------------------------------------------------

%macro FILTER_LINE_HORZ 10
    %assign %%i1 %1 / 1000 % 10
    %assign %%i2 %1 / 100 % 10
    %assign %%i3 %1 / 10 % 10
    %assign %%i4 %1 / 1 % 10

%if mmsize == 32
    vperm2i128 m%2, m%2, m%3, 0x21
%endif
    psrldq m%2, 16 - 2 * %%i4
    pslldq m%7, m%3, 2 * %%i4
    por m%7, m%2
    psrldq m%2, 2 * (%%i4 - %%i3)
    pslldq m%8, m%3, 2 * %%i3
    por m%8, m%2
    psubw m%7, m%3
    psubw m%8, m%3
    punpcklwd m%9, m%8, m%7
    punpckhwd m%8, m%7
    pshufd m%7, m%5, q1111
    pmaddwd m%9, m%7
    pmaddwd m%8, m%7
%if ARCH_X86_64
    paddd m%9, m%6
    paddd m%10, m%6, m%8
%else
    paddd m%9, m%10
    paddd m%10, m%8
%endif

    psrldq m%2, 2 * (%%i3 - %%i2)
    pslldq m%7, m%3, 2 * %%i2
    por m%7, m%2
    psrldq m%2, 2 * (%%i2 - %%i1)
    pslldq m%8, m%3, 2 * %%i1
    por m%8, m%2
    psubw m%7, m%3
    psubw m%8, m%3
    punpcklwd m%2, m%8, m%7
    punpckhwd m%8, m%7
    pshufd m%7, m%5, q0000
    pmaddwd m%2, m%7
    pmaddwd m%8, m%7
    paddd m%9, m%2
    paddd m%10, m%8

%if mmsize == 32
    vperm2i128 m%2, m%3, m%4, 0x21
    pslldq m%2, 16 - 2 * %%i4
%else
    pslldq m%2, m%4, 16 - 2 * %%i4
%endif
    psrldq m%7, m%3, 2 * %%i4
    por m%7, m%2
    pslldq m%2, 2 * (%%i4 - %%i3)
    psrldq m%8, m%3, 2 * %%i3
    por m%8, m%2
    psubw m%7, m%3
    psubw m%8, m%3
    punpcklwd m%2, m%8, m%7
    punpckhwd m%8, m%7
    pshufd m%7, m%5, q1111
    pmaddwd m%2, m%7
    pmaddwd m%8, m%7
    paddd m%9, m%2
    paddd m%10, m%8

%if mmsize == 32
    vperm2i128 m%2, m%3, m%4, 0x21
    pslldq m%2, 16 - 2 * %%i2
%else
    pslldq m%2, m%4, 16 - 2 * %%i2
%endif
    psrldq m%7, m%3, 2 * %%i2
    por m%7, m%2
    pslldq m%2, 2 * (%%i2 - %%i1)
    psrldq m%8, m%3, 2 * %%i1
    por m%8, m%2
    psubw m%7, m%3
    psubw m%8, m%3
    punpcklwd m%2, m%8, m%7
    punpckhwd m%8, m%7
    pshufd m%7, m%5, q0000
    pmaddwd m%2, m%7
    pmaddwd m%8, m%7
    paddd m%9, m%2
    paddd m%10, m%8

    punpcklwd m%7, m%9, m%10
    punpckhwd m%9, m%10
    punpcklwd m%2, m%7, m%9
    punpckhwd m%7, m%9
    punpckhwd m%2, m%7
    paddw m%2, m%3
%endmacro

;------------------------------------------------------------------------------
; BLUR_HORZ_TILE 1:tile_order, 2:suffix, 3:pattern
; void blur****_horz_tile(int16_t *dst,
;                         const int16_t *side1, const int16_t *src, const int16_t *side2,
;                         void *param)
;------------------------------------------------------------------------------

%macro BLUR_HORZ_TILE 3
%if ARCH_X86_64
cglobal blur%3_horz_tile%2, 5,6,9
    mova m8, [dwords_round]
%else
cglobal blur%3_horz_tile%2, 5,6,8
%endif

    mov r5, [r4]
    movd xm3, r5d
%if ARCH_X86_64
    shr r5, 32
%else
    mov r5d, [r4 + 4]
%endif
    movd xm4, r5d
    punpckldq xm3, xm4
%if mmsize == 32
    vpbroadcastq m3, xm3
%endif

    xor r5d, r5d
.main_loop

    mova m0, [r1 + r5 + (2 << %1) - mmsize]
    mova m1, [r2 + r5]
    %assign %%i 0
%rep (2 << %1) / mmsize
%if %%i + mmsize == (2 << %1)
    mova m2, [r3 + r5]
%else
    mova m2, [r2 + r5 + %%i + mmsize]
%endif
%if ARCH_X86_64 == 0
    mova m7, [dwords_round]
%endif
    FILTER_LINE_HORZ %3, 0,1,2, 3,8, 4,5,6,7
    mova [r0 + r5 + %%i], m0
    SWAP 0, 1, 2
    %assign %%i %%i + mmsize
%endrep

    add r5d, 2 << %1
    cmp r5d, 2 << (2 * %1)
    jnz .main_loop
    RET
%endmacro

INIT_XMM sse2
BLUR_HORZ_TILE 4,16, 1234
BLUR_HORZ_TILE 5,32, 1234
INIT_YMM avx2
BLUR_HORZ_TILE 4,16, 1234
BLUR_HORZ_TILE 5,32, 1234
INIT_XMM sse2
BLUR_HORZ_TILE 4,16, 1235
BLUR_HORZ_TILE 5,32, 1235
INIT_YMM avx2
BLUR_HORZ_TILE 4,16, 1235
BLUR_HORZ_TILE 5,32, 1235
INIT_XMM sse2
BLUR_HORZ_TILE 4,16, 1246
BLUR_HORZ_TILE 5,32, 1246
INIT_YMM avx2
BLUR_HORZ_TILE 4,16, 1246
BLUR_HORZ_TILE 5,32, 1246

;------------------------------------------------------------------------------
; FILTER_LINE_VERT 1:n4, 2:n3, 3:n2, 4:n1, 5:z0, 6:p1, 7:p2, 8:p3, 9:p4,
;                  10:m_dst, 11:m_mul, 12:m_round, 13-16:m_tmp
; Apply generic 9-tap filter
;------------------------------------------------------------------------------

%macro FILTER_LINE_VERT 16
    mova m%13, %1
    psubw m%13, %5
    mova m%14, %2
    psubw m%14, %5
    punpcklwd m%15, m%14, m%13
    punpckhwd m%14, m%13
    pshufd m%13, m%11, q1111
    pmaddwd m%15, m%13
    pmaddwd m%14, m%13
    paddd m%16, m%12, m%15
    paddd m%10, m%12, m%14

    mova m%13, %3
    psubw m%13, %5
    mova m%14, %4
    psubw m%14, %5
    punpcklwd m%15, m%14, m%13
    punpckhwd m%14, m%13
    pshufd m%13, m%11, q0000
    pmaddwd m%15, m%13
    pmaddwd m%14, m%13
    paddd m%16, m%15
    paddd m%10, m%14

    mova m%13, %9
    psubw m%13, %5
    mova m%14, %8
    psubw m%14, %5
    punpcklwd m%15, m%14, m%13
    punpckhwd m%14, m%13
    pshufd m%13, m%11, q1111
    pmaddwd m%15, m%13
    pmaddwd m%14, m%13
    paddd m%16, m%15
    paddd m%10, m%14

    mova m%13, %7
    psubw m%13, %5
    mova m%14, %6
    psubw m%14, %5
    punpcklwd m%15, m%14, m%13
    punpckhwd m%14, m%13
    pshufd m%13, m%11, q0000
    pmaddwd m%15, m%13
    pmaddwd m%14, m%13
    paddd m%16, m%15
    paddd m%10, m%14

    punpcklwd m%13, m%16, m%10
    punpckhwd m%16, m%10
    punpcklwd m%10, m%13, m%16
    punpckhwd m%13, m%16
    punpckhwd m%10, m%13
    paddw m%10, %5
%endmacro

;------------------------------------------------------------------------------
; DEFINE_ARGUMENT 1:tile_order, 2:arg, 3:prev, 4:src, 5:next, 6:pos
; Choose corret tile depending on offset
;------------------------------------------------------------------------------

%macro DEFINE_ARGUMENT 6
%if (%6) < 0
    %define a%2 [%3 + (((%6) + (1 << %1)) << (1 + %1))]
%elif (%6) < (1 << %1)
    %define a%2 [%4 + (((%6) + (0 << %1)) << (1 + %1))]
%else
    %define a%2 [%5 + (((%6) - (1 << %1)) << (1 + %1))]
%endif
%endmacro

;------------------------------------------------------------------------------
; BLUR_VERT_TILE 1:tile_order, 2:suffix, 3:pattern
; void blur****_vert_tile(int16_t *dst,
;                         const int16_t *side1, const int16_t *src, const int16_t *side2,
;                         void *param)
;------------------------------------------------------------------------------

%macro BLUR_VERT_TILE 3
    %assign %%i1 %3 / 1000 % 10
    %assign %%i2 %3 / 100 % 10
    %assign %%i3 %3 / 10 % 10
    %assign %%i4 %3 / 1 % 10

cglobal blur%3_vert_tile%2, 5,6,8
    mov r5, [r4]
    movd xm1, r5d
%if ARCH_X86_64
    shr r5, 32
%else
    mov r5d, [r4 + 4]
%endif
    movd xm0, r5d
    punpckldq xm1, xm0
%if mmsize == 32
    vpbroadcastq m1, xm1
%endif
    mova m2, [dwords_round]

%if (2 << %1) > mmsize
    mov r5d, (2 << %1) / mmsize
.col_loop
%endif

    %assign %%i 0
%rep %%i4
    DEFINE_ARGUMENT %1, n4, r1,r2,r3, %%i - %%i4
    DEFINE_ARGUMENT %1, n3, r1,r2,r3, %%i - %%i3
    DEFINE_ARGUMENT %1, n2, r1,r2,r3, %%i - %%i2
    DEFINE_ARGUMENT %1, n1, r1,r2,r3, %%i - %%i1
    mova m7, [r2 + (%%i << (1 + %1))]
    DEFINE_ARGUMENT %1, p1, r1,r2,r3, %%i + %%i1
    DEFINE_ARGUMENT %1, p2, r1,r2,r3, %%i + %%i2
    DEFINE_ARGUMENT %1, p3, r1,r2,r3, %%i + %%i3
    DEFINE_ARGUMENT %1, p4, r1,r2,r3, %%i + %%i4
    FILTER_LINE_VERT an4,an3,an2,an1,m7,ap1,ap2,ap3,ap4, 0,1,2, 3,4,5,6
    mova [r0 + (%%i << (1 + %1))], m0
    %assign %%i %%i + 1
%endrep

    mov r4d, %%i4 << (1 + %1)
.row_loop
    %define an4 [r2 + r4 - (%%i4 << (1 + %1))]
    %define an3 [r2 + r4 - (%%i3 << (1 + %1))]
    %define an2 [r2 + r4 - (%%i2 << (1 + %1))]
    %define an1 [r2 + r4 - (%%i1 << (1 + %1))]
    mova m7, [r2 + r4]
    %define ap1 [r2 + r4 + (%%i1 << (1 + %1))]
    %define ap2 [r2 + r4 + (%%i2 << (1 + %1))]
    %define ap3 [r2 + r4 + (%%i3 << (1 + %1))]
    %define ap4 [r2 + r4 + (%%i4 << (1 + %1))]
    FILTER_LINE_VERT an4,an3,an2,an1,m7,ap1,ap2,ap3,ap4, 0,1,2, 3,4,5,6
    mova [r0 + r4], m0
    add r4d, 2 << %1
    cmp r4d, ((1 << %1) - %%i4) << (1 + %1)
    jl .row_loop

    %assign %%i (1 << %1) - %%i4
%rep %%i4
    DEFINE_ARGUMENT %1, n4, r1,r2,r3, %%i - %%i4
    DEFINE_ARGUMENT %1, n3, r1,r2,r3, %%i - %%i3
    DEFINE_ARGUMENT %1, n2, r1,r2,r3, %%i - %%i2
    DEFINE_ARGUMENT %1, n1, r1,r2,r3, %%i - %%i1
    mova m7, [r2 + (%%i << (1 + %1))]
    DEFINE_ARGUMENT %1, p1, r1,r2,r3, %%i + %%i1
    DEFINE_ARGUMENT %1, p2, r1,r2,r3, %%i + %%i2
    DEFINE_ARGUMENT %1, p3, r1,r2,r3, %%i + %%i3
    DEFINE_ARGUMENT %1, p4, r1,r2,r3, %%i + %%i4
    FILTER_LINE_VERT an4,an3,an2,an1,m7,ap1,ap2,ap3,ap4, 0,1,2, 3,4,5,6
    mova [r0 + (%%i << (1 + %1))], m0
    %assign %%i %%i + 1
%endrep

%if (2 << %1) > mmsize
    add r0, mmsize
    add r1, mmsize
    add r2, mmsize
    add r3, mmsize
    sub r5d, 1
    jnz .col_loop
%endif
    RET
%endmacro

INIT_XMM sse2
BLUR_VERT_TILE 4,16, 1234
BLUR_VERT_TILE 5,32, 1234
INIT_YMM avx2
BLUR_VERT_TILE 4,16, 1234
BLUR_VERT_TILE 5,32, 1234
INIT_XMM sse2
BLUR_VERT_TILE 4,16, 1235
BLUR_VERT_TILE 5,32, 1235
INIT_YMM avx2
BLUR_VERT_TILE 4,16, 1235
BLUR_VERT_TILE 5,32, 1235
INIT_XMM sse2
BLUR_VERT_TILE 4,16, 1246
BLUR_VERT_TILE 5,32, 1246
INIT_YMM avx2
BLUR_VERT_TILE 4,16, 1246
BLUR_VERT_TILE 5,32, 1246
