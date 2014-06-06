;******************************************************************************
;* shift.asm: SSE2/AVX2 tile shift
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
words_full: dw 16384,16384,16384,16384,16384,16384,16384,16384
            dw 16384,16384,16384,16384,16384,16384,16384,16384

SECTION .text

;------------------------------------------------------------------------------
; SCALE 1:m_reg, 2:m_mul, 3:m_tmp
;------------------------------------------------------------------------------

%macro SCALE 3
%if cpuflag(ssse3)
    pmulhrsw m%1, m%2
%else
    pmullw m%3, m%1, m%2
    pmulhw m%1, m%2
    psrlw m%3, 14
    psllw m%1, 2
    paddw m%1, m%3
    paddw m%1, mm_one
    psraw m%1, 1
%endif
%endmacro

;------------------------------------------------------------------------------
; SHIFT_LINE 1:m_dst/m_src1, 2:m_src2, 3:m_mul, 4-5:m_tmp, 6:shift
; Calculate shifted vector
;------------------------------------------------------------------------------

%macro SHIFT_LINE 6
    %assign %%shift ((%6) & (mmsize - 1)) / 2
%if mmsize == 32 && %%shift < 8

    vperm2i128 m%4, m%1, m%2, 0x21
%if %%shift
    psrldq m%1, 2 * %%shift
%endif
%if %%shift < 7
    pslldq m%4, 14 - 2 * %%shift
    psrldq m%5, m%1, 2
    por m%4, m%5
%endif
%if %%shift
    pslldq m%5, m%4, 2
    por m%1, m%5
    psubw m%4, m%1
%endif

%else

    %assign %%shift %%shift & 7
%if mmsize == 32
    vperm2i128 m%1, m%1, m%2, 0x21
%endif
%if %%shift
    psrldq m%1, 2 * %%shift
%endif
%if %%shift < 7
    psrldq m%4, m%1, 2
%endif
%if %%shift
    pslldq m%5, m%2, 16 - 2 * %%shift
    por m%1, m%5
%endif
%if %%shift < 7
    pslldq m%5, m%2, 14 - 2 * %%shift
    por m%4, m%5
    psubw m%4, m%1
%else
    psubw m%4, m%2, m%1
%endif

%endif
    SCALE %4, %3, %5
    paddw m%1, m%4
%endmacro

;------------------------------------------------------------------------------
; LOAD_FROM_PAIR 1:tile_order, 2:m_dst, 3:src1, 4:src2, 5:offs, 6:shift
; Load vector from tile pair
;------------------------------------------------------------------------------

%macro LOAD_FROM_PAIR 6
    %assign %%shift (%6) & ~(mmsize - 1)
%if %%shift < (2 << %1)
    mova m%2, [%3 + %5 + %%shift]
%else
    mova m%2, [%4 + %5 + %%shift - (2 << %1)]
%endif
%endmacro

;------------------------------------------------------------------------------
; SHIFT_TILE 1:tile_order, 2:suffix, 3:code_chunk_size
; int shift_tile%2(int16_t *dst,
;                  const int16_t *src0, const int16_t *src1,
;                  const int16_t *src2, const int16_t *src3,
;                  int dx, int dy);
;------------------------------------------------------------------------------

%macro SHIFT_TILE 3
    %assign %%n (2 << %1) / mmsize
%if ARCH_X86_64 == 0
    %assign %%alloc_size 2 << %1
cglobal shift_tile%2, 3,6,8, %%alloc_size
%elif cpuflag(ssse3)
cglobal shift_tile%2, 7,8,%%n + 8
%else
cglobal shift_tile%2, 7,8,%%n + 9
%endif

    pxor m6, m6
    mova m7, [words_full]
%if ARCH_X86_64 == 0
    %define mm_one [words_one]
    mov r3d, r5m
    mov r4d, r6m
    DECLARE_REG_TMP 3,4,5
%elif cpuflag(ssse3) == 0
    %assign %%reg %%n + 8
    mova m %+ %%reg, [words_one]
    %xdefine mm_one m %+ %%reg
    DECLARE_REG_TMP 5,6,7
%endif
    mov t2d, t0d
    shr t0d, 6  ; x_shift
    and t2d, 63
    shl t2d, 9
    BCASTW 2, t2d  ; x_mul
    mov t2d, t1d
    shr t1d, 6  ; y_shift
    and t2d, 63
    shl t2d, 9
    BCASTW 3, t2d  ; y_mul
    inc t1d
    shl t1d, 1 + %1
    add r1, t1
    add r2, t1
%if ARCH_X86_64
    lea r3, [r3 + t1 - (2 << (2 * %1))]
    lea r4, [r4 + t1 - (2 << (2 * %1))]
%else
    mov t2, r3m
    lea r2, [r2 + t1 - (2 << (2 * %1))]
    mov r3m, t2
    mov t2, r4m
    lea r2, [r2 + t1 - (2 << (2 * %1))]
    mov r4m, t2
%endif
    mov t2d, 2 << (2 * %1)
    sub t2d, t1d

    imul t0d, %3
    lea t0, [.start0 + t0]  ; XXX: make rip relative
    jmp t0

.finalize
    xor r1d, r1d
    pxor m0, m0
    pcmpeqw m0, m7
    pmovmskb eax, m0
    add eax, 1
    cmovnz eax, r1d
    mova m0, [words_full]
    pcmpeqw m0, m6
    pmovmskb r0d, m0
    add r0d, 1
    cmovnz r0d, r1d
    sub eax, r0d
    RET

    times (($$ - $) & 63) nop
    %assign %%shift 0
%rep 1 << %1
    .start %+ %%shift equ $

%if ARCH_X86_64
    %assign %%reg %%n + 8
    SWAP 0, %%reg
    LOAD_FROM_PAIR %1, 8,r1,r2,-(2 << %1), %%shift
    %assign %%i 0
%rep %%n
    LOAD_FROM_PAIR %1, 9,r1,r2,-(2 << %1), %%shift + %%i + mmsize
    SHIFT_LINE 8,9,2, 4,5, %%shift
    SWAP 8, 9, 10, 11, 12
    %assign %%i %%i + mmsize
%endrep
    RESET_MM_PERMUTATION
%else
    LOAD_FROM_PAIR %1, 0,r1,r2,-(2 << %1), %%shift
    %assign %%i 0
%rep %%n
    LOAD_FROM_PAIR %1, 1,r1,r2,-(2 << %1), %%shift + %%i + mmsize
    SHIFT_LINE 0,1,2, 4,5, %%shift
    mova [rsp + %%i], m0
    SWAP 0, 1
    %assign %%i %%i + mmsize
%endrep
%endif
    RESET_MM_PERMUTATION

    xor t0d, t0d
.row_loop %+ %%shift
    cmp t0d, t2d
%if ARCH_X86_64
    cmove r1, r3
    cmove r2, r4
%else
    cmove r1, r3m
    cmove r2, r4m
%endif
    LOAD_FROM_PAIR %1, 0,r1,r2,t0, %%shift
    %assign %%i 0
%rep %%n
    LOAD_FROM_PAIR %1, 1,r1,r2,t0, %%shift + %%i + mmsize
    SHIFT_LINE 0,1,2, 4,5, %%shift
%if ARCH_X86_64
    %assign %%reg %%i / mmsize + 8
    psubw m4, m0, m %+ %%reg
    SCALE 4,3, 5
    paddw m4, m %+ %%reg
    mova m %+ %%reg, m0
    mova [r0 + t0 + %%i], m4
    pminsw m6, m4
    pmaxsw m7, m4
%else
    mova m4, [rsp + %%i]
    mova [rsp + %%i], m0
    psubw m0, m4
    SCALE 0,3, 5
    paddw m0, m4
    mova [r0 + t0 + %%i], m0
    pminsw m6, m0
    pmaxsw m7, m0
%endif
    SWAP 0, 1
    %assign %%i %%i + mmsize
%endrep
    RESET_MM_PERMUTATION

    add t0d, 2 << %1
    cmp t0d, 2 << (2 * %1)
    jl .row_loop %+ %%shift
    jmp .finalize

    .delta %+ %%shift equ $ - .start %+ %%shift
%if %%shift < (2 << %1) - 2
    times (%3 - .delta %+ %%shift) nop
%endif
    %assign %%shift %%shift + 2
%endrep
%endmacro

%if ARCH_X86_64
INIT_XMM sse2
SHIFT_TILE 4,16,  9 * 64
SHIFT_TILE 5,32, 17 * 64
INIT_YMM avx2
SHIFT_TILE 4,16,  3 * 64
SHIFT_TILE 5,32,  6 * 64
%else
INIT_XMM sse2
SHIFT_TILE 4,16,  9 * 64
SHIFT_TILE 5,32, 18 * 64
INIT_YMM avx2
SHIFT_TILE 4,16,  3 * 64
SHIFT_TILE 5,32,  6 * 64
%endif
