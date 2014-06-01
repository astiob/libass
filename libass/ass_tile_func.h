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
    for (int j = 0; j < TILE_SIZE; ++j) {
        for (int i = 0; i < TILE_SIZE; ++i) {
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

    int16_t va1[TILE_SIZE], va2[TILE_SIZE];
    for (int i = 0; i < TILE_SIZE; ++i) {
        va1[i] = aa * i - delta;
        va2[i] = aa * i + delta;
    }

    static const int16_t full = (1 << (14 - TILE_ORDER)) - 1;
    for (int j = 0; j < TILE_SIZE; ++j) {
        for (int i = 0; i < TILE_SIZE; ++i) {
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

    size <<= 1;
    for (int i = 0; i < TILE_SIZE; ++i) {
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
    int16_t delta[TILE_SIZE + 2];
    for (int k = 0; k < TILE_SIZE * TILE_SIZE; ++k)
        buf[k] = 0;
    for (int j = 0; j < TILE_SIZE + 2; ++j)
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
        for (int i = 0; i < TILE_SIZE; ++i)
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
            ++up;
            c -= b;
        }
        for (int j = up; j < dn; ++j) {
            for (int i = 0; i < TILE_SIZE; ++i) {
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
    for (int j = 0; j < TILE_SIZE; ++j) {
        cur += delta[j];
        for (int i = 0; i < TILE_SIZE; ++i) {
            int16_t val = buf[TILE_SIZE * j + i] + cur, neg_val = -val;
            val = (val > neg_val ? val : neg_val);
            buf[TILE_SIZE * j + i] = FFMIN(val, 256) << 6;
        }
    }
}


int DECORATE(mul_tile)(int16_t *dst,
                       const int16_t *src1, const int16_t *src2)
{
    int16_t flag = 0;
    for (int k = 0; k < TILE_SIZE * TILE_SIZE; ++k)
        flag |= dst[k] = src1[k] * src2[k] >> 14;
    return flag != 0;
}

int DECORATE(add_tile)(int16_t *dst,
                       const int16_t *src1, const int16_t *src2)
{
    int16_t flag = -1;
    for (int k = 0; k < TILE_SIZE * TILE_SIZE; ++k)
        flag &= dst[k] = FFMIN(1 << 14, src1[k] + src2[k]);
    return flag != -1;
}

int DECORATE(sub_tile)(int16_t *dst,
                       const int16_t *src1, const int16_t *src2)
{
    int16_t flag = 0;
    for (int k = 0; k < TILE_SIZE * TILE_SIZE; ++k)
        flag |= dst[k] = FFMAX(0, src1[k] - src2[k]);
    return flag != 0;
}


void DECORATE(shrink_horz_tile)(int16_t *dst,
                                const int16_t *side1, const int16_t *src1,
                                const int16_t *src2, const int16_t *side2)
{
#define LINE(s0, s1, s2, s3, s4, s5) \
    *dst++ = ((s0)[-2] + 5 * (s1)[-1] + 10 * (s2)[0] + 10 * (s3)[1] + 5 * (s4)[2] + (s5)[3] + 16) >> 5;

    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        LINE(side1, side1, src1, src1, src1, src1);
        src1 += 2;
        for (int j = 1; j < TILE_SIZE / 2 - 1; ++j) {
            LINE(src1, src1, src1, src1, src1, src1);
            src1 += 2;
        }
        LINE(src1, src1, src1, src1, src2 - 2, src2 - 2);
        src1 += 2;
        LINE(src1, src1, src2, src2, src2, src2);
        src2 += 2;
        for (int j = 1; j < TILE_SIZE / 2 - 1; ++j) {
            LINE(src2, src2, src2, src2, src2, src2);
            src2 += 2;
        }
        LINE(src2, src2, src2, src2, side2 - 2, side2 - 2);
        src2 += 2;
        side2 += TILE_SIZE;
    }
#undef LINE
}

int DECORATE(shrink_horz_solid_tile)(int16_t *dst,
                                     const int16_t *side1, int set, const int16_t *side2)
{
    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        ptr[0] = (side1[-2] + 5 * side1[-1] + 26 * val + 16) >> 5;
        flag |= ptr[0] ^ val;
        ptr += TILE_SIZE;
        ptr[-1] = (26 * val + 5 * side2[0] + side2[1] + 16) >> 5;
        flag |= ptr[-1] ^ val;
        side2 += TILE_SIZE;
    }
    if (!flag)
        return 0;

    for (int i = 0; i < TILE_SIZE; ++i) {
        ++dst;
        for (int j = 1; j < TILE_SIZE - 1; ++j)
            *dst++ = val;
        ++dst;
    }
    return 1;
}

void DECORATE(shrink_vert_tile)(int16_t *dst,
                                const int16_t *side1, const int16_t *src1,
                                const int16_t *src2, const int16_t *side2)
{
#define LINE(s0, s1, s2, s3, s4, s5) \
    for (int j = 0; j < TILE_SIZE; ++j) \
        dst[j] = ((s0)[j - 2 * TILE_SIZE] *  1 + \
                  (s1)[j - 1 * TILE_SIZE] *  5 + \
                  (s2)[j + 0 * TILE_SIZE] * 10 + \
                  (s3)[j + 1 * TILE_SIZE] * 10 + \
                  (s4)[j + 2 * TILE_SIZE] *  5 + \
                  (s5)[j + 3 * TILE_SIZE] *  1 + \
                  16) >> 5; \
    dst += TILE_SIZE;

    side1 += TILE_SIZE * TILE_SIZE;
    LINE(side1, side1, src1, src1, src1, src1);
    src1 += 2 * TILE_SIZE;
    for (int i = 1; i < TILE_SIZE / 2 - 1; ++i) {
        LINE(src1, src1, src1, src1, src1, src1);
        src1 += 2 * TILE_SIZE;
    }
    LINE(src1, src1, src1, src1, src2 - 2 * TILE_SIZE, src2 - 2 * TILE_SIZE);
    src1 += 2 * TILE_SIZE;
    LINE(src1, src1, src2, src2, src2, src2);
    src2 += 2 * TILE_SIZE;
    for (int i = 1; i < TILE_SIZE / 2 - 1; ++i) {
        LINE(src2, src2, src2, src2, src2, src2);
        src2 += 2 * TILE_SIZE;
    }
    LINE(src2, src2, src2, src2, side2 - 2 * TILE_SIZE, side2 - 2 * TILE_SIZE);

#undef LINE
}

int DECORATE(shrink_vert_solid_tile)(int16_t *dst,
                                     const int16_t *side1, int set, const int16_t *side2)
{
#define IDX(n) (j + (n) * TILE_SIZE)

    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    side1 += TILE_SIZE * TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (side1[IDX(-2)] + 5 * side1[IDX(-1)] + 26 * val + 16) >> 5;
        flag |= ptr[j] ^ val;
    }
    ptr += (TILE_SIZE - 1) * TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (26 * val + 5 * side2[IDX(0)] + side2[IDX(1)] + 16) >> 5;
        flag |= ptr[j] ^ val;
    }
    if (!flag)
        return 0;

    dst += TILE_SIZE;
    for (int i = 1; i < TILE_SIZE - 1; ++i) {
        for (int j = 0; j < TILE_SIZE; ++j)
            dst[j] = val;
        dst += TILE_SIZE;
    }
    return 1;

#undef IDX
}


#define LINE(s0, s1, s2) \
    *dst++ = (5 * (s0)[-1] + 10 * (s1)[0] + 1 * (s2)[1] + 8) >> 4; \
    *dst++ = (1 * (s0)[-1] + 10 * (s1)[0] + 5 * (s2)[1] + 8) >> 4;

void DECORATE(expand_horz1_tile)(int16_t *dst, const int16_t *side, const int16_t *src)
{
    for (int i = 0; i < TILE_SIZE; ++i) {
        side += TILE_SIZE;
        LINE(side, src, src);
        ++src;
        for (int j = 2; j < TILE_SIZE; j += 2) {
            LINE(src, src, src);
            ++src;
        }
        src += TILE_SIZE / 2;
    }
}

void DECORATE(expand_horz2_tile)(int16_t *dst, const int16_t *side, const int16_t *src)
{
    for (int i = 0; i < TILE_SIZE; ++i) {
        src += TILE_SIZE / 2;
        for (int j = 0; j < TILE_SIZE - 2; j += 2) {
            LINE(src, src, src);
            ++src;
        }
        LINE(src, src, side - 1);
        ++src;
        side += TILE_SIZE;
    }
}
#undef LINE

#define LINE(s0, s1, s2) \
    dst[0] = (5 * (s0)[-1] + 10 * (s1)[0] + 1 * (s2)[1] + 8) >> 4; \
    dst[1] = (1 * (s0)[-1] + 10 * (s1)[0] + 5 * (s2)[1] + 8) >> 4; \
    flag |= (dst[0] ^ val) | (dst[1] ^ val); \
    dst += 2;

int DECORATE(expand_horz1_solid1_tile)(int16_t *dst, const int16_t *src, int set)
{
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        dst[0] = (5 * val + 10 * src[0] + 1 * src[1] + 8) >> 4;
        dst[1] = (1 * val + 10 * src[0] + 5 * src[1] + 8) >> 4;
        flag |= (dst[0] ^ val) | (dst[1] ^ val);
        dst += 2;
        ++src;
        for (int j = 2; j < TILE_SIZE; j += 2) {
            LINE(src, src, src);
            ++src;
        }
        src += TILE_SIZE / 2;
    }
    return flag != 0;
}

int DECORATE(expand_horz2_solid1_tile)(int16_t *dst, const int16_t *src, int set)
{
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        src += TILE_SIZE / 2;
        for (int j = 0; j < TILE_SIZE - 2; j += 2) {
            LINE(src, src, src);
            ++src;
        }
        dst[0] = (5 * src[-1] + 10 * src[0] + 1 * val + 8) >> 4;
        dst[1] = (1 * src[-1] + 10 * src[0] + 5 * val + 8) >> 4;
        flag |= (dst[0] ^ val) | (dst[1] ^ val);
        dst += 2;
        ++src;
    }
    return flag != 0;
}
#undef LINE

int DECORATE(expand_horz1_solid2_tile)(int16_t *dst, const int16_t *side, int set)
{
    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side += TILE_SIZE;
        ptr[0] = (5 * side[-1] + 11 * val + 8) >> 4;
        ptr[1] = (1 * side[-1] + 15 * val + 8) >> 4;
        flag |= (ptr[0] ^ val) | (ptr[1] ^ val);
        ptr += TILE_SIZE;
    }
    if (!flag)
        return 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        dst += 2;
        for (int j = 2; j < TILE_SIZE; ++j)
            *dst++ = val;
    }
    return 1;
}

int DECORATE(expand_horz2_solid2_tile)(int16_t *dst, const int16_t *side, int set)
{
    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        ptr += TILE_SIZE;
        ptr[-2] = (15 * val + 1 * side[0] + 8) >> 4;
        ptr[-1] = (11 * val + 5 * side[0] + 8) >> 4;
        flag |= (ptr[-2] ^ val) | (ptr[-1] ^ val);
        side += TILE_SIZE;
    }
    if (!flag)
        return 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        for (int j = 0; j < TILE_SIZE - 2; ++j)
            *dst++ = val;
        dst += 2;
    }
    return 1;
}

#define LINE(s0, s1, s2) \
    for (int j = 0; j < TILE_SIZE; ++j) \
        dst[j] = ((s0)[j - 1 * TILE_SIZE] *  5 + \
                  (s1)[j + 0 * TILE_SIZE] * 10 + \
                  (s2)[j + 1 * TILE_SIZE] *  1 + \
                  8) >> 4; \
    dst += TILE_SIZE; \
    for (int j = 0; j < TILE_SIZE; ++j) \
        dst[j] = ((s0)[j - 1 * TILE_SIZE] *  1 + \
                  (s1)[j + 0 * TILE_SIZE] * 10 + \
                  (s2)[j + 1 * TILE_SIZE] *  5 + \
                  8) >> 4; \
    dst += TILE_SIZE;

void DECORATE(expand_vert1_tile)(int16_t *dst, const int16_t *side, const int16_t *src)
{
    side += TILE_SIZE * TILE_SIZE;
    LINE(side, src, src);
    src += TILE_SIZE;
    for (int j = 2; j < TILE_SIZE; j += 2) {
        LINE(src, src, src);
        src += TILE_SIZE;
    }
}

void DECORATE(expand_vert2_tile)(int16_t *dst, const int16_t *side, const int16_t *src)
{
    src += TILE_SIZE * TILE_SIZE / 2;
    for (int j = 0; j < TILE_SIZE - 2; j += 2) {
        LINE(src, src, src);
        src += TILE_SIZE;
    }
    LINE(src, src, side - TILE_SIZE);
}
#undef LINE

#define LINE(s0, s1, s2) \
    for (int j = 0; j < TILE_SIZE; ++j) { \
        dst[j] = ((s0)[j - 1 * TILE_SIZE] *  5 + \
                  (s1)[j + 0 * TILE_SIZE] * 10 + \
                  (s2)[j + 1 * TILE_SIZE] *  1 + \
                  8) >> 4; \
        flag |= dst[j] ^ val; \
    } \
    dst += TILE_SIZE; \
    for (int j = 0; j < TILE_SIZE; ++j) { \
        dst[j] = ((s0)[j - 1 * TILE_SIZE] *  1 + \
                  (s1)[j + 0 * TILE_SIZE] * 10 + \
                  (s2)[j + 1 * TILE_SIZE] *  5 + \
                  8) >> 4; \
        flag |= dst[j] ^ val; \
    } \
    dst += TILE_SIZE;

int DECORATE(expand_vert1_solid1_tile)(int16_t *dst, const int16_t *src, int set)
{
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int j = 0; j < TILE_SIZE; ++j) {
        dst[j] = (5 * val + 10 * src[j] + 1 * src[j + TILE_SIZE] + 8) >> 4;
        flag |= dst[j] ^ val;
    }
    dst += TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        dst[j] = (1 * val + 10 * src[j] + 5 * src[j + TILE_SIZE] + 8) >> 4;
        flag |= dst[j] ^ val;
    }
    dst += TILE_SIZE;
    src += TILE_SIZE;
    for (int j = 2; j < TILE_SIZE; j += 2) {
        LINE(src, src, src);
        src += TILE_SIZE;
    }
    return flag != 0;
}

int DECORATE(expand_vert2_solid1_tile)(int16_t *dst, const int16_t *src, int set)
{
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    src += TILE_SIZE * TILE_SIZE / 2;
    for (int j = 0; j < TILE_SIZE - 2; j += 2) {
        LINE(src, src, src);
        src += TILE_SIZE;
    }
    for (int j = 0; j < TILE_SIZE; ++j) {
        dst[j] = (5 * src[j - TILE_SIZE] + 10 * src[j] + 1 * val + 8) >> 4;
        flag |= dst[j] ^ val;
    }
    dst += TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        dst[j] = (1 * src[j - TILE_SIZE] + 10 * src[j] + 5 * val + 8) >> 4;
        flag |= dst[j] ^ val;
    }
    return flag != 0;
}
#undef LINE

int DECORATE(expand_vert1_solid2_tile)(int16_t *dst, const int16_t *side, int set)
{
    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    side += TILE_SIZE * TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (5 * side[j - TILE_SIZE] + 11 * val + 8) >> 4;
        flag |= ptr[j] ^ val;
    }
    ptr += TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (1 * side[j - TILE_SIZE] + 15 * val + 8) >> 4;
        flag |= ptr[j] ^ val;
    }
    if (!flag)
        return 0;

    dst += 2 * TILE_SIZE;
    for (int i = 2; i < TILE_SIZE; ++i) {
        for (int j = 0; j < TILE_SIZE; ++j)
            dst[j] = val;
        dst += TILE_SIZE;
    }
    return 1;
}

int DECORATE(expand_vert2_solid2_tile)(int16_t *dst, const int16_t *side, int set)
{
    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    ptr += (TILE_SIZE - 2) * TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (15 * val + 1 * side[j] + 8) >> 4;
        flag |= ptr[j] ^ val;
    }
    ptr += TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (11 * val + 5 * side[j] + 8) >> 4;
        flag |= ptr[j] ^ val;
    }
    if (!flag)
        return 0;

    for (int i = 0; i < TILE_SIZE - 2; ++i) {
        for (int j = 0; j < TILE_SIZE; ++j)
            dst[j] = val;
        dst += TILE_SIZE;
    }
    return 1;
}


void DECORATE(pre_blur1_horz_tile)(int16_t *dst,
                                   const int16_t *side1, const int16_t *src, const int16_t *side2,
                                   void *param)
{
#define LINE(s0, s1, s2) \
    *dst++ = ((s0)[-1] + 2 * (s1)[0] + (s2)[1] + 2) >> 2;

    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        LINE(side1 + 0, src, src);
        ++src;
        for (int j = 1; j < TILE_SIZE - 1; ++j) {
            LINE(src, src, src);
            ++src;
        }
        LINE(src, src, side2 - 1);
        ++src;
        side2 += TILE_SIZE;
    }
#undef LINE
}

int DECORATE(pre_blur1_horz_solid_tile)(int16_t *dst,
                                        const int16_t *side1, int set, const int16_t *side2,
                                        void *param)
{
    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        ptr[0] = (side1[-1] + 3 * val + 2) >> 2;
        flag |= (ptr[0] ^ val);
        ptr += TILE_SIZE;
        ptr[-1] = (3 * val + side2[0] + 2) >> 2;
        flag |= (ptr[-1] ^ val);
        side2 += TILE_SIZE;
    }
    if (!flag)
        return 0;

    for (int i = 0; i < TILE_SIZE; ++i) {
        ++dst;
        for (int j = 1; j < TILE_SIZE - 1; ++j)
            *dst++ = val;
        ++dst;
    }
    return 1;
}

void DECORATE(pre_blur1_vert_tile)(int16_t *dst,
                                   const int16_t *side1, const int16_t *src, const int16_t *side2,
                                   void *param)
{
#define LINE(s0, s1, s2) \
    for (int j = 0; j < TILE_SIZE; ++j) \
        dst[j] = ((s0)[j - 1 * TILE_SIZE] * 1 + \
                  (s1)[j + 0 * TILE_SIZE] * 2 + \
                  (s2)[j + 1 * TILE_SIZE] * 1 + \
                  2) >> 2; \
    dst += TILE_SIZE;

    side1 += TILE_SIZE * TILE_SIZE;
    LINE(side1, src, src);
    src += TILE_SIZE;
    for (int i = 1; i < TILE_SIZE - 1; ++i) {
        LINE(src, src, src);
        src += TILE_SIZE;
    }
    LINE(src, src, side2 - TILE_SIZE);

#undef LINE
}

int DECORATE(pre_blur1_vert_solid_tile)(int16_t *dst,
                                        const int16_t *side1, int set, const int16_t *side2,
                                        void *param)
{
#define IDX(n) (j + (n) * TILE_SIZE)

    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    side1 += TILE_SIZE * TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (side1[IDX(-1)] + 3 * val + 2) >> 2;
        flag |= ptr[j] ^ val;
    }
    ptr += (TILE_SIZE - 1) * TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (3 * val + side2[IDX(0)] + 2) >> 2;
        flag |= ptr[j] ^ val;
    }
    if (!flag)
        return 0;

    dst += TILE_SIZE;
    for (int i = 1; i < TILE_SIZE - 1; ++i) {
        for (int j = 0; j < TILE_SIZE; ++j)
            dst[j] = val;
        dst += TILE_SIZE;
    }
    return 1;

#undef IDX
}


void DECORATE(pre_blur2_horz_tile)(int16_t *dst,
                                   const int16_t *side1, const int16_t *src, const int16_t *side2,
                                   void *param)
{
#define LINE(s0, s1, s2, s3, s4) \
    *dst++ = ((s0)[-2] + 4 * (s1)[-1] + 6 * (s2)[0] + 4 * (s3)[1] + (s4)[2] + 8) >> 4;

    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        LINE(side1 + 0, side1 + 0, src, src, src);
        ++src;
        LINE(side1 + 1, src, src, src, src);
        ++src;
        for (int j = 2; j < TILE_SIZE - 2; ++j) {
            LINE(src, src, src, src, src);
            ++src;
        }
        LINE(src, src, src, src, side2 - 2);
        ++src;
        LINE(src, src, src, side2 - 1, side2 - 1);
        ++src;
        side2 += TILE_SIZE;
    }
#undef LINE
}

int DECORATE(pre_blur2_horz_solid_tile)(int16_t *dst,
                                        const int16_t *side1, int set, const int16_t *side2,
                                        void *param)
{
    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        ptr[0] = (side1[-2] + 4 * side1[-1] + 11 * val + 8) >> 4;
        ptr[1] = (side1[-1] + 15 * val + 8) >> 4;
        flag |= (ptr[0] ^ val) | (ptr[1] ^ val);
        ptr += TILE_SIZE;
        ptr[-2] = (15 * val + side2[0] + 8) >> 4;
        ptr[-1] = (11 * val + 4 * side2[0] + side2[1] + 8) >> 4;
        flag |= (ptr[-2] ^ val) | (ptr[-1] ^ val);
        side2 += TILE_SIZE;
    }
    if (!flag)
        return 0;

    for (int i = 0; i < TILE_SIZE; ++i) {
        dst += 2;
        for (int j = 2; j < TILE_SIZE - 2; ++j)
            *dst++ = val;
        dst += 2;
    }
    return 1;
}

void DECORATE(pre_blur2_vert_tile)(int16_t *dst,
                                   const int16_t *side1, const int16_t *src, const int16_t *side2,
                                   void *param)
{
#define LINE(s0, s1, s2, s3, s4) \
    for (int j = 0; j < TILE_SIZE; ++j) \
        dst[j] = ((s0)[j - 2 * TILE_SIZE] * 1 + \
                  (s1)[j - 1 * TILE_SIZE] * 4 + \
                  (s2)[j + 0 * TILE_SIZE] * 6 + \
                  (s3)[j + 1 * TILE_SIZE] * 4 + \
                  (s4)[j + 2 * TILE_SIZE] * 1 + \
                  8) >> 4; \
    dst += TILE_SIZE;


    side1 += TILE_SIZE * TILE_SIZE;
    LINE(side1, side1, src, src, src);
    src += TILE_SIZE;
    side1 += TILE_SIZE;
    LINE(side1, src, src, src, src);
    src += TILE_SIZE;
    for (int i = 2; i < TILE_SIZE - 2; ++i) {
        LINE(src, src, src, src, src);
        src += TILE_SIZE;
    }
    side2 -= 2 * TILE_SIZE;
    LINE(src, src, src, src, side2);
    src += TILE_SIZE;
    side2 += TILE_SIZE;
    LINE(src, src, src, side2, side2);

#undef LINE
}

int DECORATE(pre_blur2_vert_solid_tile)(int16_t *dst,
                                        const int16_t *side1, int set, const int16_t *side2,
                                        void *param)
{
#define IDX(n) (j + (n) * TILE_SIZE)

    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    side1 += TILE_SIZE * TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (side1[IDX(-2)] + 4 * side1[IDX(-1)] + 11 * val + 8) >> 4;
        flag |= ptr[j] ^ val;
    }
    ptr += TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (side1[IDX(-1)] + 15 * val + 8) >> 4;
        flag |= ptr[j] ^ val;
    }
    ptr += (TILE_SIZE - 3) * TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (15 * val + side2[IDX(0)] + 8) >> 4;
        flag |= ptr[j] ^ val;
    }
    ptr += TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (11 * val + 4 * side2[IDX(0)] + side2[IDX(1)] + 8) >> 4;
        flag |= ptr[j] ^ val;
    }
    if (!flag)
        return 0;

    dst += 2 * TILE_SIZE;
    for (int i = 2; i < TILE_SIZE - 2; ++i) {
        for (int j = 0; j < TILE_SIZE; ++j)
            dst[j] = val;
        dst += TILE_SIZE;
    }
    return 1;

#undef IDX
}


void DECORATE(pre_blur3_horz_tile)(int16_t *dst,
                                   const int16_t *side1, const int16_t *src, const int16_t *side2,
                                   void *param)
{
#define LINE(s0, s1, s2, s3, s4, s5, s6) \
    *dst++ = ((s0)[-3] + 6 * (s1)[-2] + 15 * (s2)[-1] + 20 * (s3)[0] + 15 * (s4)[1] + 6 * (s5)[2] + (s6)[3] + 32) >> 6;

    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        LINE(side1 + 0, side1 + 0, side1 + 0, src, src, src, src);
        ++src;
        LINE(side1 + 1, side1 + 1, src, src, src, src, src);
        ++src;
        LINE(side1 + 2, src, src, src, src, src, src);
        ++src;
        for (int j = 3; j < TILE_SIZE - 3; ++j) {
            LINE(src, src, src, src, src, src, src);
            ++src;
        }
        LINE(src, src, src, src, src, src, side2 - 3);
        ++src;
        LINE(src, src, src, src, src, side2 - 2, side2 - 2);
        ++src;
        LINE(src, src, src, src, side2 - 1, side2 - 1, side2 - 1);
        ++src;
        side2 += TILE_SIZE;
    }
#undef LINE
}

int DECORATE(pre_blur3_horz_solid_tile)(int16_t *dst,
                                        const int16_t *side1, int set, const int16_t *side2,
                                        void *param)
{
    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        ptr[0] = (side1[-3] + 6 * side1[-2] + 15 * side1[-1] + 42 * val + 32) >> 6;
        ptr[1] = (side1[-2] + 6 * side1[-1] + 57 * val + 32) >> 6;
        ptr[2] = (side1[-1] + 63 * val + 32) >> 6;
        flag |= (ptr[0] ^ val) | (ptr[1] ^ val) | (ptr[2] ^ val);
        ptr += TILE_SIZE;
        ptr[-3] = (63 * val + side2[0] + 32) >> 6;
        ptr[-2] = (57 * val + 6 * side2[0] + side2[1] + 32) >> 6;
        ptr[-1] = (42 * val + 15 * side2[0] + 6 * side2[1] + side2[2] + 32) >> 6;
        flag |= (ptr[-3] ^ val) | (ptr[-2] ^ val) | (ptr[-1] ^ val);
        side2 += TILE_SIZE;
    }
    if (!flag)
        return 0;

    for (int i = 0; i < TILE_SIZE; ++i) {
        dst += 3;
        for (int j = 3; j < TILE_SIZE - 3; ++j)
            *dst++ = val;
        dst += 3;
    }
    return 1;
}

void DECORATE(pre_blur3_vert_tile)(int16_t *dst,
                                   const int16_t *side1, const int16_t *src, const int16_t *side2,
                                   void *param)
{
#define LINE(s0, s1, s2, s3, s4, s5, s6) \
    for (int j = 0; j < TILE_SIZE; ++j) \
        dst[j] = ((s0)[j - 3 * TILE_SIZE] *  1 + \
                  (s1)[j - 2 * TILE_SIZE] *  5 + \
                  (s2)[j - 1 * TILE_SIZE] * 15 + \
                  (s3)[j + 0 * TILE_SIZE] * 20 + \
                  (s4)[j + 1 * TILE_SIZE] * 15 + \
                  (s5)[j + 2 * TILE_SIZE] *  5 + \
                  (s6)[j + 3 * TILE_SIZE] *  1 + \
                  32) >> 6; \
    dst += TILE_SIZE;


    side1 += TILE_SIZE * TILE_SIZE;
    LINE(side1, side1, side1, src, src, src, src);
    src += TILE_SIZE;
    side1 += TILE_SIZE;
    LINE(side1, side1, src, src, src, src, src);
    src += TILE_SIZE;
    side1 += TILE_SIZE;
    LINE(side1, src, src, src, src, src, src);
    src += TILE_SIZE;
    for (int i = 3; i < TILE_SIZE - 3; ++i) {
        LINE(src, src, src, src, src, src, src);
        src += TILE_SIZE;
    }
    side2 -= 3 * TILE_SIZE;
    LINE(src, src, src, src, src, src, side2);
    src += TILE_SIZE;
    side2 += TILE_SIZE;
    LINE(src, src, src, src, src, side2, side2);
    src += TILE_SIZE;
    side2 += TILE_SIZE;
    LINE(src, src, src, src, side2, side2, side2);

#undef LINE
}

int DECORATE(pre_blur3_vert_solid_tile)(int16_t *dst,
                                        const int16_t *side1, int set, const int16_t *side2,
                                        void *param)
{
#define IDX(n) (j + (n) * TILE_SIZE)

    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    side1 += TILE_SIZE * TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (side1[IDX(-3)] + 6 * side1[IDX(-2)] + 15 * side1[IDX(-1)] + 42 * val + 32) >> 6;
        flag |= ptr[j] ^ val;
    }
    ptr += TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (side1[IDX(-2)] + 6 * side1[IDX(-1)] + 57 * val + 32) >> 6;
        flag |= ptr[j] ^ val;
    }
    ptr += TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (side1[IDX(-1)] + 63 * val + 32) >> 6;
        flag |= ptr[j] ^ val;
    }
    ptr += (TILE_SIZE - 5) * TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (63 * val + side2[IDX(0)] + 32) >> 6;
        flag |= ptr[j] ^ val;
    }
    ptr += TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (57 * val + 6 * side2[IDX(0)] + side2[IDX(1)] + 32) >> 6;
        flag |= ptr[j] ^ val;
    }
    ptr += TILE_SIZE;
    for (int j = 0; j < TILE_SIZE; ++j) {
        ptr[j] = (42 * val + 15 * side2[IDX(0)] + 6 * side2[IDX(1)] + side2[IDX(2)] + 32) >> 6;
        flag |= ptr[j] ^ val;
    }
    if (!flag)
        return 0;

    dst += 3 * TILE_SIZE;
    for (int i = 3; i < TILE_SIZE - 3; ++i) {
        for (int j = 0; j < TILE_SIZE; ++j)
            dst[j] = val;
        dst += TILE_SIZE;
    }
    return 1;

#undef IDX
}


void DECORATE(blur1234_horz_tile)(int16_t *dst,
                                  const int16_t *side1, const int16_t *src, const int16_t *side2,
                                  void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(n4, n3, n2, n1, z0, p1, p2, p3, p4) \
    *dst++ = (c[4] * (n4)[-4] + \
              c[3] * (n3)[-3] + \
              c[2] * (n2)[-2] + \
              c[1] * (n1)[-1] + \
              c[0] * (z0)[+0] + \
              c[1] * (p1)[+1] + \
              c[2] * (p2)[+2] + \
              c[3] * (p3)[+3] + \
              c[4] * (p4)[+4] + \
              0x8000) >> 16;

#define BEG_LINE(n) \
        LINE(n < 4 ? side1 : src, \
             n < 3 ? side1 : src, \
             n < 2 ? side1 : src, \
             n < 1 ? side1 : src, \
             src, src, src, src, src); \
        ++side1; \
        ++src;

#define END_LINE(n) \
        LINE(src, src, src, src, src, \
             n < 1 ? side2 : src, \
             n < 2 ? side2 : src, \
             n < 3 ? side2 : src, \
             n < 4 ? side2 : src); \
        ++side2; \
        ++src;

    side1 += 4;
    side2 -= 4;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE - 4;
        BEG_LINE(0);
        BEG_LINE(1);
        BEG_LINE(2);
        BEG_LINE(3);
        for (int j = 4; j < TILE_SIZE - 4; ++j) {
            LINE(src, src, src, src, src, src, src, src, src);
            ++src;
        }
        END_LINE(3);
        END_LINE(2);
        END_LINE(1);
        END_LINE(0);
        side2 += TILE_SIZE - 4;
    }

#undef LINE
#undef BEG_LINE
#undef END_LINE
}

int DECORATE(blur1234_horz_solid_tile)(int16_t *dst,
                                       const int16_t *side1, int set, const int16_t *side2,
                                       void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(res, n4, n3, n2, n1, z0, p1, p2, p3, p4) \
        ptr[res] = (c[4] * (n4) + \
                    c[3] * (n3) + \
                    c[2] * (n2) + \
                    c[1] * (n1) + \
                    c[0] * (z0) + \
                    c[1] * (p1) + \
                    c[2] * (p2) + \
                    c[3] * (p3) + \
                    c[4] * (p4) + \
                    0x8000) >> 16; \
        flag |= ptr[res] ^ val;

#define BEG_LINE(n) \
        LINE(n, \
             n < 4 ? side1[n - 4] : val, \
             n < 3 ? side1[n - 3] : val, \
             n < 2 ? side1[n - 2] : val, \
             n < 1 ? side1[n - 1] : val, \
             val, val, val, val, val);

#define END_LINE(n) \
        LINE(-(n + 1), \
             val, val, val, val, val, \
             n < 1 ? side2[1 - (n + 1)] : val, \
             n < 2 ? side2[2 - (n + 1)] : val, \
             n < 3 ? side2[3 - (n + 1)] : val, \
             n < 4 ? side2[4 - (n + 1)] : val);

    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        BEG_LINE(0);
        BEG_LINE(1);
        BEG_LINE(2);
        BEG_LINE(3);
        ptr += TILE_SIZE;
        END_LINE(3);
        END_LINE(2);
        END_LINE(1);
        END_LINE(0);
        side2 += TILE_SIZE;
    }
    if (!flag)
        return 0;

    for (int i = 0; i < TILE_SIZE; ++i) {
        dst += 4;
        for (int j = 4; j < TILE_SIZE - 4; ++j)
            *dst++ = val;
        dst += 4;
    }
    return 1;

#undef LINE
#undef BEG_LINE
#undef END_LINE
}

void DECORATE(blur1234_vert_tile)(int16_t *dst,
                                  const int16_t *side1, const int16_t *src, const int16_t *side2,
                                  void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(n4, n3, n2, n1, z0, p1, p2, p3, p4) \
    for (int j = 0; j < TILE_SIZE; ++j) \
        dst[j] = (c[4] * (n4)[j - 4 * TILE_SIZE] + \
                  c[3] * (n3)[j - 3 * TILE_SIZE] + \
                  c[2] * (n2)[j - 2 * TILE_SIZE] + \
                  c[1] * (n1)[j - 1 * TILE_SIZE] + \
                  c[0] * (z0)[j + 0 * TILE_SIZE] + \
                  c[1] * (p1)[j + 1 * TILE_SIZE] + \
                  c[2] * (p2)[j + 2 * TILE_SIZE] + \
                  c[3] * (p3)[j + 3 * TILE_SIZE] + \
                  c[4] * (p4)[j + 4 * TILE_SIZE] + \
                  0x8000) >> 16; \
    dst += TILE_SIZE;

#define BEG_LINE(n) \
        LINE(n < 4 ? side1 : src, \
             n < 3 ? side1 : src, \
             n < 2 ? side1 : src, \
             n < 1 ? side1 : src, \
             src, src, src, src, src); \
        side1 += TILE_SIZE; \
        src += TILE_SIZE;

#define END_LINE(n) \
        LINE(src, src, src, src, src, \
             n < 1 ? side2 : src, \
             n < 2 ? side2 : src, \
             n < 3 ? side2 : src, \
             n < 4 ? side2 : src); \
        side2 += TILE_SIZE; \
        src += TILE_SIZE;

    side1 += TILE_SIZE * TILE_SIZE;
    BEG_LINE(0);
    BEG_LINE(1);
    BEG_LINE(2);
    BEG_LINE(3);
    for (int i = 4; i < TILE_SIZE - 4; ++i) {
        LINE(src, src, src, src, src, src, src, src, src);
        src += TILE_SIZE;
    }
    side2 -= 4 * TILE_SIZE;
    END_LINE(3);
    END_LINE(2);
    END_LINE(1);
    END_LINE(0);

#undef LINE
#undef BEG_LINE
#undef END_LINE
}

int DECORATE(blur1234_vert_solid_tile)(int16_t *dst,
                                       const int16_t *side1, int set, const int16_t *side2,
                                       void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(n4, n3, n2, n1, z0, p1, p2, p3, p4) \
    for (int j = 0; j < TILE_SIZE; ++j) { \
        ptr[j] = (c[4] * (n4) + \
                  c[3] * (n3) + \
                  c[2] * (n2) + \
                  c[1] * (n1) + \
                  c[0] * (z0) + \
                  c[1] * (p1) + \
                  c[2] * (p2) + \
                  c[3] * (p3) + \
                  c[4] * (p4) + \
                  0x8000) >> 16; \
        flag |= ptr[j] ^ val; \
    } \
    ptr += TILE_SIZE;

#define BEG_LINE(n) \
        LINE(n < 4 ? side1[j + (n - 4) * TILE_SIZE] : val, \
             n < 3 ? side1[j + (n - 3) * TILE_SIZE] : val, \
             n < 2 ? side1[j + (n - 2) * TILE_SIZE] : val, \
             n < 1 ? side1[j + (n - 1) * TILE_SIZE] : val, \
             val, val, val, val, val);

#define END_LINE(n) \
        LINE(val, val, val, val, val, \
             n < 1 ? side2[j + (1 - (n + 1)) * TILE_SIZE] : val, \
             n < 2 ? side2[j + (2 - (n + 1)) * TILE_SIZE] : val, \
             n < 3 ? side2[j + (3 - (n + 1)) * TILE_SIZE] : val, \
             n < 4 ? side2[j + (4 - (n + 1)) * TILE_SIZE] : val);

    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    side1 += TILE_SIZE * TILE_SIZE;
    BEG_LINE(0);
    BEG_LINE(1);
    BEG_LINE(2);
    BEG_LINE(3);
    ptr += (TILE_SIZE - 2 * 4) * TILE_SIZE;
    END_LINE(3);
    END_LINE(2);
    END_LINE(1);
    END_LINE(0);
    if (!flag)
        return 0;

    dst += 4 * TILE_SIZE;
    for (int i = 4; i < TILE_SIZE - 4; ++i) {
        for (int j = 0; j < TILE_SIZE; ++j)
            dst[j] = val;
        dst += TILE_SIZE;
    }
    return 1;

#undef LINE
#undef BEG_LINE
#undef END_LINE
}


void DECORATE(blur1235_horz_tile)(int16_t *dst,
                                  const int16_t *side1, const int16_t *src, const int16_t *side2,
                                  void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(n5, n3, n2, n1, z0, p1, p2, p3, p5) \
    *dst++ = (c[4] * (n5)[-5] + \
              c[3] * (n3)[-3] + \
              c[2] * (n2)[-2] + \
              c[1] * (n1)[-1] + \
              c[0] * (z0)[+0] + \
              c[1] * (p1)[+1] + \
              c[2] * (p2)[+2] + \
              c[3] * (p3)[+3] + \
              c[4] * (p5)[+5] + \
              0x8000) >> 16;

#define BEG_LINE(n) \
        LINE(n < 5 ? side1 : src, \
             n < 3 ? side1 : src, \
             n < 2 ? side1 : src, \
             n < 1 ? side1 : src, \
             src, src, src, src, src); \
        ++side1; \
        ++src;

#define END_LINE(n) \
        LINE(src, src, src, src, src, \
             n < 1 ? side2 : src, \
             n < 2 ? side2 : src, \
             n < 3 ? side2 : src, \
             n < 5 ? side2 : src); \
        ++side2; \
        ++src;

    side1 += 5;
    side2 -= 5;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE - 5;
        BEG_LINE(0);
        BEG_LINE(1);
        BEG_LINE(2);
        BEG_LINE(3);
        BEG_LINE(4);
        for (int j = 5; j < TILE_SIZE - 5; ++j) {
            LINE(src, src, src, src, src, src, src, src, src);
            ++src;
        }
        END_LINE(4);
        END_LINE(3);
        END_LINE(2);
        END_LINE(1);
        END_LINE(0);
        side2 += TILE_SIZE - 5;
    }

#undef LINE
#undef BEG_LINE
#undef END_LINE
}

int DECORATE(blur1235_horz_solid_tile)(int16_t *dst,
                                       const int16_t *side1, int set, const int16_t *side2,
                                       void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(res, n5, n3, n2, n1, z0, p1, p2, p3, p5) \
        ptr[res] = (c[4] * (n5) + \
                    c[3] * (n3) + \
                    c[2] * (n2) + \
                    c[1] * (n1) + \
                    c[0] * (z0) + \
                    c[1] * (p1) + \
                    c[2] * (p2) + \
                    c[3] * (p3) + \
                    c[4] * (p5) + \
                    0x8000) >> 16; \
        flag |= ptr[res] ^ val;

#define BEG_LINE(n) \
        LINE(n, \
             n < 5 ? side1[n - 5] : val, \
             n < 3 ? side1[n - 3] : val, \
             n < 2 ? side1[n - 2] : val, \
             n < 1 ? side1[n - 1] : val, \
             val, val, val, val, val);

#define END_LINE(n) \
        LINE(-(n + 1), \
             val, val, val, val, val, \
             n < 1 ? side2[1 - (n + 1)] : val, \
             n < 2 ? side2[2 - (n + 1)] : val, \
             n < 3 ? side2[3 - (n + 1)] : val, \
             n < 5 ? side2[5 - (n + 1)] : val);

    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        BEG_LINE(0);
        BEG_LINE(1);
        BEG_LINE(2);
        BEG_LINE(3);
        BEG_LINE(4);
        ptr += TILE_SIZE;
        END_LINE(4);
        END_LINE(3);
        END_LINE(2);
        END_LINE(1);
        END_LINE(0);
        side2 += TILE_SIZE;
    }
    if (!flag)
        return 0;

    for (int i = 0; i < TILE_SIZE; ++i) {
        dst += 5;
        for (int j = 5; j < TILE_SIZE - 5; ++j)
            *dst++ = val;
        dst += 5;
    }
    return 1;

#undef LINE
#undef BEG_LINE
#undef END_LINE
}

void DECORATE(blur1235_vert_tile)(int16_t *dst,
                                  const int16_t *side1, const int16_t *src, const int16_t *side2,
                                  void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(n5, n3, n2, n1, z0, p1, p2, p3, p5) \
    for (int j = 0; j < TILE_SIZE; ++j) \
        dst[j] = (c[4] * (n5)[j - 5 * TILE_SIZE] + \
                  c[3] * (n3)[j - 3 * TILE_SIZE] + \
                  c[2] * (n2)[j - 2 * TILE_SIZE] + \
                  c[1] * (n1)[j - 1 * TILE_SIZE] + \
                  c[0] * (z0)[j + 0 * TILE_SIZE] + \
                  c[1] * (p1)[j + 1 * TILE_SIZE] + \
                  c[2] * (p2)[j + 2 * TILE_SIZE] + \
                  c[3] * (p3)[j + 3 * TILE_SIZE] + \
                  c[4] * (p5)[j + 5 * TILE_SIZE] + \
                  0x8000) >> 16; \
    dst += TILE_SIZE;

#define BEG_LINE(n) \
        LINE(n < 5 ? side1 : src, \
             n < 3 ? side1 : src, \
             n < 2 ? side1 : src, \
             n < 1 ? side1 : src, \
             src, src, src, src, src); \
        side1 += TILE_SIZE; \
        src += TILE_SIZE;

#define END_LINE(n) \
        LINE(src, src, src, src, src, \
             n < 1 ? side2 : src, \
             n < 2 ? side2 : src, \
             n < 3 ? side2 : src, \
             n < 5 ? side2 : src); \
        side2 += TILE_SIZE; \
        src += TILE_SIZE;

    side1 += TILE_SIZE * TILE_SIZE;
    BEG_LINE(0);
    BEG_LINE(1);
    BEG_LINE(2);
    BEG_LINE(3);
    BEG_LINE(4);
    for (int i = 5; i < TILE_SIZE - 5; ++i) {
        LINE(src, src, src, src, src, src, src, src, src);
        src += TILE_SIZE;
    }
    side2 -= 5 * TILE_SIZE;
    END_LINE(4);
    END_LINE(3);
    END_LINE(2);
    END_LINE(1);
    END_LINE(0);

#undef LINE
#undef BEG_LINE
#undef END_LINE
}

int DECORATE(blur1235_vert_solid_tile)(int16_t *dst,
                                       const int16_t *side1, int set, const int16_t *side2,
                                       void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(n5, n3, n2, n1, z0, p1, p2, p3, p5) \
    for (int j = 0; j < TILE_SIZE; ++j) { \
        ptr[j] = (c[4] * (n5) + \
                  c[3] * (n3) + \
                  c[2] * (n2) + \
                  c[1] * (n1) + \
                  c[0] * (z0) + \
                  c[1] * (p1) + \
                  c[2] * (p2) + \
                  c[3] * (p3) + \
                  c[4] * (p5) + \
                  0x8000) >> 16; \
        flag |= ptr[j] ^ val; \
    } \
    ptr += TILE_SIZE;

#define BEG_LINE(n) \
        LINE(n < 5 ? side1[j + (n - 5) * TILE_SIZE] : val, \
             n < 3 ? side1[j + (n - 3) * TILE_SIZE] : val, \
             n < 2 ? side1[j + (n - 2) * TILE_SIZE] : val, \
             n < 1 ? side1[j + (n - 1) * TILE_SIZE] : val, \
             val, val, val, val, val);

#define END_LINE(n) \
        LINE(val, val, val, val, val, \
             n < 1 ? side2[j + (1 - (n + 1)) * TILE_SIZE] : val, \
             n < 2 ? side2[j + (2 - (n + 1)) * TILE_SIZE] : val, \
             n < 3 ? side2[j + (3 - (n + 1)) * TILE_SIZE] : val, \
             n < 5 ? side2[j + (5 - (n + 1)) * TILE_SIZE] : val);

    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    side1 += TILE_SIZE * TILE_SIZE;
    BEG_LINE(0);
    BEG_LINE(1);
    BEG_LINE(2);
    BEG_LINE(3);
    BEG_LINE(4);
    ptr += (TILE_SIZE - 2 * 5) * TILE_SIZE;
    END_LINE(4);
    END_LINE(3);
    END_LINE(2);
    END_LINE(1);
    END_LINE(0);
    if (!flag)
        return 0;

    dst += 5 * TILE_SIZE;
    for (int i = 5; i < TILE_SIZE - 5; ++i) {
        for (int j = 0; j < TILE_SIZE; ++j)
            dst[j] = val;
        dst += TILE_SIZE;
    }
    return 1;

#undef LINE
#undef BEG_LINE
#undef END_LINE
}


void DECORATE(blur1246_horz_tile)(int16_t *dst,
                                  const int16_t *side1, const int16_t *src, const int16_t *side2,
                                  void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(n6, n4, n2, n1, z0, p1, p2, p4, p6) \
    *dst++ = (c[4] * (n6)[-6] + \
              c[3] * (n4)[-4] + \
              c[2] * (n2)[-2] + \
              c[1] * (n1)[-1] + \
              c[0] * (z0)[+0] + \
              c[1] * (p1)[+1] + \
              c[2] * (p2)[+2] + \
              c[3] * (p4)[+4] + \
              c[4] * (p6)[+6] + \
              0x8000) >> 16;

#define BEG_LINE(n) \
        LINE(n < 6 ? side1 : src, \
             n < 4 ? side1 : src, \
             n < 2 ? side1 : src, \
             n < 1 ? side1 : src, \
             src, src, src, src, src); \
        ++side1; \
        ++src;

#define END_LINE(n) \
        LINE(src, src, src, src, src, \
             n < 1 ? side2 : src, \
             n < 2 ? side2 : src, \
             n < 4 ? side2 : src, \
             n < 6 ? side2 : src); \
        ++side2; \
        ++src;

    side1 += 6;
    side2 -= 6;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE - 6;
        BEG_LINE(0);
        BEG_LINE(1);
        BEG_LINE(2);
        BEG_LINE(3);
        BEG_LINE(4);
        BEG_LINE(5);
        for (int j = 6; j < TILE_SIZE - 6; ++j) {
            LINE(src, src, src, src, src, src, src, src, src);
            ++src;
        }
        END_LINE(5);
        END_LINE(4);
        END_LINE(3);
        END_LINE(2);
        END_LINE(1);
        END_LINE(0);
        side2 += TILE_SIZE - 6;
    }

#undef LINE
#undef BEG_LINE
#undef END_LINE
}

int DECORATE(blur1246_horz_solid_tile)(int16_t *dst,
                                       const int16_t *side1, int set, const int16_t *side2,
                                       void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(res, n6, n4, n2, n1, z0, p1, p2, p4, p6) \
        ptr[res] = (c[4] * (n6) + \
                    c[3] * (n4) + \
                    c[2] * (n2) + \
                    c[1] * (n1) + \
                    c[0] * (z0) + \
                    c[1] * (p1) + \
                    c[2] * (p2) + \
                    c[3] * (p4) + \
                    c[4] * (p6) + \
                    0x8000) >> 16; \
        flag |= ptr[res] ^ val;

#define BEG_LINE(n) \
        LINE(n, \
             n < 6 ? side1[n - 6] : val, \
             n < 4 ? side1[n - 4] : val, \
             n < 2 ? side1[n - 2] : val, \
             n < 1 ? side1[n - 1] : val, \
             val, val, val, val, val);

#define END_LINE(n) \
        LINE(-(n + 1), \
             val, val, val, val, val, \
             n < 1 ? side2[1 - (n + 1)] : val, \
             n < 2 ? side2[2 - (n + 1)] : val, \
             n < 4 ? side2[4 - (n + 1)] : val, \
             n < 6 ? side2[6 - (n + 1)] : val);

    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    for (int i = 0; i < TILE_SIZE; ++i) {
        side1 += TILE_SIZE;
        BEG_LINE(0);
        BEG_LINE(1);
        BEG_LINE(2);
        BEG_LINE(3);
        BEG_LINE(4);
        BEG_LINE(5);
        ptr += TILE_SIZE;
        END_LINE(5);
        END_LINE(4);
        END_LINE(3);
        END_LINE(2);
        END_LINE(1);
        END_LINE(0);
        side2 += TILE_SIZE;
    }
    if (!flag)
        return 0;

    for (int i = 0; i < TILE_SIZE; ++i) {
        dst += 6;
        for (int j = 6; j < TILE_SIZE - 6; ++j)
            *dst++ = val;
        dst += 6;
    }
    return 1;

#undef LINE
#undef BEG_LINE
#undef END_LINE
}

void DECORATE(blur1246_vert_tile)(int16_t *dst,
                                  const int16_t *side1, const int16_t *src, const int16_t *side2,
                                  void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(n6, n4, n2, n1, z0, p1, p2, p4, p6) \
    for (int j = 0; j < TILE_SIZE; ++j) \
        dst[j] = (c[4] * (n6)[j - 6 * TILE_SIZE] + \
                  c[3] * (n4)[j - 4 * TILE_SIZE] + \
                  c[2] * (n2)[j - 2 * TILE_SIZE] + \
                  c[1] * (n1)[j - 1 * TILE_SIZE] + \
                  c[0] * (z0)[j + 0 * TILE_SIZE] + \
                  c[1] * (p1)[j + 1 * TILE_SIZE] + \
                  c[2] * (p2)[j + 2 * TILE_SIZE] + \
                  c[3] * (p4)[j + 4 * TILE_SIZE] + \
                  c[4] * (p6)[j + 6 * TILE_SIZE] + \
                  0x8000) >> 16; \
    dst += TILE_SIZE;

#define BEG_LINE(n) \
        LINE(n < 6 ? side1 : src, \
             n < 4 ? side1 : src, \
             n < 2 ? side1 : src, \
             n < 1 ? side1 : src, \
             src, src, src, src, src); \
        side1 += TILE_SIZE; \
        src += TILE_SIZE;

#define END_LINE(n) \
        LINE(src, src, src, src, src, \
             n < 1 ? side2 : src, \
             n < 2 ? side2 : src, \
             n < 4 ? side2 : src, \
             n < 6 ? side2 : src); \
        side2 += TILE_SIZE; \
        src += TILE_SIZE;

    side1 += TILE_SIZE * TILE_SIZE;
    BEG_LINE(0);
    BEG_LINE(1);
    BEG_LINE(2);
    BEG_LINE(3);
    BEG_LINE(4);
    BEG_LINE(5);
    for (int i = 6; i < TILE_SIZE - 6; ++i) {
        LINE(src, src, src, src, src, src, src, src, src);
        src += TILE_SIZE;
    }
    side2 -= 6 * TILE_SIZE;
    END_LINE(5);
    END_LINE(4);
    END_LINE(3);
    END_LINE(2);
    END_LINE(1);
    END_LINE(0);

#undef LINE
#undef BEG_LINE
#undef END_LINE
}

int DECORATE(blur1246_vert_solid_tile)(int16_t *dst,
                                       const int16_t *side1, int set, const int16_t *side2,
                                       void *param)
{
    const int16_t *c = (const int16_t *)param;

#define LINE(n6, n4, n2, n1, z0, p1, p2, p4, p6) \
    for (int j = 0; j < TILE_SIZE; ++j) { \
        ptr[j] = (c[4] * (n6) + \
                  c[3] * (n4) + \
                  c[2] * (n2) + \
                  c[1] * (n1) + \
                  c[0] * (z0) + \
                  c[1] * (p1) + \
                  c[2] * (p2) + \
                  c[3] * (p4) + \
                  c[4] * (p6) + \
                  0x8000) >> 16; \
        flag |= ptr[j] ^ val; \
    } \
    ptr += TILE_SIZE;

#define BEG_LINE(n) \
        LINE(n < 6 ? side1[j + (n - 6) * TILE_SIZE] : val, \
             n < 4 ? side1[j + (n - 4) * TILE_SIZE] : val, \
             n < 2 ? side1[j + (n - 2) * TILE_SIZE] : val, \
             n < 1 ? side1[j + (n - 1) * TILE_SIZE] : val, \
             val, val, val, val, val);

#define END_LINE(n) \
        LINE(val, val, val, val, val, \
             n < 1 ? side2[j + (1 - (n + 1)) * TILE_SIZE] : val, \
             n < 2 ? side2[j + (2 - (n + 1)) * TILE_SIZE] : val, \
             n < 4 ? side2[j + (4 - (n + 1)) * TILE_SIZE] : val, \
             n < 6 ? side2[j + (6 - (n + 1)) * TILE_SIZE] : val);

    int16_t *ptr = dst;
    int16_t val = set ? 1 << 14 : 0, flag = 0;
    side1 += TILE_SIZE * TILE_SIZE;
    BEG_LINE(0);
    BEG_LINE(1);
    BEG_LINE(2);
    BEG_LINE(3);
    BEG_LINE(4);
    BEG_LINE(5);
    ptr += (TILE_SIZE - 2 * 6) * TILE_SIZE;
    END_LINE(5);
    END_LINE(4);
    END_LINE(3);
    END_LINE(2);
    END_LINE(1);
    END_LINE(0);
    if (!flag)
        return 0;

    dst += 6 * TILE_SIZE;
    for (int i = 6; i < TILE_SIZE - 6; ++i) {
        for (int j = 0; j < TILE_SIZE; ++j)
            dst[j] = val;
        dst += TILE_SIZE;
    }
    return 1;

#undef LINE
#undef BEG_LINE
#undef END_LINE
}


int DECORATE(shift_tile)(int16_t *dst,
                         const int16_t *src0, const int16_t *src1,
                         const int16_t *src2, const int16_t *src3,
                         int dx, int dy)
{
#define LERP(s0, s1, c) ((s0) + ((((s1) - (s0)) * (c) + 32) >> 6));
#define PIXEL(index, s0, s1) \
    next = LERP(s0, s1, sub_dx); \
    dst[index] = LERP(prev[index], next, sub_dy); \
    flag0 |= dst[index]; \
    flag1 |= dst[index] ^ full; \
    prev[index] = next;

    int sub_dx = dx & 63;
    int sub_dy = dy & 63;
    dx >>= 6;
    dy >>= 6;

    int16_t prev[TILE_SIZE];
    src0 += dy * TILE_SIZE;
    src1 += dy * TILE_SIZE;
    for (int j = dx; j < TILE_SIZE - 1; ++j)
        prev[j - dx] = LERP(src0[j], src0[j + 1], sub_dx);
    prev[TILE_SIZE - dx - 1] = LERP(src0[TILE_SIZE - 1], src1[0], sub_dx);
    for (int j = 0; j < dx; ++j)
        prev[j + TILE_SIZE - dx] = LERP(src1[j], src1[j + 1], sub_dx);

    const int16_t full = 1 << 14;
    int16_t next, flag0 = 0, flag1 = 0;
    for (int i = dy + 1; i < TILE_SIZE; ++i) {
        src0 += TILE_SIZE;
        src1 += TILE_SIZE;
        for (int j = dx; j < TILE_SIZE - 1; ++j) {
            PIXEL(j - dx, src0[j], src0[j + 1]);
        }
        PIXEL(TILE_SIZE - dx - 1, src0[TILE_SIZE - 1], src1[0]);
        for (int j = 0; j < dx; ++j) {
            PIXEL(j + TILE_SIZE - dx, src1[j], src1[j + 1]);
        }
        dst += TILE_SIZE;
    }
    for (int i = 0; i <= dy; ++i) {
        for (int j = dx; j < TILE_SIZE - 1; ++j) {
            PIXEL(j - dx, src2[j], src2[j + 1]);
        }
        PIXEL(TILE_SIZE - dx - 1, src2[TILE_SIZE - 1], src3[0]);
        for (int j = 0; j < dx; ++j) {
            PIXEL(j + TILE_SIZE - dx, src3[j], src3[j + 1]);
        }
        src2 += TILE_SIZE;
        src3 += TILE_SIZE;
        dst += TILE_SIZE;
    }
    return (flag0 ? 1 : 0) - (flag1 ? 1 : 0);

#undef LERP
#undef PIXEL
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
    .combine = { DECORATE(mul_tile), DECORATE(add_tile), DECORATE(sub_tile) },
    .shrink = { DECORATE(shrink_horz_tile), DECORATE(shrink_vert_tile) },
    .shrink_solid = { DECORATE(shrink_horz_solid_tile), DECORATE(shrink_vert_solid_tile) },
    .expand = {
        { DECORATE(expand_horz1_tile), DECORATE(expand_vert1_tile) },
        { DECORATE(expand_horz2_tile), DECORATE(expand_vert2_tile) },
    },
    .expand_solid_out = {
        { DECORATE(expand_horz1_solid1_tile), DECORATE(expand_vert1_solid1_tile) },
        { DECORATE(expand_horz2_solid1_tile), DECORATE(expand_vert2_solid1_tile) },
    },
    .expand_solid_in = {
        { DECORATE(expand_horz1_solid2_tile), DECORATE(expand_vert1_solid2_tile) },
        { DECORATE(expand_horz2_solid2_tile), DECORATE(expand_vert2_solid2_tile) },
    },
    .pre_blur = {
        { DECORATE(pre_blur1_horz_tile), DECORATE(pre_blur1_vert_tile) },
        { DECORATE(pre_blur2_horz_tile), DECORATE(pre_blur2_vert_tile) },
        { DECORATE(pre_blur3_horz_tile), DECORATE(pre_blur3_vert_tile) },
    },
    .pre_blur_solid = {
        { DECORATE(pre_blur1_horz_solid_tile), DECORATE(pre_blur1_vert_solid_tile) },
        { DECORATE(pre_blur2_horz_solid_tile), DECORATE(pre_blur2_vert_solid_tile) },
        { DECORATE(pre_blur3_horz_solid_tile), DECORATE(pre_blur3_vert_solid_tile) },
    },
    .main_blur = {
        { DECORATE(blur1234_horz_tile), DECORATE(blur1234_vert_tile) },
        { DECORATE(blur1235_horz_tile), DECORATE(blur1235_vert_tile) },
        { DECORATE(blur1246_horz_tile), DECORATE(blur1246_vert_tile) },
    },
    .main_blur_solid = {
        { DECORATE(blur1234_horz_solid_tile), DECORATE(blur1234_vert_solid_tile) },
        { DECORATE(blur1235_horz_solid_tile), DECORATE(blur1235_vert_solid_tile) },
        { DECORATE(blur1246_horz_solid_tile), DECORATE(blur1246_vert_solid_tile) },
    },
    .shift = DECORATE(shift_tile),
};
