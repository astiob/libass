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
                                     const int16_t *src)
{
    int i, j;
    for (j = 0; j < TILE_SIZE; ++j) {
        for (i = 0; i < TILE_SIZE; ++i) {
            buf[i] = FFMIN(*src >> 6, 255);
            ++src;
        }
        buf += stride;
    }
}


/*
 * Halfplane Filling Functions
 *
 * Fill pixels with antialiasing corresponding to equation
 * A * x + B * y < C, where
 * x, y - offset of pixel center from bottom-left,
 * A = a * scale, B = b * scale, C = c * scale / 64.
 *
 * Normalization of coefficients prior call:
 * max(abs(a), abs(b)) * scale = 1 << 61
 *
 * Used Algorithm
 * Let
 * max_ab = max(abs(A), abs(B)),
 * min_ab = min(abs(A), abs(B)),
 * CC = C - A * x - B * y, then
 * result = (clamp((CC - min_ab / 4) / max_ab) +
 *           clamp((CC + min_ab / 4) / max_ab) +
 *           1) / 2,
 * where clamp(Z) = max(-0.5, min(0.5, Z)).
 */

void DECORATE(fill_halfplane_tile)(int16_t *buf,
                                   int32_t a, int32_t b, int64_t c, int32_t scale)
{
    const int64_t offs = (int64_t)1 << (45 + TILE_ORDER);
    int16_t aa = (a * (int64_t)scale + offs) >> (46 + TILE_ORDER);
    int16_t bb = (b * (int64_t)scale + offs) >> (46 + TILE_ORDER);
    int16_t cc = ((int32_t)(c >> (7 + TILE_ORDER)) * (int64_t)scale + ((int64_t)1 << 44)) >> 45;
    cc += (1 << (13 - TILE_ORDER)) - ((aa + bb) >> 1);

    int16_t abs_a = aa < 0 ? -aa : aa;
    int16_t abs_b = bb < 0 ? -bb : bb;
    int16_t delta = (FFMIN(abs_a, abs_b) + 2) >> 2;

    int i, j;
    int16_t va1[TILE_SIZE], va2[TILE_SIZE];
    for (i = 0; i < TILE_SIZE; ++i) {
        va1[i] = aa * i - delta;
        va2[i] = aa * i + delta;
    }

    static const int16_t full = (1 << (14 - TILE_ORDER)) - 1;
    for (j = 0; j < TILE_SIZE; ++j) {
        for (i = 0; i < TILE_SIZE; ++i) {
            int16_t c1 = cc - va1[i];
            int16_t c2 = cc - va2[i];
            c1 = FFMINMAX(c1, 0, full);
            c2 = FFMINMAX(c2, 0, full);
            *buf++ = (c1 + c2) << (TILE_ORDER - 1);
        }
        cc -= bb;
    }
}


/*
 * Generic Filling Functions
 *
 * Used Algorithm
 * Construct trapezium from each polyline segment and its projection into left side of tile.
 * Render that trapezium into internal buffer with additive blending and correct sign.
 * Store clamped absolute value from internal buffer into result buffer.
 */

// Render top/bottom line of the trapezium with antialiasing
static inline void DECORATE(update_border_line)(int16_t buf[],
                                                int16_t abs_a, const int16_t va[],
                                                int16_t b, int16_t abs_b,
                                                int16_t c, int up, int dn)
{
    int16_t size = dn - up;
    int16_t w = (1 << (14 - TILE_ORDER)) + (size << (8 - TILE_ORDER)) - abs_a;
    w = FFMIN(w, 1 << (14 - TILE_ORDER)) << (2 * TILE_ORDER - 5);

    int16_t dc_b = abs_b * (int32_t)size >> 6;
    int16_t dc = (FFMIN(abs_a, dc_b) + 2) >> 2;

    int16_t base = (int32_t)b * (int16_t)(up + dn) >> 7;
    int16_t offs1 = size - ((base + dc) * (int32_t)w >> 16);
    int16_t offs2 = size - ((base - dc) * (int32_t)w >> 16);

    int i;
    size <<= 1;
    for (i = 0; i < TILE_SIZE; ++i) {
        int16_t cw = (c - va[i]) * (int32_t)w >> 16;
        int16_t c1 = cw + offs1;
        int16_t c2 = cw + offs2;
        c1 = FFMINMAX(c1, 0, size);
        c2 = FFMINMAX(c2, 0, size);
        buf[i] += c1 + c2;
    }
}

void DECORATE(fill_generic_tile)(int16_t *buf,
                                 const struct segment *line, size_t n_lines,
                                 int winding)
{
    int i, j;
    int16_t delta[TILE_SIZE + 2];
    for (i = 0; i < TILE_SIZE * TILE_SIZE; ++i)
        buf[i] = 0;
    for (j = 0; j < TILE_SIZE + 2; ++j)
        delta[j] = 0;

    const int16_t full = 1 << (14 - TILE_ORDER);
    const int64_t offs = (int64_t)1 << (45 + TILE_ORDER);
    const struct segment *end = line + n_lines;
    for (; line != end; ++line) {
        assert(line->y_min >= 0 && line->y_min < 64 * TILE_SIZE);
        assert(line->y_max > 0 && line->y_max <= 64 * TILE_SIZE);
        assert(line->y_min <= line->y_max);

        int16_t up_delta = line->flags & SEGFLAG_DN ? 4 : 0;
        int16_t dn_delta = up_delta;
        if (!line->x_min && (line->flags & SEGFLAG_EXACT_LEFT))dn_delta ^= 4;
        if (line->flags & SEGFLAG_UL_DR) {
            int16_t tmp = up_delta;
            up_delta = dn_delta;
            dn_delta = tmp;
        }

        int up = line->y_min >> 6, dn = line->y_max >> 6;
        int16_t up_pos = line->y_min & 63;
        int16_t up_delta1 = up_delta * up_pos;
        int16_t dn_pos = line->y_max & 63;
        int16_t dn_delta1 = dn_delta * dn_pos;
        delta[up + 1] -= up_delta1;
        delta[up] -= (up_delta << 6) - up_delta1;
        delta[dn + 1] += dn_delta1;
        delta[dn] += (dn_delta << 6) - dn_delta1;
        if (line->y_min == line->y_max)
            continue;

        int16_t a = (line->a * (int64_t)line->scale + offs) >> (46 + TILE_ORDER);
        int16_t b = (line->b * (int64_t)line->scale + offs) >> (46 + TILE_ORDER);
        int16_t c = ((int32_t)(line->c >> (7 + TILE_ORDER)) * (int64_t)line->scale + ((int64_t)1 << 44)) >> 45;
        c -= (a >> 1) + b * up;

        int16_t va[TILE_SIZE];
        for (i = 0; i < TILE_SIZE; ++i)
            va[i] = a * i;
        int16_t abs_a = a < 0 ? -a : a;
        int16_t abs_b = b < 0 ? -b : b;
        int16_t dc = (FFMIN(abs_a, abs_b) + 2) >> 2;
        int16_t base = (1 << (13 - TILE_ORDER)) - (b >> 1);
        int16_t dc1 = base + dc;
        int16_t dc2 = base - dc;

        if (up_pos) {
            if (dn == up) {
                DECORATE(update_border_line)(buf + TILE_SIZE * up, abs_a, va, b, abs_b, c, up_pos, dn_pos);
                continue;
            }
            DECORATE(update_border_line)(buf + TILE_SIZE * up, abs_a, va, b, abs_b, c, up_pos, 64);
            up++;
            c -= b;
        }
        for (j = up; j < dn; ++j) {
            for (i = 0; i < TILE_SIZE; ++i) {
                int16_t c1 = c - va[i] + dc1;
                int16_t c2 = c - va[i] + dc2;
                c1 = FFMINMAX(c1, 0, full);
                c2 = FFMINMAX(c2, 0, full);
                buf[TILE_SIZE * j + i] += (c1 + c2) >> (7 - TILE_ORDER);
            }
            c -= b;
        }
        if (dn_pos)
            DECORATE(update_border_line)(buf + TILE_SIZE * dn, abs_a, va, b, abs_b, c, 0, dn_pos);
    }

    int16_t cur = winding << 8;
    for (j = 0; j < TILE_SIZE; ++j) {
        cur += delta[j];
        for (i = 0; i < TILE_SIZE; ++i) {
            int16_t val = buf[TILE_SIZE * j + i] + cur, neg_val = -val;
            val = (val > neg_val ? val : neg_val);
            buf[TILE_SIZE * j + i] = FFMIN(val, 256) << 6;
        }
    }
}


int DECORATE(mul_tile)(int16_t *dst,
                       const int16_t *src1, const int16_t *src2)
{
    int k;
    int16_t flag = 0;
    for (k = 0; k < TILE_SIZE * TILE_SIZE; k++)
        flag |= dst[k] = src1[k] * src2[k] >> 14;
    return flag != 0;
}

int DECORATE(add_tile)(int16_t *dst,
                       const int16_t *src1, const int16_t *src2)
{
    int k;
    int16_t flag = -1;
    for (k = 0; k < TILE_SIZE * TILE_SIZE; k++)
        flag &= dst[k] = FFMIN(1 << 14, src1[k] + src2[k]);
    return flag != -1;
}

int DECORATE(sub_tile)(int16_t *dst,
                       const int16_t *src1, const int16_t *src2)
{
    int k;
    int16_t flag = 0;
    for (k = 0; k < TILE_SIZE * TILE_SIZE; k++)
        flag |= dst[k] = FFMAX(0, src1[k] - src2[k]);
    return flag != 0;
}


const TileEngine DECORATE(engine_tile) =
{
    .tile_order = TILE_ORDER,
    .tile_alignment = 32,
    .solid_tile = { empty_tile, solid_tile },
    .finalize_solid = ass_finalize_solid_c,
    .finalize_generic = DECORATE(finalize_generic_tile),
    .fill_halfplane = DECORATE(fill_halfplane_tile),
    .fill_generic = DECORATE(fill_generic_tile),
    .tile_combine = { DECORATE(mul_tile), DECORATE(add_tile), DECORATE(sub_tile) }
};
