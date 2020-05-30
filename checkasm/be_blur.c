/*
 * Copyright (C) 2020 rcombs <rcombs@rcombs.me>
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

#include "checkasm.h"

#include "ass_bitmap.h"

#define WIDTH  61
#define HEIGHT 8
#define STRIDE 64

static void check_be_blur(BeBlurFunc func)
{
    if (check_func(func, "be_blur")) {
        ALIGN(uint8_t buf_ref[STRIDE * HEIGHT], 32);
        ALIGN(uint8_t buf_new[STRIDE * HEIGHT], 32);
        ALIGN(uint16_t tmp[STRIDE * 2], 32);
        declare_func(void,
                     uint8_t *buf, intptr_t w, intptr_t h,
                     intptr_t stride, uint16_t *tmp);

        memset(buf_ref, 0, sizeof(buf_ref));
        memset(buf_new, 0, sizeof(buf_new));

        for (int y = 2; y < HEIGHT - 2; y++) {
            for (int x = 2; x < WIDTH - 2; x++)
                buf_ref[x + y * STRIDE] = buf_new[x + y * STRIDE] = rnd();
        }

        memset(tmp, 0, sizeof(tmp));
        call_ref(buf_ref, WIDTH, HEIGHT,
                 STRIDE, tmp);
        memset(tmp, 0, sizeof(tmp));
        call_new(buf_new, WIDTH, HEIGHT,
                 STRIDE, tmp);

        for (int i = 0; i < HEIGHT; i++) {
            if (memcmp(buf_ref + STRIDE * i,
                       buf_new + STRIDE * i,
                       WIDTH)) {
                fail();
                break;
            }
        }

        bench_new(buf_new, WIDTH, HEIGHT, STRIDE, tmp);
    }

    report("be_blur");
}

void checkasm_check_be_blur(const BitmapEngine *engine)
{
    check_be_blur(engine->be_blur);
}
