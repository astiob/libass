/*
 * Copyright (C) 2014 Vabishchevich Nikolay <vabnick@gmail.com>
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



void DECORATE(finalize_generic_tile)(uint8_t *buf, ptrdiff_t stride,
                                     const int16_t *src);
void DECORATE(fill_halfplane_tile)(int16_t *buf,
                                   int32_t a, int32_t b, int64_t c, int32_t scale);
void DECORATE(fill_generic_tile)(int16_t *buf,
                                 const struct segment *line, size_t n_lines,
                                 int winding);


const TileEngine DECORATE(engine_tile) =
{
    .tile_order = TILE_ORDER,
    .tile_alignment = 32,
    .solid_tile = { empty_tile, solid_tile },
    .finalize_solid = DECORATE_F(finalize_solid),
    .finalize_generic = DECORATE(finalize_generic_tile),
    .fill_halfplane = DECORATE(fill_halfplane_tile),
    .fill_generic = DECORATE(fill_generic_tile),
    .combine = { DECORATE_C(mul_tile), DECORATE_C(add_tile), DECORATE_C(sub_tile) },
    .shrink = { DECORATE_C(shrink_horz_tile), DECORATE_C(shrink_vert_tile) },
    .shrink_solid = { DECORATE_C(shrink_horz_solid_tile), DECORATE_C(shrink_vert_solid_tile) },
    .expand = {
        { DECORATE_C(expand_horz1_tile), DECORATE_C(expand_vert1_tile) },
        { DECORATE_C(expand_horz2_tile), DECORATE_C(expand_vert2_tile) },
    },
    .expand_solid_out = {
        { DECORATE_C(expand_horz1_solid1_tile), DECORATE_C(expand_vert1_solid1_tile) },
        { DECORATE_C(expand_horz2_solid1_tile), DECORATE_C(expand_vert2_solid1_tile) },
    },
    .expand_solid_in = {
        { DECORATE_C(expand_horz1_solid2_tile), DECORATE_C(expand_vert1_solid2_tile) },
        { DECORATE_C(expand_horz2_solid2_tile), DECORATE_C(expand_vert2_solid2_tile) },
    },
    .pre_blur = {
        { DECORATE_C(pre_blur1_horz_tile), DECORATE_C(pre_blur1_vert_tile) },
        { DECORATE_C(pre_blur2_horz_tile), DECORATE_C(pre_blur2_vert_tile) },
        { DECORATE_C(pre_blur3_horz_tile), DECORATE_C(pre_blur3_vert_tile) },
    },
    .pre_blur_solid = {
        { DECORATE_C(pre_blur1_horz_solid_tile), DECORATE_C(pre_blur1_vert_solid_tile) },
        { DECORATE_C(pre_blur2_horz_solid_tile), DECORATE_C(pre_blur2_vert_solid_tile) },
        { DECORATE_C(pre_blur3_horz_solid_tile), DECORATE_C(pre_blur3_vert_solid_tile) },
    },
    .main_blur = {
        { DECORATE_C(blur1234_horz_tile), DECORATE_C(blur1234_vert_tile) },
        { DECORATE_C(blur1235_horz_tile), DECORATE_C(blur1235_vert_tile) },
        { DECORATE_C(blur1246_horz_tile), DECORATE_C(blur1246_vert_tile) },
    },
    .main_blur_solid = {
        { DECORATE_C(blur1234_horz_solid_tile), DECORATE_C(blur1234_vert_solid_tile) },
        { DECORATE_C(blur1235_horz_solid_tile), DECORATE_C(blur1235_vert_solid_tile) },
        { DECORATE_C(blur1246_horz_solid_tile), DECORATE_C(blur1246_vert_solid_tile) },
    },
    .shift = DECORATE_C(shift_tile),
};
