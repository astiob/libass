;******************************************************************************
;* tile_func.asm: SSE2/AVX2 tile functions
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

%include "x86inc.asm"

%if ARCH_X86_64
DEFAULT REL
%endif

SECTION_RODATA 32

words_index: dw 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
words_tile16: dw 1024,1024,1024,1024,1024,1024,1024,1024,1024,1024,1024,1024,1024,1024,1024,1024
words_tile32: dw 512,512,512,512,512,512,512,512,512,512,512,512,512,512,512,512
words_max: dw 256,256,256,256,256,256,256,256,256,256,256,256,256,256,256,256
words_one: dw 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1

SECTION .text

;------------------------------------------------------------------------------
; MUL reg, num
; Multiply by constant
;------------------------------------------------------------------------------

%macro MUL 2
%if (%2) == 0
    xor %1, %1
%elif (%2) == 1
%elif (%2) == 2
    add %1, %1  ; lea %1, [%1 + %1]
%elif (%2) == 3
    lea %1, [%1 + 2 * %1]
%elif (%2) == 4
    lea %1, [4 * %1]  ; shl %1, 2
%elif (%2) == 5
    lea %1, [%1 + 4 * %1]
%elif (%2) == 8
    lea %1, [8 * %1]  ; shl %1, 3
%elif (%2) == 9
    lea %1, [%1 + 8 * %1]
%elif (%2) == 16
    shl %1, 4
%elif (%2) == 32
    shl %1, 5
%elif (%2) == 64
    shl %1, 6
%elif (%2) == 128
    shl %1, 7
%elif (%2) == 256
    shl %1, 8
%else
    imul %1, %2
%endif
%endmacro

;------------------------------------------------------------------------------
; BCASTW m_dst, r_src
;------------------------------------------------------------------------------

%macro BCASTW 2
    movd xm%1, %2
%if mmsize == 32
    vpbroadcastw m%1, xm%1
%elif mmsize == 16
    punpcklwd m%1, m%1
    pshufd m%1, m%1, q0000
%endif
%endmacro

;------------------------------------------------------------------------------
; PABSW m_reg, m_tmp
;------------------------------------------------------------------------------

%macro PABSW 2
%if cpuflag(ssse3)
    pabsw m%1, m%1
%else
    pxor m%2, m%2
    psubw m%2, m%1
    pmaxsw m%1, m%2
%endif
%endmacro

;------------------------------------------------------------------------------
; FINALIZE_SOLID
; void finalize_solid(uint8_t *buf, ptrdiff_t stride, int size_order, int set);
;------------------------------------------------------------------------------

%macro FINALIZE_SOLID 0
%if ARCH_X86_64 == 0
cglobal finalize_solid, 0,5,1
    DECLARE_REG_TMP 0,2,1,3
    mov t0, r0m
    mov t1, r1m
    mov t2, r2m
    mov t3, r3m
%elif WIN64
cglobal finalize_solid, 4,5,1
    DECLARE_REG_TMP 2,1,0,3
    xchg t0, t2
%else
cglobal finalize_solid, 4,5,1
    DECLARE_REG_TMP 0,1,3,2
    xchg t3, t2
%endif

    mov r4d, -1
    test t3d, t3d
    cmovnz t3d, r4d
    movd xm0, t3d
%if mmsize == 32
    vpbroadcastd m0, xm0
%else
    pshufd m0, m0, q0000
%endif

%if mmsize == 32
    cmp t2d, 5
    jl .tile16
%endif
    mov t3d, 1
    shl t3d, t2b

    sub t1, t3
    mov t2d, t3d
.row_loop
    mov r4d, t3d
.col_loop
    mova [t0], m0
    add t0, mmsize
    sub r4d, mmsize
    jnz .col_loop
    add t0, t1
    sub t2d, 1
    jnz .row_loop
    RET

%if mmsize == 32
.tile16
%rep 15
    mova [t0], xm0
    add t0, t1
%endrep
    mova [t0], xm0
    RET
%endif
%endmacro

INIT_XMM sse2
FINALIZE_SOLID
INIT_YMM avx2
FINALIZE_SOLID

;------------------------------------------------------------------------------
; FINALIZE_GENERIC_TILE tile_order, suffix
; void finalize_generic_tile%2(uint8_t *buf, ptrdiff_t stride,
;                              const int16_t *src);
;------------------------------------------------------------------------------

%macro FINALIZE_GENERIC_TILE 2
cglobal finalize_generic_tile%2, 3,4,2
    mov r3, 1 << %1
.main_loop
%assign %%i 0
%rep (1 << %1) / mmsize
    mova m0, [r2 + 2 * %%i]
    psrlw m0, 6
    mova m1, [r2 + 2 * %%i + mmsize]
    psrlw m1, 6
    packuswb m0, m1
%if mmsize == 32
    vpermq m0, m0, q3120
%endif
    mova [r0 + %%i], m0
%assign %%i %%i + mmsize
%endrep
%if (1 << %1) < mmsize
    mova m0, [r2 + 2 * %%i]
    psrlw m0, 6
    packuswb m0, m0
    vpermq m0, m0, q3120
    mova [r0 + %%i], xm0
%endif
    add r0, r1
    add r2, 2 << %1
    sub r3, 1
    jnz .main_loop
    RET
%endmacro

INIT_XMM sse2
FINALIZE_GENERIC_TILE 4,16
FINALIZE_GENERIC_TILE 5,32
INIT_YMM avx2
FINALIZE_GENERIC_TILE 4,16
FINALIZE_GENERIC_TILE 5,32

;------------------------------------------------------------------------------
; CALC_LINE m_dst, m_src, m_delta, m_zero, m_full, m_tmp, shift
; Calculate line using antialiased halfplane algorithm
;------------------------------------------------------------------------------

%macro CALC_LINE 7
    paddw m%6, m%2, m%3
    pmaxsw m%1, m%2, m%4
    pmaxsw m%6, m%4
    pminsw m%1, m%5
    pminsw m%6, m%5
    paddw m%1, m%6
%if (%7) < 0
    psllw m%1, -(%7)
%elif (%7) > 0
    psraw m%1, %7
%endif
%endmacro

;------------------------------------------------------------------------------
; DEF_A_SHIFT tile_order
; If single mm-register is enough to store the whole line
; then sets a_shift = 0,
; else sets a_shift = log2(mmsize / sizeof(int16_t)).
;------------------------------------------------------------------------------

%macro DEF_A_SHIFT 1
%if mmsize >= (2 << %1)
    %define a_shift 0
%elif mmsize == 32
    %define a_shift 4
%elif mmsize == 16
    %define a_shift 3
%else
    %error "invalid mmsize"
%endif
%endmacro

;------------------------------------------------------------------------------
; FILL_HALFPLANE_TILE tile_order, suffix
; void fill_halfplane_tile%2(uint16_t *buf,
;                            int32_t a, int32_t b, int64_t c, int32_t scale);
;------------------------------------------------------------------------------

%macro FILL_HALFPLANE_TILE 2
    DEF_A_SHIFT %1
%assign %%n 8
%if a_shift == 0
    SWAP 3, 7
%assign %%n 7
%endif

%if ARCH_X86_64
cglobal fill_halfplane_tile%2, 5,6,%%n
%else
cglobal fill_halfplane_tile%2, 0,6,%%n
%endif

%if ARCH_X86_64
    movsxd r1, r1d  ; a
    movsxd r2, r2d  ; b
    sar r3, 7 + %1  ; c >> (tile_order + 7)
    movsxd r4, r4d  ; scale
    mov r5, 1 << (45 + %1)
    imul r1, r4
    add r1, r5
    sar r1, 46 + %1  ; aa
    imul r2, r4
    add r2, r5
    sar r2, 46 + %1  ; bb
    imul r3, r4
    shr r5, 1 + %1
    add r3, r5
    sar r3, 45  ; cc

    DECLARE_REG_TMP 4,5, 1,2,3  ; t2=aa t3=bb t4=cc
%else
    mov r0d, r3m  ; c_lo
    mov r2d, r4m  ; c_hi
    mov r1d, r5m  ; scale
    mov r5d, 1 << 12
    shr r0d, 7 + %1
    shl r2d, 25 - %1
    or r0d, r2d  ; r0d (eax) = c >> (tile_order + 7)
    imul r1d  ; r2d (edx) = (c >> ...) * scale >> 32
    add r2d, r5d
    sar r2d, 13
    mov r4d, r2d  ; cc
    shl r5d, 1 + %1
    mov r0d, r2m  ; r0d (eax) = b
    imul r1d  ; r2d (edx) = b * scale >> 32
    add r2d, r5d
    sar r2d, 14 + %1
    mov r3d, r2d  ; bb
    mov r0d, r1m  ; r0d (eax) = a
    imul r1d  ; r2d (edx) = a * scale >> 32
    add r2d, r5d
    sar r2d, 14 + %1  ; aa

    DECLARE_REG_TMP 1,5, 2,3,4  ; t2=aa t3=bb t4=cc
    mov r0d, r0m
%endif
    add t4d, 1 << (13 - %1)
    mov t1d, t2d
    add t1d, t3d
    sar t1d, 1
    sub t4d, t1d

    BCASTW 1, t4d  ; cc
    BCASTW 2, t2d  ; aa
%if a_shift
    psllw m3, m2, a_shift  ; aa * (mmsize / 2)
%endif
    pmullw m2, [words_index]
    psubw m1, m2  ; cc - aa * i

    mov t4d, t2d  ; aa
    mov t1d, t4d
    sar t1d, 31
    xor t4d, t1d
    sub t4d, t1d  ; abs_a
    mov t0d, t3d  ; bb
    mov t1d, t0d
    sar t1d, 31
    xor t0d, t1d
    sub t0d, t1d  ; abs_b
    cmp t4d, t0d
    cmovg t4d, t0d
    add t4d, 2
    sar t4d, 2  ; delta
    BCASTW 2, t4d
    psubw m1, m2  ; c1 = cc - aa * i - delta
    paddw m2, m2  ; 2 * delta

%if a_shift
    MUL t2d, (1 << %1) - (mmsize / 2)
    sub t3d, t2d  ; bb - (tile_size - mmsize / 2) * aa
%endif
    BCASTW 7, t3d

    pxor m0, m0
    mova m4, [words_tile%2]
    mov t0d, 1 << %1
    jmp .loop_entry

.loop_start
    add r0, 2 << %1
    psubw m1, m7
.loop_entry
%assign %%i 0
%rep (2 << %1) / mmsize
%if %%i
    psubw m1, m3
%endif
    CALC_LINE 5, 1,2, 0,4, 6, 1 - %1
    mova [r0 + %%i], m5
%assign %%i %%i + mmsize
%endrep
    sub t0d,1
    jnz .loop_start
    RET
%endmacro

INIT_XMM sse2
FILL_HALFPLANE_TILE 4,16
FILL_HALFPLANE_TILE 5,32
INIT_YMM avx2
FILL_HALFPLANE_TILE 4,16
FILL_HALFPLANE_TILE 5,32

;------------------------------------------------------------------------------
; struct segment {
;     int64_t c;
;     int32_t a, b, scale, flags;
;     int32_t x_min, x_max, y_min, y_max;
; };
;------------------------------------------------------------------------------

struc line
    .c: resq 1
    .a: resd 1
    .b: resd 1
    .scale: resd 1
    .flags: resd 1
    .x_min: resd 1
    .x_max: resd 1
    .y_min: resd 1
    .y_max: resd 1
endstruc

;------------------------------------------------------------------------------
; ZEROFILL dst, size, tmp1
;------------------------------------------------------------------------------

%macro ZEROFILL 3
%assign %%n 128 / mmsize
    mov %3, (%2) / 128
%%zerofill_loop:
%assign %%i 0
%rep %%n
    mova [%1 + %%i], mm_zero
%assign %%i %%i + mmsize
%endrep
    add %1, 128
    sub %3, 1
    jnz %%zerofill_loop
%assign %%i 0
%rep ((%2) / mmsize) & (%%n - 1)
    mova [%1 + %%i], mm_zero
%assign %%i %%i + mmsize
%endrep
%endmacro

;------------------------------------------------------------------------------
; CALC_DELTA_FLAG res, line, tmp1, tmp2
; Set bits of result register (res):
; bit 3 - for nonzero up_delta,
; bit 2 - for nonzero dn_delta.
;------------------------------------------------------------------------------

%macro CALC_DELTA_FLAG 4
    mov %3d, [%2 + line.flags]
    xor %4d, %4d
    cmp %4d, [%2 + line.x_min]
    cmovz %4d, %3d
    xor %1d, %1d
    test %3d, 2  ; SEGFLAG_UL_DR
    cmovnz %1d, %4d
    shl %3d, 2
    xor %1d, %3d
    and %4d, 4
    and %1d, 4
    lea %1d, [%1d + 2 * %1d]
    xor %1d, %4d
%endmacro

;------------------------------------------------------------------------------
; UPDATE_DELTA dn/up, dst, flag, pos, tmp
; Update delta array
;------------------------------------------------------------------------------

%macro UPDATE_DELTA 5
%ifidn %1, dn
    %define %%op add
    %define %%opi sub
    %assign %%flag 1 << 2
%elifidn %1, up
    %define %%op sub
    %define %%opi add
    %assign %%flag 1 << 3
%else
    %error "dn/up expected"
%endif

    test %3d, %%flag
    jz %%skip
    lea %5d, [4 * %4d - 256]
    %%opi [%2], %5w
    lea %5d, [4 * %4d]
    %%op [%2 + 2], %5w
%%skip:
%endmacro

;------------------------------------------------------------------------------
; CALC_VBA tile_order, b
; Calculate b - (tile_size - (mmsize / sizeof(int16_t))) * a
;------------------------------------------------------------------------------

%macro CALC_VBA 2
    BCASTW m_vba, %2d
%rep (2 << %1) / mmsize - 1
    psubw mm_vba, mm_van
%endrep
%endmacro

;------------------------------------------------------------------------------
; FILL_BORDER_LINE tile_order, res, abs_a(abs_ab), b, [abs_b], size, sum,
;                  tmp8, tmp9, mt10, mt11, mt12, mt13, mt14, [mt15]
; Render top/bottom line of the trapezium with antialiasing
;------------------------------------------------------------------------------

%macro FILL_BORDER_LINE 15
    mov %8d, %6d
    shl %8d, 8 - %1  ; size << (8 - tile_order)
    xor %9d, %9d
%if ARCH_X86_64
    sub %8d, %3d  ; abs_a
    cmovg %8d, %9d
    add %8d, 1 << (14 - %1)
    shl %8d, 2 * %1 - 5  ; w
    BCASTW %15, %8d

    mov %9d, %5d  ; abs_b
    imul %9d, %6d
    sar %9d, 6  ; dc_b
    cmp %9d, %3d  ; abs_a
    cmovg %9d, %3d
%else
    sub %8w, %3w  ; abs_a
    cmovg %8d, %9d
    add %8w, 1 << (14 - %1)
    shl %8d, 2 * %1 - 5  ; w

    mov %9d, %3d  ; abs_ab
    shr %9d, 16  ; abs_b
    imul %9d, %6d
    sar %9d, 6  ; dc_b
    cmp %9w, %3w
    cmovg %9w, %3w
%endif
    add %9d, 2
    sar %9d, 2  ; dc

    imul %7d, %4d  ; sum * b
    sar %7d, 7  ; avg * b
    add %7d, %9d  ; avg * b + dc
    add %9d, %9d  ; 2 * dc

    imul %7d, %8d
    sar %7d, 16
    sub %7d, %6d  ; -offs1
    BCASTW %10, %7d
    imul %9d, %8d
    sar %9d, 16  ; offs2 - offs1
    BCASTW %11, %9d
    add %6d, %6d
    BCASTW %12, %6d

%assign %%i 0
%rep (2 << %1) / mmsize
%if %%i
    psubw mm_c, mm_van
%endif
%if ARCH_X86_64
    pmulhw m%13, mm_c, m%15
%else
    BCASTW %14, %8d
    pmulhw m%13, mm_c, m%14
%endif
    psubw m%13, m%10  ; c1
    paddw m%14, m%13, m%11  ; c2
    pmaxsw m%13, mm_zero
    pmaxsw m%14, mm_zero
    pminsw m%13, m%12
    pminsw m%14, m%12
    paddw m%13, m%14
    paddw m%13, [%2 + %%i]
    mova [%2 + %%i], m%13
%assign %%i %%i + mmsize
%endrep
%endmacro

;------------------------------------------------------------------------------
; SAVE_RESULT tile_order, buf, delta, m_max,
;             tmp5, tmp6, mt7, mt8, mt9
; Apply delta and rescale rusult
;------------------------------------------------------------------------------

%macro SAVE_RESULT 9
    mov %5d, 1 << %1
    xor %6d, %6d
%%save_loop:
    add %6w, [%3]
    BCASTW %8, %6d
    add %3, 2
%assign %%i 0
%rep (2 << %1) / mmsize
    paddw m%7, m%8, [%2 + %%i]
    PABSW %7, %9
    pminsw m%7, m%4
    psllw m%7, 6
    mova [%2 + %%i], m%7
%assign %%i %%i + mmsize
%endrep
    add %2, 2 << %1
    sub %5d, 1
    jnz %%save_loop
%endmacro

;------------------------------------------------------------------------------
; FILL_GENERIC_TILE tile_order, suffix
; void fill_generic_tile%2(uint16_t *buf,
;                          const struct segment *line, size_t n_lines,
;                          int winding);
;------------------------------------------------------------------------------

%macro FILL_GENERIC_TILE 2
    ; t3=line t4=up/cur t5=dn/end t6=dn_pos t7=up_pos
    ; t8=a/abs_a/abs_ab t9=b t10=c/abs_b
%if ARCH_X86_64
    DECLARE_REG_TMP 9,10,4,1, 3,8,5,6, 7,11,12
%else
    DECLARE_REG_TMP 0,1,5,3, 4,6,6,0, 2,3,5
%endif

    %assign tile_size 1 << %1
    %assign alloc_size 2 * tile_size + 4
    DEF_A_SHIFT %1

%if ARCH_X86_64
    %define m_zero  6
    %define m_full  7
    %define mm_index m8
    %define m_c     9
    %define m_vba   10
%if a_shift
    %define m_van   11
cglobal fill_generic_tile%2, 4,13,12
%else
cglobal fill_generic_tile%2, 4,13,11
%endif

%else
    %define m_zero  5
    %define m_full  4  ; tmp
    %define mm_index [words_index]
    %define m_c     7
%if a_shift
    %define m_van   6
    %define m_vba   3  ; tmp
%else
    %define m_vba   6
%endif

    %assign alloc_size alloc_size + 8
cglobal fill_generic_tile%2, 0,7,8
%endif

    %define mm_zero  m %+ m_zero
    %define mm_full  m %+ m_full
    %define mm_c     m %+ m_c
    %define mm_vba   m %+ m_vba
%if a_shift
    %define mm_van   m %+ m_van
%endif

    SUB rstk, alloc_size

%if ARCH_X86_64
    mov t0, r0
%else
    mov t0, r0m
%endif
    pxor mm_zero, mm_zero
    ZEROFILL t0, 2 * tile_size * tile_size, t1
%assign %%i 0
%rep 2 * tile_size / mmsize
    movu [rstk + %%i], mm_zero
%assign %%i %%i + mmsize
%endrep

%if ARCH_X86_64 == 0
    mov r3d, r3m
%endif
    shl r3d, 8
    mov [rstk], r3w

%if ARCH_X86_64
    mova mm_index, [words_index]
    mova mm_full, [words_tile%2]
    %define dn_addr t5
%else
    %define dn_addr [rstk + 2 * tile_size + 4]
    %define dn_pos [rstk + 2 * tile_size + 8]
%endif

.line_loop
%if ARCH_X86_64 == 0
    mov t3, r1m
    lea t0, [t3 + line_size]
    mov r1m, t0
%endif
    CALC_DELTA_FLAG t0, t3, t1,t2

    mov t4d, [t3 + line.y_min]
    mov t2d, [t3 + line.y_max]
%if ARCH_X86_64
    mov t8d, t4d
    mov t6d, t4d
    and t6d, 63  ; up_pos
    shr t4d, 6  ; up
    mov t5d, t2d
    mov t7d, t2d
    and t7d, 63  ; dn_pos
    shr t5d, 6  ; dn

    UPDATE_DELTA up, rstk + 2 * t4, t0,t6, t1
    UPDATE_DELTA dn, rstk + 2 * t5, t0,t7, t1
    cmp t8d, t2d
%else
    lea t1d, [t0d + 1]
    cmp t4d, t2d
    cmovnz t0d, t1d  ; bit 0 -- not horz line

    mov t6d, t2d
    and t6d, 63  ; dn_pos
    shr t2d, 6  ; dn
    UPDATE_DELTA dn, rstk + 2 * t2, t0,t6, t1

    shl t2d, 1 + %1
    add t2, r0m
    mov dn_addr, t2
    mov dn_pos, t6d

    mov t6d, t4d
    and t6d, 63  ; up_pos
    shr t4d, 6  ; up
    UPDATE_DELTA up, rstk + 2 * t4, t0,t6, t1
    test t0d, 1
%endif
    jz .end_line_loop

%if ARCH_X86_64
    movsxd t8, dword [t3 + line.a]
    movsxd t9, dword [t3 + line.b]
    mov t10, [t3 + line.c]
    sar t10, 7 + %1  ; c >> (tile_order + 7)
    movsxd t0, dword [t3 + line.scale]
    mov t1, 1 << (45 + %1)
    imul t8, t0
    add t8, t1
    sar t8, 46 + %1  ; a
    imul t9, t0
    add t9, t1
    sar t9, 46 + %1  ; b
    imul t10, t0
    shr t1, 1 + %1
    add t10, t1
    sar t10, 45  ; c
%else
    mov r0d, [t3 + line.c]
    mov r2d, [t3 + line.c + 4]
    mov r1d, [t3 + line.scale]
    shr r0d, 7 + %1
    shl r2d, 25 - %1
    or r0d, r2d  ; r0d (eax) = c >> (tile_order + 7)
    imul r1d  ; r2d (edx) = (c >> ...) * scale >> 32
    add r2d, 1 << 12
    sar r2d, 13
    mov t10d, r2d  ; c
    mov r0d, [t3 + line.b]  ; r0d (eax)
    imul r1d  ; r2d (edx) = b * scale >> 32
    add r2d, 1 << (13 + %1)
    sar r2d, 14 + %1
    mov r0d, [t3 + line.a]  ; r0d (eax)
    mov t9d, r2d  ; b (overrides t3)
    imul r1d  ; r2d (edx) = a * scale >> 32
    add r2d, 1 << (13 + %1)
    sar r2d, 14 + %1  ; a (t8d)
%endif

    mov t0d, t8d  ; a
    sar t0d, 1
    sub t10d, t0d
    mov t0d, t9d  ; b
    imul t0d, t4d
    sub t10d, t0d
    BCASTW m_c, t10d

    BCASTW 0, t8d
%if a_shift
    psllw mm_van, m0, a_shift  ; a * (mmsize / 2)
%endif
    pmullw m0, mm_index
    psubw mm_c, m0  ; c - a * i

    mov t0d, t8d  ; a
    sar t0d, 31
    xor t8d, t0d
    sub t8d, t0d  ; abs_a
    mov t0d, t9d  ; b
    mov t10d, t9d
    sar t0d, 31
    xor t10d, t0d
    sub t10d, t0d  ; abs_b
%if ARCH_X86_64 == 0
    shl t10d, 16
    or t8d, t10d  ; abs_ab
%endif

    shl t4d, 1 + %1
%if ARCH_X86_64
    add t4, r0
    shl t5d, 1 + %1
    add t5, r0
%else
    add t4, r0m
%endif
    cmp t4, dn_addr
    jz .single_line

%if ARCH_X86_64 || a_shift == 0
    CALC_VBA %1, t9
%endif

    test t6d, t6d
    jz .generic_fist
    mov t2d, 64
    sub t2d, t6d  ; 64 - up_pos
    add t6d, 64  ; 64 + up_pos
    FILL_BORDER_LINE %1, t4,t8,t9,t10,t2,t6, t0,t1, 0,1,2,3,4,5

%if ARCH_X86_64 == 0
    mov t5, dn_addr
%if a_shift
    CALC_VBA %1, t9
%endif
%endif

    psubw mm_c, mm_vba
    add t4, 2 << %1
    cmp t4, t5
    jge .end_loop
%if ARCH_X86_64 == 0
    jmp .bulk_fill
%endif

.generic_fist
%if ARCH_X86_64 == 0
    mov t5, dn_addr
%if a_shift
    CALC_VBA %1, t9
%endif
%endif

.bulk_fill
    mov t2d, 1 << (13 - %1)
    mov t0d, t9d  ; b
    sar t0d, 1
    sub t2d, t0d  ; base
%if ARCH_X86_64
    mov t0d, t10d  ; abs_b
    cmp t0d, t8d  ; abs_a
    cmovg t0d, t8d
%else
    mov t0d, t8d  ; abs_ab
    shr t0d, 16  ; abs_b
    cmp t0w, t8w
    cmovg t0w, t8w
%endif
    add t0d, 2
    sar t0d, 2  ; dc
%if ARCH_X86_64
    sub t2d, t0d  ; base - dc
%else
    sub t2w, t0w  ; base - dc
%endif
    add t0d, t0d  ; 2 * dc
    BCASTW 2, t0d

%if ARCH_X86_64
    BCASTW 3, t2d
    paddw mm_c, m3
%else
    BCASTW 0, t2d
    paddw mm_c, m0

    mova mm_full, [words_tile%2]
%endif
.internal_loop
%assign i 0
%rep (2 << %1) / mmsize
%if i
    psubw mm_c, mm_van
%endif
    CALC_LINE 0, m_c,2, m_zero,m_full, 1, 7 - %1
    paddw m0, [t4 + i]
    mova [t4 + i], m0
%assign i i + mmsize
%endrep
    psubw mm_c, mm_vba
    add t4, 2 << %1
    cmp t4, t5
    jl .internal_loop
%if ARCH_X86_64
    psubw mm_c, m3
%else
    BCASTW 0, t2d
    psubw mm_c, m0
%endif

.end_loop
%if ARCH_X86_64
    test t7d, t7d
    jz .end_line_loop
    xor t6d, t6d
%else
    mov t2d, dn_pos
    test t2d, t2d
    jz .end_line_loop
    mov t6d, t2d
    jmp .last_line
%endif

.single_line
%if ARCH_X86_64 == 0
    mov t7d, dn_pos
%endif
    mov t2d, t7d
    sub t2d, t6d  ; dn_pos - up_pos
    add t6d, t7d  ; dn_pos + up_pos
.last_line
    FILL_BORDER_LINE %1, t4,t8,t9,t10,t2,t6, t0,t1, 0,1,2,3,4,5

.end_line_loop
%if ARCH_X86_64
    add r1, line_size
    sub r2, 1
%else
    sub dword r2m, 1
%endif
    jnz .line_loop

%if ARCH_X86_64 == 0
    mov r0, r0m
%endif
    mov r1, rstk
    mova m3, [words_max]
    SAVE_RESULT %1, r0,r1,3, r2,r3, 0,1,2
    ADD rstk, alloc_size
    RET
%endmacro

INIT_XMM sse2
FILL_GENERIC_TILE 4,16
FILL_GENERIC_TILE 5,32
INIT_YMM avx2
FILL_GENERIC_TILE 4,16
FILL_GENERIC_TILE 5,32

;------------------------------------------------------------------------------
; EXPAND_LINE_HORZ m_dst1, m_dst2, m_prev3, m_src4, m_next5, m_one6
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
; UPDATE_FLAG m_flag1, m_flag2[64],
;             m_src_a3, m_src_b4, m_cmp_a5, m_cmp_b6,
;             m_tmp7, i_a8, i_b9
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
; EXPAND_HORZ_TILE tile_order, suffix
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
; EXPAND_LINE_VERT m_dst1, m_dst2, m_prev3, m_src4, m_next5, m_one6
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
; EXPAND_VERT_TILE tile_order, suffix
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
