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

#include "ass_utils.h"
#include "ass_rasterizer.h"
#include <assert.h>



int16_t empty_tile[32 * 32], solid_tile[32 * 32];

void prepare_solid_tiles()
{
    for (int k = 0; k < 32 * 32; k++) {
        empty_tile[k] = 0;
        solid_tile[k] = 1 << 14;
    }
}

void ass_finalize_solid_c(uint8_t *buf, ptrdiff_t stride, int size_order, int set)
{
    int size = 1 << size_order;
    int8_t value = set ? 255 : 0;
    for (int j = 0; j < size; ++j) {
        for (int i = 0; i < size; ++i)
            buf[i] = value;
        buf += stride;
    }
}


#define TILE_ORDER 4
#define TILE_SIZE 16
#define DECORATE(func) ass_##func##16_c
#include "ass_tile_func.h"
#undef TILE_ORDER
#undef TILE_SIZE
#undef DECORATE

#define TILE_ORDER 5
#define TILE_SIZE 32
#define DECORATE(func) ass_##func##32_c
#include "ass_tile_func.h"
#undef TILE_ORDER
#undef TILE_SIZE
#undef DECORATE
