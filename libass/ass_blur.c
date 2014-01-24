/*
 * Copyright (C) 2013 Vabishchevich Nikolay <vabnick@gmail.com>
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

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>



static void zerofill_horz(uint16_t *buf, int width, int height, int stride, int width_new)
{
    int x, y;
    for(y = 0; y < height; y++)
    {
        for(x = width; x < width_new; x++)buf[x] = 0;
        buf += stride;
    }
}

static void zerofill_vert(uint16_t *buf, int width, int height, int stride, int height_new)
{
    int x, y;
    buf += height * stride;
    for(y = height; y < height_new; y++)
    {
        for(x = 0; x < width; x++)buf[x] = 0;
        buf += stride;
    }
}


// start -> start + size / 2, size -> (size + 5) / 2
#define FILTER(dst, src, step,  f0, f1, f2, f3, f4, f5) \
    (*(dst) = (uint16_t)(15 + \
        (f0 ? (src)[(step) * 0] : 0) *  1 + \
        (f1 ? (src)[(step) * 1] : 0) *  5 + \
        (f2 ? (src)[(step) * 2] : 0) * 10 + \
        (f3 ? (src)[(step) * 3] : 0) * 10 + \
        (f4 ? (src)[(step) * 4] : 0) *  5 + \
        (f5 ? (src)[(step) * 5] : 0) *  1) >> 5)

static void shrink_horz(uint16_t *buf, int width, int height, int stride)
{
    const int delta = 4;
    if(width < delta)
        zerofill_horz(buf, width - delta, height, stride, 0);

    buf += width - 2;
    int x, y, n = (width + 5) / 2;
    for(y = 0; y < height; y++)
    {
        uint16_t *src = buf, *dst = buf + 3;
        buf += stride;

#define LINE(f0, f1, f2, f3, f4, f5) \
        FILTER(dst, src, 1,  f0, f1, f2, f3, f4, f5); \
        src -= 2;  dst--;

        LINE(1, 1, 0, 0, 0, 0)
        LINE(1, 1, 1, 1, 0, 0)

        for(x = n - 3; x >= 2 + (width & 1); x--)
        {
            LINE(1, 1, 1, 1, 1, 1)
        }

        if(width & 1)
        {
            LINE(0, 1, 1, 1, 1, 1)
            LINE(0, 0, 0, 1, 1, 1)
            LINE(0, 0, 0, 0, 0, 1)
        }
        else
        {
            LINE(0, 0, 1, 1, 1, 1)
            LINE(0, 0, 0, 0, 1, 1)
        }

#undef LINE
    }
}

static void shrink_vert(uint16_t *buf, int width, int height, int stride)
{
    const int delta = 4;
    if(height < delta)
        zerofill_vert(buf, width, height - delta, stride, 0);

    buf += (height - 2) * stride;
    uint16_t *dst = buf + 3 * stride;
    int x, y, n = (height + 5) / 2;

#define LINE(f0, f1, f2, f3, f4, f5) \
    for(x = 0; x < width; x++) \
        FILTER(dst + x, buf + x, stride,  f0, f1, f2, f3, f4, f5); \
    buf -= 2 * stride;  dst -= stride;

    LINE(1, 1, 0, 0, 0, 0)
    LINE(1, 1, 1, 1, 0, 0)

    for(y = n - 3; y >= 2 + (height & 1); y--)
    {
        LINE(1, 1, 1, 1, 1, 1)
    }

    if(height & 1)
    {
        LINE(0, 1, 1, 1, 1, 1)
        LINE(0, 0, 0, 1, 1, 1)
        LINE(0, 0, 0, 0, 0, 1)
    }
    else
    {
        LINE(0, 0, 1, 1, 1, 1)
        LINE(0, 0, 0, 0, 1, 1)
    }

#undef LINE
}

#undef FILTER


// start -> start - size - 3, size -> 2 * size + 4
#define FILTER(dst, src, step,  f0, f1, f2)  do { \
    uint16_t val0 = (uint16_t)(7 + \
        (f0 ? (src)[(step) * 0] : 0) *  5 + \
        (f1 ? (src)[(step) * 1] : 0) * 10 + \
        (f2 ? (src)[(step) * 2] : 0) *  1) >> 4; \
    uint16_t val1 = (uint16_t)(7 + \
        (f0 ? (src)[(step) * 0] : 0) *  1 + \
        (f1 ? (src)[(step) * 1] : 0) * 10 + \
        (f2 ? (src)[(step) * 2] : 0) *  5) >> 4; \
    (dst)[(step) * 0] = val0;  (dst)[(step) * 1] = val1; } while(0)

static void expand_horz(uint16_t *buf, int width, int height, int stride)
{
    const int delta = 2;
    if(width < delta)
        zerofill_horz(buf, width, height, stride, delta);

    int x, y;  buf -= 2;
    for(y = 0; y < height; y++)
    {
        uint16_t *src = buf, *dst = buf - width - 1;
        buf += stride;

#define LINE(f0, f1, f2) \
        FILTER(dst, src, 1,  f0, f1, f2); \
        src++;  dst += 2;

        LINE(0, 0, 1)
        LINE(0, 1, 1)

        for(x = delta; x < width; x++)
        {
            LINE(1, 1, 1)
        }

        LINE(1, 1, 0)
        LINE(1, 0, 0)

#undef LINE
    }
}

static void expand_vert(uint16_t *buf, int width, int height, int stride)
{
    const int delta = 2;
    if(height < delta)
        zerofill_vert(buf, width, height, stride, delta);

    int x, y;  buf -= 2 * stride;
    uint16_t *dst = buf - (height + 1) * stride;

#define LINE(f0, f1, f2) \
    for(x = 0; x < width; x++) \
        FILTER(dst + x, buf + x, stride,  f0, f1, f2); \
    buf += stride;  dst += 2 * stride;

    LINE(0, 0, 1)
    LINE(0, 1, 1)

    for(y = delta; y < height; y++)
    {
        LINE(1, 1, 1)
    }

    LINE(1, 1, 0)
    LINE(1, 0, 0)

#undef LINE
}

#undef FILTER


// start -> start - 2, size -> size + 2
#define FILTER(buf, step,  f0, f1, f2) \
    (*(buf) = (uint16_t)(1 + \
        (f0 ? (buf)[(step) * 0] : 0) * 1 + \
        (f1 ? (buf)[(step) * 1] : 0) * 2 + \
        (f2 ? (buf)[(step) * 2] : 0) * 1 + 1) >> 2)

static void prefilter1_horz(uint16_t *buf, int width, int height, int stride)
{
    const int delta = 2;
    if(width < delta)
        zerofill_horz(buf, width, height, stride, delta);

    int x, y;  buf -= delta;
    for(y = 0; y < height; y++)
    {
        uint16_t *ptr = buf;  buf += stride;

#define LINE(f0, f1, f2) \
        FILTER(ptr, 1,  f0, f1, f2);  ptr++;

        LINE(0, 0, 1)
        LINE(0, 1, 1)

        for(x = delta; x < width; x++)
        {
            LINE(1, 1, 1)
        }

        LINE(1, 1, 0)
        LINE(1, 0, 0)
    }

#undef LINE
}

static void prefilter1_vert(uint16_t *buf, int width, int height, int stride)
{
    const int delta = 2;
    if(height < delta)
        zerofill_vert(buf, width, height, stride, delta);

    int x, y;
    buf -= delta * stride;

#define LINE(f0, f1, f2) \
    for(x = 0; x < width; x++) \
        FILTER(buf + x, stride,  f0, f1, f2); \
    buf += stride;

    LINE(0, 0, 1)
    LINE(0, 1, 1)

    for(y = delta; y < height; y++)
    {
        LINE(1, 1, 1)
    }

    LINE(1, 1, 0)
    LINE(1, 0, 0)

#undef LINE
}

#undef FILTER


// start -> start - 4, size -> size + 4
#define FILTER(buf, step,  f0, f1, f2, f3, f4) \
    (*(buf) = (uint16_t)(7 + \
        (f0 ? (buf)[(step) * 0] : 0) * 1 + \
        (f1 ? (buf)[(step) * 1] : 0) * 4 + \
        (f2 ? (buf)[(step) * 2] : 0) * 6 + \
        (f3 ? (buf)[(step) * 3] : 0) * 4 + \
        (f4 ? (buf)[(step) * 4] : 0) * 1) >> 4)

static void prefilter2_horz(uint16_t *buf, int width, int height, int stride)
{
    const int delta = 4;
    if(width < delta)
        zerofill_horz(buf, width, height, stride, delta);

    int x, y;  buf -= delta;
    for(y = 0; y < height; y++)
    {
        uint16_t *ptr = buf;
        buf += stride;

#define LINE(f0, f1, f2, f3, f4) \
        FILTER(ptr, 1,  f0, f1, f2, f3, f4);  ptr++;

        LINE(0, 0, 0, 0, 1)
        LINE(0, 0, 0, 1, 1)
        LINE(0, 0, 1, 1, 1)
        LINE(0, 1, 1, 1, 1)

        for(x = delta; x < width; x++)
        {
            LINE(1, 1, 1, 1, 1)
        }

        LINE(1, 1, 1, 1, 0)
        LINE(1, 1, 1, 0, 0)
        LINE(1, 1, 0, 0, 0)
        LINE(1, 0, 0, 0, 0)

#undef LINE
    }
}

static void prefilter2_vert(uint16_t *buf, int width, int height, int stride)
{
    const int delta = 4;
    if(height < delta)
        zerofill_vert(buf, width, height, stride, delta);

    int x, y;
    buf -= delta * stride;

#define LINE(f0, f1, f2, f3, f4) \
    for(x = 0; x < width; x++) \
        FILTER(buf + x, stride,  f0, f1, f2, f3, f4); \
    buf += stride;

    LINE(0, 0, 0, 0, 1)
    LINE(0, 0, 0, 1, 1)
    LINE(0, 0, 1, 1, 1)
    LINE(0, 1, 1, 1, 1)

    for(y = delta; y < height; y++)
    {
        LINE(1, 1, 1, 1, 1)
    }

    LINE(1, 1, 1, 1, 0)
    LINE(1, 1, 1, 0, 0)
    LINE(1, 1, 0, 0, 0)
    LINE(1, 0, 0, 0, 0)

#undef LINE
}

#undef FILTER


// start -> start - 6, size -> size + 6
#define FILTER(buf, step,  f0, f1, f2, f3, f4, f5, f6) \
    (*(buf) = (31 + \
        (f0 ? (buf)[(step) * 0] : 0) *  1 + \
        (f1 ? (buf)[(step) * 1] : 0) *  6 + \
        (f2 ? (buf)[(step) * 2] : 0) * 15 + \
        (f3 ? (buf)[(step) * 3] : 0) * 20 + \
        (f4 ? (buf)[(step) * 4] : 0) * 15 + \
        (f5 ? (buf)[(step) * 5] : 0) *  6 + \
        (f6 ? (buf)[(step) * 6] : 0) *  1) >> 6)

static void prefilter3_horz(uint16_t *buf, int width, int height, int stride)
{
    const int delta = 6;
    if(width < delta)
        zerofill_horz(buf, width, height, stride, delta);

    int x, y;  buf -= delta;
    for(y = 0; y < height; y++)
    {
        uint16_t *ptr = buf;
        buf += stride;

#define LINE(f0, f1, f2, f3, f4, f5, f6) \
        FILTER(ptr, 1,  f0, f1, f2, f3, f4, f5, f6);  ptr++;

        LINE(0, 0, 0, 0, 0, 0, 1)
        LINE(0, 0, 0, 0, 0, 1, 1)
        LINE(0, 0, 0, 0, 1, 1, 1)
        LINE(0, 0, 0, 1, 1, 1, 1)
        LINE(0, 0, 1, 1, 1, 1, 1)
        LINE(0, 1, 1, 1, 1, 1, 1)

        for(x = delta; x < width; x++)
        {
            LINE(1, 1, 1, 1, 1, 1, 1)
        }

        LINE(1, 1, 1, 1, 1, 1, 0)
        LINE(1, 1, 1, 1, 1, 0, 0)
        LINE(1, 1, 1, 1, 0, 0, 0)
        LINE(1, 1, 1, 0, 0, 0, 0)
        LINE(1, 1, 0, 0, 0, 0, 0)
        LINE(1, 0, 0, 0, 0, 0, 0)

#undef LINE
    }
}

static void prefilter3_vert(uint16_t *buf, int width, int height, int stride)
{
    const int delta = 6;
    if(height < delta)
        zerofill_vert(buf, width, height, stride, delta);

    int x, y;
    buf -= delta * stride;

#define LINE(f0, f1, f2, f3, f4, f5, f6) \
    for(x = 0; x < width; x++) \
        FILTER(buf + x, stride,  f0, f1, f2, f3, f4, f5, f6); \
    buf += stride;

    LINE(0, 0, 0, 0, 0, 0, 1)
    LINE(0, 0, 0, 0, 0, 1, 1)
    LINE(0, 0, 0, 0, 1, 1, 1)
    LINE(0, 0, 0, 1, 1, 1, 1)
    LINE(0, 0, 1, 1, 1, 1, 1)
    LINE(0, 1, 1, 1, 1, 1, 1)

    for(y = delta; y < height; y++)
    {
        LINE(1, 1, 1, 1, 1, 1, 1)
    }

    LINE(1, 1, 1, 1, 1, 1, 0)
    LINE(1, 1, 1, 1, 1, 0, 0)
    LINE(1, 1, 1, 1, 0, 0, 0)
    LINE(1, 1, 1, 0, 0, 0, 0)
    LINE(1, 1, 0, 0, 0, 0, 0)
    LINE(1, 0, 0, 0, 0, 0, 0)

#undef LINE
}

#undef FILTER


// start -> start - 8, size -> size + 8
#define FILTER(buf, step, coeff,  f0, f1, f2, f3, f4, f5, f6, f7, f8) \
    (*(buf) = (0x7FFF + \
        (f0 ? (buf)[(step) * 0] : 0) * (coeff)[4] + \
        (f1 ? (buf)[(step) * 1] : 0) * (coeff)[3] + \
        (f2 ? (buf)[(step) * 2] : 0) * (coeff)[2] + \
        (f3 ? (buf)[(step) * 3] : 0) * (coeff)[1] + \
        (f4 ? (buf)[(step) * 4] : 0) * (coeff)[0] + \
        (f5 ? (buf)[(step) * 5] : 0) * (coeff)[1] + \
        (f6 ? (buf)[(step) * 6] : 0) * (coeff)[2] + \
        (f7 ? (buf)[(step) * 7] : 0) * (coeff)[3] + \
        (f8 ? (buf)[(step) * 8] : 0) * (coeff)[4]) >> 16)

static void filter1234_horz(uint16_t *buf, int width, int height, int stride, const int32_t coeff[5])
{
    const int delta = 8;
    if(width < delta)
        zerofill_horz(buf, width, height, stride, delta);

    int x, y;  buf -= delta;
    for(y = 0; y < height; y++)
    {
        uint16_t *ptr = buf;
        buf += stride;

#define LINE(f0, f1, f2, f3, f4, f5, f6, f7, f8) \
        FILTER(ptr, 1, coeff,  f0, f1, f2, f3, f4, f5, f6, f7, f8);  ptr++;

        LINE(0, 0, 0, 0, 0, 0, 0, 0, 1)
        LINE(0, 0, 0, 0, 0, 0, 0, 1, 1)
        LINE(0, 0, 0, 0, 0, 0, 1, 1, 1)
        LINE(0, 0, 0, 0, 0, 1, 1, 1, 1)
        LINE(0, 0, 0, 0, 1, 1, 1, 1, 1)
        LINE(0, 0, 0, 1, 1, 1, 1, 1, 1)
        LINE(0, 0, 1, 1, 1, 1, 1, 1, 1)
        LINE(0, 1, 1, 1, 1, 1, 1, 1, 1)

        for(x = delta; x < width; x++)
        {
            LINE(1, 1, 1, 1, 1, 1, 1, 1, 1)
        }

        LINE(1, 1, 1, 1, 1, 1, 1, 1, 0)
        LINE(1, 1, 1, 1, 1, 1, 1, 0, 0)
        LINE(1, 1, 1, 1, 1, 1, 0, 0, 0)
        LINE(1, 1, 1, 1, 1, 0, 0, 0, 0)
        LINE(1, 1, 1, 1, 0, 0, 0, 0, 0)
        LINE(1, 1, 1, 0, 0, 0, 0, 0, 0)
        LINE(1, 1, 0, 0, 0, 0, 0, 0, 0)
        LINE(1, 0, 0, 0, 0, 0, 0, 0, 0)

#undef LINE
    }
}

static void filter1234_vert(uint16_t *buf, int width, int height, int stride, const int32_t coeff[5])
{
    const int delta = 8;
    if(height < delta)
        zerofill_vert(buf, width, height, stride, delta);

    int x, y;
    buf -= delta * stride;

#define LINE(f0, f1, f2, f3, f4, f5, f6, f7, f8) \
    for(x = 0; x < width; x++) \
        FILTER(buf + x, stride, coeff,  f0, f1, f2, f3, f4, f5, f6, f7, f8); \
    buf += stride;

    LINE(0, 0, 0, 0, 0, 0, 0, 0, 1)
    LINE(0, 0, 0, 0, 0, 0, 0, 1, 1)
    LINE(0, 0, 0, 0, 0, 0, 1, 1, 1)
    LINE(0, 0, 0, 0, 0, 1, 1, 1, 1)
    LINE(0, 0, 0, 0, 1, 1, 1, 1, 1)
    LINE(0, 0, 0, 1, 1, 1, 1, 1, 1)
    LINE(0, 0, 1, 1, 1, 1, 1, 1, 1)
    LINE(0, 1, 1, 1, 1, 1, 1, 1, 1)

    for(y = delta; y < height; y++)
    {
        LINE(1, 1, 1, 1, 1, 1, 1, 1, 1)
    }

    LINE(1, 1, 1, 1, 1, 1, 1, 1, 0)
    LINE(1, 1, 1, 1, 1, 1, 1, 0, 0)
    LINE(1, 1, 1, 1, 1, 1, 0, 0, 0)
    LINE(1, 1, 1, 1, 1, 0, 0, 0, 0)
    LINE(1, 1, 1, 1, 0, 0, 0, 0, 0)
    LINE(1, 1, 1, 0, 0, 0, 0, 0, 0)
    LINE(1, 1, 0, 0, 0, 0, 0, 0, 0)
    LINE(1, 0, 0, 0, 0, 0, 0, 0, 0)

#undef LINE
}

#undef FILTER


// start -> start - 10, size -> size + 10
#define FILTER(buf, step, coeff,  f0, f1, f2, f3, f4, f5, f6, f7, f8) \
    (*(buf) = (0x7FFF + \
        (f0 ? (buf)[(step) *  0] : 0) * (coeff)[4] + \
        (f1 ? (buf)[(step) *  2] : 0) * (coeff)[3] + \
        (f2 ? (buf)[(step) *  3] : 0) * (coeff)[2] + \
        (f3 ? (buf)[(step) *  4] : 0) * (coeff)[1] + \
        (f4 ? (buf)[(step) *  5] : 0) * (coeff)[0] + \
        (f5 ? (buf)[(step) *  6] : 0) * (coeff)[1] + \
        (f6 ? (buf)[(step) *  7] : 0) * (coeff)[2] + \
        (f7 ? (buf)[(step) *  8] : 0) * (coeff)[3] + \
        (f8 ? (buf)[(step) * 10] : 0) * (coeff)[4]) >> 16)

static void filter1235_horz(uint16_t *buf, int width, int height, int stride, const int32_t coeff[5])
{
    const int delta = 10;
    if(width < delta)
        zerofill_horz(buf, width, height, stride, delta);

    int x, y;  buf -= delta;
    for(y = 0; y < height; y++)
    {
        uint16_t *ptr = buf;
        buf += stride;

#define LINE(f0, f1, f2, f3, f4, f5, f6, f7, f8) \
        FILTER(ptr, 1, coeff,  f0, f1, f2, f3, f4, f5, f6, f7, f8);  ptr++;

        LINE(0, 0, 0, 0, 0, 0, 0, 0, 1)
        LINE(0, 0, 0, 0, 0, 0, 0, 0, 1)
        LINE(0, 0, 0, 0, 0, 0, 0, 1, 1)
        LINE(0, 0, 0, 0, 0, 0, 1, 1, 1)
        LINE(0, 0, 0, 0, 0, 1, 1, 1, 1)
        LINE(0, 0, 0, 0, 1, 1, 1, 1, 1)
        LINE(0, 0, 0, 1, 1, 1, 1, 1, 1)
        LINE(0, 0, 1, 1, 1, 1, 1, 1, 1)
        LINE(0, 1, 1, 1, 1, 1, 1, 1, 1)
        LINE(0, 1, 1, 1, 1, 1, 1, 1, 1)

        for(x = delta; x < width; x++)
        {
            LINE(1, 1, 1, 1, 1, 1, 1, 1, 1)
        }

        LINE(1, 1, 1, 1, 1, 1, 1, 1, 0)
        LINE(1, 1, 1, 1, 1, 1, 1, 1, 0)
        LINE(1, 1, 1, 1, 1, 1, 1, 0, 0)
        LINE(1, 1, 1, 1, 1, 1, 0, 0, 0)
        LINE(1, 1, 1, 1, 1, 0, 0, 0, 0)
        LINE(1, 1, 1, 1, 0, 0, 0, 0, 0)
        LINE(1, 1, 1, 0, 0, 0, 0, 0, 0)
        LINE(1, 1, 0, 0, 0, 0, 0, 0, 0)
        LINE(1, 0, 0, 0, 0, 0, 0, 0, 0)
        LINE(1, 0, 0, 0, 0, 0, 0, 0, 0)

#undef LINE
    }
}

static void filter1235_vert(uint16_t *buf, int width, int height, int stride, const int32_t coeff[5])
{
    const int delta = 10;
    if(height < delta)
        zerofill_vert(buf, width, height, stride, delta);

    int x, y;
    buf -= delta * stride;

#define LINE(f0, f1, f2, f3, f4, f5, f6, f7, f8) \
    for(x = 0; x < width; x++) \
        FILTER(buf + x, stride, coeff,  f0, f1, f2, f3, f4, f5, f6, f7, f8); \
    buf += stride;

    LINE(0, 0, 0, 0, 0, 0, 0, 0, 1)
    LINE(0, 0, 0, 0, 0, 0, 0, 0, 1)
    LINE(0, 0, 0, 0, 0, 0, 0, 1, 1)
    LINE(0, 0, 0, 0, 0, 0, 1, 1, 1)
    LINE(0, 0, 0, 0, 0, 1, 1, 1, 1)
    LINE(0, 0, 0, 0, 1, 1, 1, 1, 1)
    LINE(0, 0, 0, 1, 1, 1, 1, 1, 1)
    LINE(0, 0, 1, 1, 1, 1, 1, 1, 1)
    LINE(0, 1, 1, 1, 1, 1, 1, 1, 1)
    LINE(0, 1, 1, 1, 1, 1, 1, 1, 1)

    for(y = delta; y < height; y++)
    {
        LINE(1, 1, 1, 1, 1, 1, 1, 1, 1)
    }

    LINE(1, 1, 1, 1, 1, 1, 1, 1, 0)
    LINE(1, 1, 1, 1, 1, 1, 1, 1, 0)
    LINE(1, 1, 1, 1, 1, 1, 1, 0, 0)
    LINE(1, 1, 1, 1, 1, 1, 0, 0, 0)
    LINE(1, 1, 1, 1, 1, 0, 0, 0, 0)
    LINE(1, 1, 1, 1, 0, 0, 0, 0, 0)
    LINE(1, 1, 1, 0, 0, 0, 0, 0, 0)
    LINE(1, 1, 0, 0, 0, 0, 0, 0, 0)
    LINE(1, 0, 0, 0, 0, 0, 0, 0, 0)
    LINE(1, 0, 0, 0, 0, 0, 0, 0, 0)

#undef LINE
}

#undef FILTER


// start -> start - 12, size -> size + 12
#define FILTER(buf, step, coeff,  f0, f1, f2, f3, f4, f5, f6, f7, f8) \
    (*(buf) = (0x7FFF + \
        (f0 ? (buf)[(step) *  0] : 0) * (coeff)[4] + \
        (f1 ? (buf)[(step) *  2] : 0) * (coeff)[3] + \
        (f2 ? (buf)[(step) *  4] : 0) * (coeff)[2] + \
        (f3 ? (buf)[(step) *  5] : 0) * (coeff)[1] + \
        (f4 ? (buf)[(step) *  6] : 0) * (coeff)[0] + \
        (f5 ? (buf)[(step) *  7] : 0) * (coeff)[1] + \
        (f6 ? (buf)[(step) *  8] : 0) * (coeff)[2] + \
        (f7 ? (buf)[(step) * 10] : 0) * (coeff)[3] + \
        (f8 ? (buf)[(step) * 12] : 0) * (coeff)[4]) >> 16)

static void filter1246_horz(uint16_t *buf, int width, int height, int stride, const int32_t coeff[5])
{
    const int delta = 12;
    if(width < delta)
        zerofill_horz(buf, width, height, stride, delta);

    int x, y;  buf -= delta;
    for(y = 0; y < height; y++)
    {
        uint16_t *ptr = buf;
        buf += stride;

#define LINE(f0, f1, f2, f3, f4, f5, f6, f7, f8) \
        FILTER(ptr, 1, coeff,  f0, f1, f2, f3, f4, f5, f6, f7, f8);  ptr++;

        LINE(0, 0, 0, 0, 0, 0, 0, 0, 1)
        LINE(0, 0, 0, 0, 0, 0, 0, 0, 1)
        LINE(0, 0, 0, 0, 0, 0, 0, 1, 1)
        LINE(0, 0, 0, 0, 0, 0, 0, 1, 1)
        LINE(0, 0, 0, 0, 0, 0, 1, 1, 1)
        LINE(0, 0, 0, 0, 0, 1, 1, 1, 1)
        LINE(0, 0, 0, 0, 1, 1, 1, 1, 1)
        LINE(0, 0, 0, 1, 1, 1, 1, 1, 1)
        LINE(0, 0, 1, 1, 1, 1, 1, 1, 1)
        LINE(0, 0, 1, 1, 1, 1, 1, 1, 1)
        LINE(0, 1, 1, 1, 1, 1, 1, 1, 1)
        LINE(0, 1, 1, 1, 1, 1, 1, 1, 1)

        for(x = delta; x < width; x++)
        {
            LINE(1, 1, 1, 1, 1, 1, 1, 1, 1)
        }

        LINE(1, 1, 1, 1, 1, 1, 1, 1, 0)
        LINE(1, 1, 1, 1, 1, 1, 1, 1, 0)
        LINE(1, 1, 1, 1, 1, 1, 1, 0, 0)
        LINE(1, 1, 1, 1, 1, 1, 1, 0, 0)
        LINE(1, 1, 1, 1, 1, 1, 0, 0, 0)
        LINE(1, 1, 1, 1, 1, 0, 0, 0, 0)
        LINE(1, 1, 1, 1, 0, 0, 0, 0, 0)
        LINE(1, 1, 1, 0, 0, 0, 0, 0, 0)
        LINE(1, 1, 0, 0, 0, 0, 0, 0, 0)
        LINE(1, 1, 0, 0, 0, 0, 0, 0, 0)
        LINE(1, 0, 0, 0, 0, 0, 0, 0, 0)
        LINE(1, 0, 0, 0, 0, 0, 0, 0, 0)

#undef LINE
    }
}

static void filter1246_vert(uint16_t *buf, int width, int height, int stride, const int32_t coeff[5])
{
    const int delta = 12;
    if(height < delta)
        zerofill_vert(buf, width, height, stride, delta);

    int x, y;
    buf -= delta * stride;

#define LINE(f0, f1, f2, f3, f4, f5, f6, f7, f8) \
    for(x = 0; x < width; x++) \
        FILTER(buf + x, stride, coeff,  f0, f1, f2, f3, f4, f5, f6, f7, f8); \
    buf += stride;

    LINE(0, 0, 0, 0, 0, 0, 0, 0, 1)
    LINE(0, 0, 0, 0, 0, 0, 0, 0, 1)
    LINE(0, 0, 0, 0, 0, 0, 0, 1, 1)
    LINE(0, 0, 0, 0, 0, 0, 0, 1, 1)
    LINE(0, 0, 0, 0, 0, 0, 1, 1, 1)
    LINE(0, 0, 0, 0, 0, 1, 1, 1, 1)
    LINE(0, 0, 0, 0, 1, 1, 1, 1, 1)
    LINE(0, 0, 0, 1, 1, 1, 1, 1, 1)
    LINE(0, 0, 1, 1, 1, 1, 1, 1, 1)
    LINE(0, 0, 1, 1, 1, 1, 1, 1, 1)
    LINE(0, 1, 1, 1, 1, 1, 1, 1, 1)
    LINE(0, 1, 1, 1, 1, 1, 1, 1, 1)

    for(y = delta; y < height; y++)
    {
        LINE(1, 1, 1, 1, 1, 1, 1, 1, 1)
    }

    LINE(1, 1, 1, 1, 1, 1, 1, 1, 0)
    LINE(1, 1, 1, 1, 1, 1, 1, 1, 0)
    LINE(1, 1, 1, 1, 1, 1, 1, 0, 0)
    LINE(1, 1, 1, 1, 1, 1, 1, 0, 0)
    LINE(1, 1, 1, 1, 1, 1, 0, 0, 0)
    LINE(1, 1, 1, 1, 1, 0, 0, 0, 0)
    LINE(1, 1, 1, 1, 0, 0, 0, 0, 0)
    LINE(1, 1, 1, 0, 0, 0, 0, 0, 0)
    LINE(1, 1, 0, 0, 0, 0, 0, 0, 0)
    LINE(1, 1, 0, 0, 0, 0, 0, 0, 0)
    LINE(1, 0, 0, 0, 0, 0, 0, 0, 0)
    LINE(1, 0, 0, 0, 0, 0, 0, 0, 0)

#undef LINE
}

#undef FILTER



static const double pi = 3.14159265358979323846264338327950288;

static void calc_gauss(double *res, double r2, int n)  // TODO: optimize
{
    int i;
    double alpha = 0.5 / r2, norm = sqrt(alpha / pi);
    for(i = 0; i < n; i++)res[i] = norm * exp(-alpha * (i * i));
}

static void calc_matrix(double mat[4][4], const double *base_exp, const int *ind)
{
    int i, j, k;
    for(i = 0; i < 4; i++)
    {
        mat[i][i] = base_exp[2 * ind[i]] + 3 * base_exp[0] - 4 * base_exp[ind[i]];
        for(j = i + 1; j < 4; j++)mat[i][j] = mat[j][i] =
            base_exp[ind[i] + ind[j]] + base_exp[ind[j] - ind[i]] +
            2 * (base_exp[0] - base_exp[ind[i]] - base_exp[ind[j]]);
    }

    for(k = 0; k < 4; k++)  // invert transpose
    {
        int ip = k, jp = k;  // pivot
        double z = 1 / mat[ip][jp];  mat[ip][jp] = 1;
        for(i = 0; i < 4; i++)if(i != ip)
        {
            double mul = mat[i][jp] * z;  mat[i][jp] = 0;
            for(j = 0; j < 4; j++)mat[i][j] -= mat[ip][j] * mul;
        }
        for(j = 0; j < 4; j++)mat[ip][j] *= z;
    }
}

static void calc_coeff(double mu[4], const int ind[4], int prefilter, int level, double r2)
{
    double mul = pow(0.25, level);
    double r2b = (5.0 / 6) * (1 - mul);
    r2b += 0.5 * prefilter;  r2 *= mul;

    double base_exp[13], mat[4][4];
    calc_gauss(base_exp, 2 * r2b, 13);  // TODO: overkill
    calc_matrix(mat, base_exp, ind);

    double avg_exp[7];
    calc_gauss(avg_exp, r2 + r2b, 7);  // TODO: overkill

    int i, j;
    double vec[4];
    for(i = 0; i < 4; i++)
        vec[i] = 2 * (base_exp[0] - base_exp[ind[i]] - avg_exp[0] + avg_exp[ind[i]]);

    for(i = 0; i < 4; i++)
    {
        double res = 0;
        for(j = 0; j < 4; j++)res += mat[i][j] * vec[j];
        mu[i] = res;
    }
}

static void (*prefilter_vert[])(uint16_t *, int, int, int) =
    {0, prefilter1_vert, prefilter2_vert, prefilter3_vert};
static void (*prefilter_horz[])(uint16_t *, int, int, int) =
    {0, prefilter1_horz, prefilter2_horz, prefilter3_horz};
static void (*filter_vert[])(uint16_t *, int, int, int, const int32_t *) =
    {filter1234_vert, filter1235_vert, filter1246_vert};
static void (*filter_horz[])(uint16_t *, int, int, int, const int32_t *) =
    {filter1234_horz, filter1235_horz, filter1246_horz};

void gaussian_blur(uint8_t *img, int img_width, int img_height, int img_stride, int border, double r2)
{
    static const int ind[][4] =
    {
        {1, 2, 3, 4},
        {1, 2, 3, 5},
        {1, 2, 4, 6},
    };

    int level, prefilter, filter;
    double mu[4];

    int i;
    if(r2 < 2.4)  // TODO: compact filters
    {
        level = prefilter = filter = 0;

        if(r2 < 0.5)
        {
            mu[1] = 0.17 * r2 * r2 * r2;
            mu[0] = r2 - 4 * mu[1];
            mu[2] = mu[3] = 0;
        }
        else
        {
            double alpha = 0.5 / r2, norm = 2 * sqrt(alpha / pi);
            for(i = 1; i <= 4; i++)
                mu[i - 1] = norm * exp(-alpha * (i * i));
        }
    }
    else
    {
        if(r2 < 8)
        {
            level = 0;

            if(r2 < 3.5)prefilter = 1;
            else if(r2 < 5.3)prefilter = 2;
            else prefilter = 3;

            filter = prefilter - 1;
        }
        else
        {
            double val = r2 + 1;  level = 1;
            for(; val > 33; val /= 4)level++;

            if(val < 14.5)prefilter = 0;
            else if(val < 22.5)prefilter = 1;
            else prefilter = 2;

            filter = prefilter;
        }

        calc_coeff(mu, ind[filter], prefilter, level, r2);
    }


    // TODO: UB -- signed shift
    int width  = img_width  - 2 * border;
    int height = img_height - 2 * border;
    int shr_w = ((width  - 5) >> level) + 5;
    int shr_h = ((height - 5) >> level) + 5;

    int filter_size = 4 + filter + prefilter;
    int total_w = ((shr_w + 2 * filter_size + 4) << level) - 4;
    int total_h = ((shr_h + 2 * filter_size + 4) << level) - 4;
    int res_offs = ((filter_size + 4) << level) - 3 * level - 4;
    int offs_x = total_w - 3 * level - width;
    int offs_y = total_h - 3 * level - height;

    int extra_space = 2 * (4 + filter - prefilter);
    if(shr_w + level < extra_space)total_w += extra_space - shr_w - level;
    if(shr_h + level < extra_space)total_h += extra_space - shr_h - level;
    int stride = (total_w + 7) & ~7;  // SSE friendly
    
    int32_t coeff[5] = {0x10000};
    for(i = 0; i < 4; i++)
        coeff[0] -= 2 * (coeff[i + 1] = (int)floor(0x8000 * mu[i] + 0.5));

    //printf("COEFF: %d %d %d %d %d\n", coeff[0], coeff[1], coeff[2], coeff[3], coeff[4]);


    uint16_t *buf = malloc(stride * total_h * sizeof(uint16_t));

    int x, y;
    uint8_t *img_ptr = img + border * (img_stride + 1);
    uint16_t *cur = buf + offs_y * stride + offs_x, *ptr = cur;
    for(y = 0; y < height; y++)
    {
        for(x = 0; x < width; x++)ptr[x] = 8 * img_ptr[x];
        img_ptr += img_stride;  ptr += stride;
    }

    for(i = 0; i < level; i++)
    {
        shrink_vert(cur, width, height, stride);
        cur += (height / 2) * stride;
        height = (height + 5) / 2;
    }
    for(i = 0; i < level; i++)
    {
        shrink_horz(cur, width, height, stride);
        cur += width / 2;
        width = (width + 5) / 2;
    }

    if(prefilter)
    {
        (*prefilter_horz[prefilter])(cur, width, height, stride);
        cur -= 2 * prefilter;
        width += 2 * prefilter;

        (*prefilter_vert[prefilter])(cur, width, height, stride);
        cur -= 2 * prefilter * stride;
        height += 2 * prefilter;
    }

    (*filter_horz[filter])(cur, width, height, stride, coeff);
    cur -= 2 * (filter + 4);
    width += 2 * (filter + 4);

    (*filter_vert[filter])(cur, width, height, stride, coeff);
    cur -= 2 * (filter + 4) * stride;
    height += 2 * (filter + 4);

    for(i = 0; i < level; i++)
    {
        expand_horz(cur, width, height, stride);
        cur -= width + 3;
        width = 2 * width + 4;
    }
    for(i = 0; i < level; i++)
    {
        expand_vert(cur, width, height, stride);
        cur -= (height + 3) * stride;
        height = 2 * height + 4;
    }
    assert(cur == buf);


    /*ptr = buf;
    int o_x = offs_x - res_offs;
    int o_y = offs_y - res_offs;
    double norm = 1023 / (2 * pi * r2);
    for(y = 0; y < height; y++)
    {
        for(x = 0; x < width; x++)
        {
            double dx = x - o_x, dy = y - o_y;
            double target = norm * exp(-0.5 * (dx * dx + dy * dy) / r2);
            ptr[x] = (int)fabs(10 * (ptr[x] - target) + 0.5);
            //ptr[x] = (int)(target + 0.5);
        }
        ptr += stride;
    }
    test_print(cur, width, height, stride);*/


    offs_x -= res_offs + border;
    offs_y -= res_offs + border;
    int img_x = offs_x < 0 ? -offs_x : 0;
    int img_y = offs_y < 0 ? -offs_y : 0;
    width -= offs_x;  height -= offs_y;
    if(width > img_width)width = img_width;
    if(height > img_height)height = img_height;
    offs_x += img_x;  offs_y += img_y;
    width -= img_x;  height -= img_y;

    ptr = buf + offs_y * stride + offs_x;
    img_ptr = img + img_y * img_stride + img_x;
    for(y = 0; y < height; y++)  // TODO: dither?
    {
        for(x = 0; x < width; x++)img_ptr[x] = (ptr[x] + 3) / 8;
        img_ptr += img_stride;  ptr += stride;
    }

    free(buf);
}
