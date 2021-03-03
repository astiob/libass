/*
 * Copyright (C) 2015 Vabishchevich Nikolay <vabnick@gmail.com>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

void DECORATE(fill_solid_tile16)(uint8_t *buf, ptrdiff_t stride, int set);
void DECORATE(fill_solid_tile32)(uint8_t *buf, ptrdiff_t stride, int set);
void DECORATE(fill_halfplane_tile16)(uint8_t *buf, ptrdiff_t stride,
                                     int32_t a, int32_t b, int64_t c, int32_t scale);
void DECORATE(fill_halfplane_tile32)(uint8_t *buf, ptrdiff_t stride,
                                     int32_t a, int32_t b, int64_t c, int32_t scale);
void DECORATE(fill_generic_tile16)(uint8_t *buf, ptrdiff_t stride,
                                   const struct segment *line, size_t n_lines,
                                   int winding);
void DECORATE(fill_generic_tile32)(uint8_t *buf, ptrdiff_t stride,
                                   const struct segment *line, size_t n_lines,
                                   int winding);

void DECORATE(add_bitmaps)(uint8_t *dst, intptr_t dst_stride,
                           uint8_t *src, intptr_t src_stride,
                           intptr_t height, intptr_t width);
void DECORATE(sub_bitmaps)(uint8_t *dst, intptr_t dst_stride,
                           uint8_t *src, intptr_t src_stride,
                           intptr_t height, intptr_t width);
void DECORATE(mul_bitmaps)(uint8_t *dst, intptr_t dst_stride,
                           uint8_t *src1, intptr_t src1_stride,
                           uint8_t *src2, intptr_t src2_stride,
                           intptr_t width, intptr_t height);

void DECORATE(be_blur)(uint8_t *buf, intptr_t w, intptr_t h,
                       intptr_t stride, uint16_t *tmp);

void DECORATE(stripe_unpack)(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
                             uintptr_t width, uintptr_t height);
void DECORATE(stripe_pack)(uint8_t *dst, ptrdiff_t dst_stride, const int16_t *src,
                           uintptr_t width, uintptr_t height);
void DECORATE(shrink_horz)(int16_t *dst, const int16_t *src,
                           uintptr_t src_width, uintptr_t src_height);
void DECORATE(shrink_vert)(int16_t *dst, const int16_t *src,
                           uintptr_t src_width, uintptr_t src_height);
void DECORATE(expand_horz)(int16_t *dst, const int16_t *src,
                           uintptr_t src_width, uintptr_t src_height);
void DECORATE(expand_vert)(int16_t *dst, const int16_t *src,
                           uintptr_t src_width, uintptr_t src_height);
void DECORATE(blur4_horz)(int16_t *dst, const int16_t *src,
                          uintptr_t src_width, uintptr_t src_height,
                          const int16_t *param);
void DECORATE(blur4_vert)(int16_t *dst, const int16_t *src,
                          uintptr_t src_width, uintptr_t src_height,
                          const int16_t *param);
void DECORATE(blur5_horz)(int16_t *dst, const int16_t *src,
                          uintptr_t src_width, uintptr_t src_height,
                          const int16_t *param);
void DECORATE(blur5_vert)(int16_t *dst, const int16_t *src,
                          uintptr_t src_width, uintptr_t src_height,
                          const int16_t *param);
void DECORATE(blur6_horz)(int16_t *dst, const int16_t *src,
                          uintptr_t src_width, uintptr_t src_height,
                          const int16_t *param);
void DECORATE(blur6_vert)(int16_t *dst, const int16_t *src,
                          uintptr_t src_width, uintptr_t src_height,
                          const int16_t *param);
void DECORATE(blur7_horz)(int16_t *dst, const int16_t *src,
                          uintptr_t src_width, uintptr_t src_height,
                          const int16_t *param);
void DECORATE(blur7_vert)(int16_t *dst, const int16_t *src,
                          uintptr_t src_width, uintptr_t src_height,
                          const int16_t *param);
void DECORATE(blur8_horz)(int16_t *dst, const int16_t *src,
                          uintptr_t src_width, uintptr_t src_height,
                          const int16_t *param);
void DECORATE(blur8_vert)(int16_t *dst, const int16_t *src,
                          uintptr_t src_width, uintptr_t src_height,
                          const int16_t *param);


#if __x86_64__
    #define DECORATE64(func)  DECORATE(func)
#else
    #define DECORATE64(func)  ass_##func##_c
#endif

#if defined(__i386__) && CONFIG_ASM
    #define DECORATE86(func) ass_##func##_x86
#else
    #define DECORATE86(func) DECORATE64(func)
#endif

const BitmapEngine DECORATE(bitmap_engine) = {
    .align_order = ALIGN,

#if CONFIG_LARGE_TILES
    .tile_order = 5,
    .fill_solid = DECORATE64(fill_solid_tile32),
    .fill_halfplane = DECORATE64(fill_halfplane_tile32),
    .fill_generic = DECORATE64(fill_generic_tile32),
#else
    .tile_order = 4,
    .fill_solid = DECORATE64(fill_solid_tile16),
    .fill_halfplane = DECORATE64(fill_halfplane_tile16),
    .fill_generic = DECORATE64(fill_generic_tile16),
#endif

    .add_bitmaps = DECORATE(add_bitmaps),
    .sub_bitmaps = DECORATE86(sub_bitmaps),
    //.sub_bitmaps = DECORATE64(sub_bitmaps),
    .mul_bitmaps = DECORATE64(mul_bitmaps),

    .be_blur = DECORATE64(be_blur),

    .stripe_unpack = DECORATE64(stripe_unpack),
    .stripe_pack = DECORATE64(stripe_pack),
    .shrink_horz = DECORATE64(shrink_horz),
    .shrink_vert = DECORATE64(shrink_vert),
    .expand_horz = DECORATE64(expand_horz),
    .expand_vert = DECORATE64(expand_vert),
    .blur_horz = { DECORATE64(blur4_horz), DECORATE64(blur5_horz), DECORATE64(blur6_horz), DECORATE64(blur7_horz), DECORATE64(blur8_horz) },
    .blur_vert = { DECORATE64(blur4_vert), DECORATE64(blur5_vert), DECORATE64(blur6_vert), DECORATE64(blur7_vert), DECORATE64(blur8_vert) },
};
