/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2011 Grigori Goronzy <greg@chown.ath.cx>
 * Copyright (c) 2011-2014, Yu Zhuohuang <yuzhuohuang@qq.com>
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

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <assert.h>
#include <ft2build.h>
#include FT_GLYPH_H
#include FT_OUTLINE_H

#include "ass_utils.h"
#include "ass_bitmap.h"
#include "ass_render.h"


#define ALIGN           C_ALIGN_ORDER
#define DECORATE(func)  ass_##func##_c
#include "ass_func_template.h"
#undef ALIGN
#undef DECORATE

#if (defined(__i386__) || defined(__x86_64__)) && CONFIG_ASM

#define ALIGN           4
#define DECORATE(func)  ass_##func##_sse2
#include "ass_func_template.h"
#undef ALIGN
#undef DECORATE

#define ALIGN           5
#define DECORATE(func)  ass_##func##_avx2
#include "ass_func_template.h"
#undef ALIGN
#undef DECORATE

#endif


static const unsigned base = 256;

struct ass_synth_priv {
    size_t tmp_allocated;
    void *tmp;

    int g_r;
    int g_w;

    int b_r;
    int b_w;

    double *g0;
    unsigned *g;
    unsigned *gt2;

    unsigned *b;
    unsigned *bt2;
    unsigned *bl;
    unsigned *blt2;

    double radius;
};

static bool generate_gaussian_tables(ASS_SynthPriv *priv, double radius)
{
    double A = log(1.0 / base) / (radius * radius * 2);
    int mx, i;
    double volume_diff, volume_factor = 0;
    unsigned volume;

    if (radius < 0)
        return false;
    if (radius + 2.0 > INT_MAX / 2)
        radius = INT_MAX / 2;

    if (priv->radius == radius)
        return true;
    else
        priv->radius = radius;

    priv->g_r = ceil(radius);
    priv->g_w = 2 * priv->g_r + 1;

    if (priv->g_r) {
        priv->g0 = ass_realloc_array(priv->g0, priv->g_w, sizeof(double));
        priv->g = ass_realloc_array(priv->g, priv->g_w, sizeof(unsigned));
        priv->gt2 = ass_realloc_array(priv->gt2, priv->g_w, 256 * sizeof(unsigned));
        if (!priv->g || !priv->g0 || !priv->gt2) {
            free(priv->g0);
            free(priv->g);
            free(priv->gt2);
            return false;
        }
    }

    if (priv->g_r) {
        // exact gaussian curve
        for (i = 0; i < priv->g_w; ++i) {
            priv->g0[i] = exp(A * (i - priv->g_r) * (i - priv->g_r));
        }

        // integer gaussian curve with volume = 65536
        for (volume_diff = 10000000; volume_diff > 0.0000001;
             volume_diff *= 0.5) {
            volume_factor += volume_diff;
            volume = 0;
            for (i = 0; i < priv->g_w; ++i) {
                priv->g[i] = (unsigned) (priv->g0[i] * volume_factor + .5);
                volume += priv->g[i];
            }
            if (volume > 65536)
                volume_factor -= volume_diff;
        }
        volume = 0;
        for (i = 0; i < priv->g_w; ++i) {
            priv->g[i] = (unsigned) (priv->g0[i] * volume_factor + .5);
            volume += priv->g[i];
        }

        // gauss table:
        for (mx = 0; mx < priv->g_w; mx++) {
            for (i = 0; i < 256; i++) {
                priv->gt2[mx + i * priv->g_w] = i * priv->g[mx];
            }
        }
    }

    return true;
}

static bool generate_be_tables(ASS_SynthPriv *priv, double radius)
{
    double inside = ceil(radius) - radius, outside = 1 - inside;
    int mx, i;
    double volume_diff, volume_factor = 0;
    unsigned volume;

    if (radius < 0)
        return false;
    if (radius + 2.0 > INT_MAX / 2)
        radius = INT_MAX / 2;

    if (priv->radius == radius)
        return true;
    else
        priv->radius = radius;

    priv->b_r = ceil(radius);
    priv->b_w = 2 * priv->b_r + 1;

    if (priv->b_r) {
        priv->b = ass_realloc_array(priv->b, priv->b_w, sizeof(unsigned));
        priv->bt2 = ass_realloc_array(priv->bt2, priv->b_w, 256 * sizeof(unsigned));
        priv->bl = ass_realloc_array(priv->bl, priv->b_w, sizeof(unsigned));
        priv->blt2 = ass_realloc_array(priv->blt2, priv->b_w, 256 * sizeof(unsigned));
        if (!priv->b || !priv->bt2 || !priv->bl || !priv->blt2) {
            free(priv->b);
            free(priv->bt2);
            free(priv->bl);
            free(priv->blt2);
            return false;
        }
    }

    if (priv->b_r) {
        // integer be comb with volume = 65536
        for (volume_diff = 10000000; volume_diff > 0.0000001;
             volume_diff *= 0.5) {
            volume_factor += volume_diff;
            volume = 0;
            priv->b[0] = (unsigned) (outside * volume_factor + .5);
            priv->b[1] = (unsigned) (inside * volume_factor + .5);
            priv->b[priv->b_w - 2] = priv->b[1];
            priv->b[priv->b_w - 1] = priv->b[0];
            priv->b[priv->b_r] = (unsigned) (2 * volume_factor + .5);
            for (i = 0; i < priv->b_w; ++i)
                volume += priv->b[i];
            if (volume > 65536)
                volume_factor -= volume_diff;
        }
        volume = 0;
        priv->b[0] = (unsigned) (outside * volume_factor + .5);
        priv->b[1] = (unsigned) (inside * volume_factor + .5);
        priv->b[priv->b_w - 2] = priv->b[1];
        priv->b[priv->b_w - 1] = priv->b[0];
        priv->b[priv->b_r] = (unsigned) (2 * volume_factor + .5);
        for (i = 0; i < priv->b_w; ++i)
            volume += priv->b[i];

        // lookup table
        for (mx = 0; mx < priv->b_w; mx++) {
            for (i = 0; i < 256; i++) {
                priv->bt2[mx + i * priv->b_w] = i * priv->b[mx];
            }
        }

        // integer triangle with volume = 65536
        for (volume_diff = 10000000; volume_diff > 0.0000001;
             volume_diff *= 0.5) {
            volume_factor += volume_diff;
            volume = 0;
            for (i = 0; i < priv->b_w; ++i) {
                double value = FFMAX(0, 1 - abs(i - priv->b_r) / radius);
                priv->bl[i] = (unsigned) (value * volume_factor + .5);
                volume += priv->bl[i];
            }
            if (volume > 65536)
                volume_factor -= volume_diff;
        }
        volume = 0;
        for (i = 0; i < priv->b_w; ++i) {
            double value = FFMAX(0, 1 - abs(i - priv->b_r) / radius);
            priv->bl[i] = (unsigned) (value * volume_factor + .5);
            volume += priv->bl[i];
        }

        // lookup table
        for (mx = 0; mx < priv->b_w; mx++) {
            for (i = 0; i < 256; i++) {
                priv->blt2[mx + i * priv->b_w] = i * priv->bl[mx];
            }
        }
    }

    return true;
}

static bool resize_tmp(ASS_SynthPriv *priv, int w, int h)
{
    if (w >= INT_MAX || (w + 1) > SIZE_MAX / 2 / sizeof(unsigned) / FFMAX(h, 1))
        return false;
    size_t needed = FFMAX(sizeof(unsigned) * (w + 1) * h,
                          sizeof(uint16_t) * ass_align(32, w) * 2);
    if (priv->tmp && priv->tmp_allocated >= needed)
        return true;

    ass_aligned_free(priv->tmp);
    priv->tmp_allocated = FFMAX(needed, priv->tmp_allocated * 2);
    priv->tmp = ass_aligned_alloc(32, priv->tmp_allocated);
    return !!priv->tmp;
}

void ass_synth_blur(const BitmapEngine *engine,
                    ASS_SynthPriv *priv_blur, int opaque_box,
                    int be, double blur_radius, double radius_scale,
                    Bitmap *bm_g, Bitmap *bm_o)
{
    if(blur_radius > 0.0 || be){
        if (bm_o && !resize_tmp(priv_blur, bm_o->w, bm_o->h))
            return;
        if ((!bm_o || opaque_box) && !resize_tmp(priv_blur, bm_g->w, bm_g->h))
            return;
    }

    // Apply gaussian blur
    if (blur_radius > 0.0 && generate_gaussian_tables(priv_blur, blur_radius)) {
        if (bm_o)
            ass_convolve(bm_o->buffer, priv_blur->tmp,
                         bm_o->w, bm_o->h, bm_o->stride,
                         priv_blur->gt2, priv_blur->g_r,
                         priv_blur->g_w);
        if (!bm_o || opaque_box)
            ass_convolve(bm_g->buffer, priv_blur->tmp,
                         bm_g->w, bm_g->h, bm_g->stride,
                         priv_blur->gt2, priv_blur->g_r,
                         priv_blur->g_w);
    }

    // Apply box blur (multiple passes, if requested)
    if (be && (radius_scale == 1 || generate_be_tables(priv_blur, radius_scale))) {
        void *tmp = priv_blur->tmp;
        if (bm_o) {
            unsigned passes = be;
            unsigned w = bm_o->w;
            unsigned h = bm_o->h;
            unsigned stride = bm_o->stride;
            unsigned char *buf = bm_o->buffer;
            if(w && h){
                if(passes > 1){
                    be_blur_pre(buf, w, h, stride);
                    while(--passes){
                        if(radius_scale == 1){
                            memset(tmp, 0, stride * 2);
                            engine->be_blur(buf, w, h, stride, tmp);
                        }else{
                            ass_convolve(buf, tmp, w, h, stride,
                                         priv_blur->bt2, priv_blur->b_r,
                                         priv_blur->b_w);
                        }
                    }
                    be_blur_post(buf, w, h, stride);
                }
                if(radius_scale == 1){
                    memset(tmp, 0, stride * 2);
                    engine->be_blur(buf, w, h, stride, tmp);
                }else{
                    ass_convolve(buf, tmp, w, h, stride,
                                 priv_blur->bt2, priv_blur->b_r,
                                 priv_blur->b_w);
                    ass_convolve(buf, tmp, w, h, stride,
                                 priv_blur->blt2, priv_blur->b_r,
                                 priv_blur->b_w);
                }
            }
        }
        if (!bm_o || opaque_box) {
            unsigned passes = be;
            unsigned w = bm_g->w;
            unsigned h = bm_g->h;
            unsigned stride = bm_g->stride;
            unsigned char *buf = bm_g->buffer;
            if(w && h){
                if(passes > 1){
                    be_blur_pre(buf, w, h, stride);
                    while(--passes){
                        if(radius_scale == 1){
                            memset(tmp, 0, stride * 2);
                            engine->be_blur(buf, w, h, stride, tmp);
                        }else{
                            ass_convolve(buf, tmp, w, h, stride,
                                         priv_blur->bt2, priv_blur->b_r,
                                         priv_blur->b_w);
                        }
                    }
                    be_blur_post(buf, w, h, stride);
                }
                if(radius_scale == 1){
                    memset(tmp, 0, stride * 2);
                    engine->be_blur(buf, w, h, stride, tmp);
                }else{
                    ass_convolve(buf, tmp, w, h, stride,
                                 priv_blur->bt2, priv_blur->b_r,
                                 priv_blur->b_w);
                    ass_convolve(buf, tmp, w, h, stride,
                                 priv_blur->blt2, priv_blur->b_r,
                                 priv_blur->b_w);
                }
            }
        }
    }
}

ASS_SynthPriv *ass_synth_init(double radius)
{
    ASS_SynthPriv *priv = calloc(1, sizeof(ASS_SynthPriv));
    if (!priv || !generate_gaussian_tables(priv, radius)) {
        free(priv);
        return NULL;
    }
    return priv;
}

void ass_synth_done(ASS_SynthPriv *priv)
{
    ass_aligned_free(priv->tmp);
    free(priv->g0);
    free(priv->g);
    free(priv->gt2);
    free(priv);
}

static bool alloc_bitmap_buffer(const BitmapEngine *engine, Bitmap *bm, int w, int h)
{
    unsigned align = 1 << engine->align_order;
    size_t s = ass_align(align, w);
    // Too often we use ints as offset for bitmaps => use INT_MAX.
    if (s > (INT_MAX - 32) / FFMAX(h, 1))
        return false;
    uint8_t *buf = ass_aligned_alloc(align, s * h + 32);
    if (!buf)
        return false;
    bm->w = w;
    bm->h = h;
    bm->stride = s;
    bm->buffer = buf;
    return true;
}

static Bitmap *alloc_bitmap_raw(const BitmapEngine *engine, int w, int h)
{
    Bitmap *bm = malloc(sizeof(Bitmap));
    if (!bm)
        return NULL;
    if (!alloc_bitmap_buffer(engine, bm, w, h)) {
        free(bm);
        return NULL;
    }
    return bm;
}

Bitmap *alloc_bitmap(const BitmapEngine *engine, int w, int h)
{
    Bitmap *bm = alloc_bitmap_raw(engine, w, h);
    if(!bm)
        return NULL;
    memset(bm->buffer, 0, bm->stride * bm->h + 32);
    bm->left = bm->top = 0;
    return bm;
}

void ass_free_bitmap(Bitmap *bm)
{
    if (bm)
        ass_aligned_free(bm->buffer);
    free(bm);
}

Bitmap *copy_bitmap(const BitmapEngine *engine, const Bitmap *src)
{
    Bitmap *dst = alloc_bitmap_raw(engine, src->w, src->h);
    if (!dst)
        return NULL;
    dst->left = src->left;
    dst->top = src->top;
    memcpy(dst->buffer, src->buffer, src->stride * src->h);
    return dst;
}

#if CONFIG_RASTERIZER

Bitmap *outline_to_bitmap(ASS_Renderer *render_priv,
                          ASS_Outline *outline, int bord)
{
    RasterizerData *rst = &render_priv->rasterizer;
    if (!rasterizer_set_outline(rst, outline)) {
        ass_msg(render_priv->library, MSGL_WARN, "Failed to process glyph outline!\n");
        return NULL;
    }

    if (bord < 0 || bord > INT_MAX / 2)
        return NULL;

    if (rst->x_min >= rst->x_max || rst->y_min >= rst->y_max) {
        Bitmap *bm = alloc_bitmap(render_priv->engine, 2 * bord, 2 * bord);
        if (!bm)
            return NULL;
        bm->left = bm->top = -bord;
        return bm;
    }

    if (rst->x_max > INT_MAX - 63 || rst->y_max > INT_MAX - 63)
        return NULL;

    int x_min = rst->x_min >> 6;
    int y_min = rst->y_min >> 6;
    int x_max = (rst->x_max + 63) >> 6;
    int y_max = (rst->y_max + 63) >> 6;
    int w = x_max - x_min;
    int h = y_max - y_min;

    int mask = (1 << render_priv->engine->tile_order) - 1;

    if (w < 0 || h < 0 || w > 8000000 / FFMAX(h, 1) ||
        w > INT_MAX - (2 * bord + mask) || h > INT_MAX - (2 * bord + mask)) {
        ass_msg(render_priv->library, MSGL_WARN, "Glyph bounding box too large: %dx%dpx",
                w, h);
        return NULL;
    }

    int tile_w = (w + 2 * bord + mask) & ~mask;
    int tile_h = (h + 2 * bord + mask) & ~mask;
    Bitmap *bm = alloc_bitmap_raw(render_priv->engine, tile_w, tile_h);
    if (!bm)
        return NULL;
    bm->left = x_min - bord;
    bm->top =  y_min - bord;

    if (!rasterizer_fill(render_priv->engine, rst, bm->buffer,
                         x_min - bord, y_min - bord,
                         bm->stride, tile_h, bm->stride)) {
        ass_msg(render_priv->library, MSGL_WARN, "Failed to rasterize glyph!\n");
        ass_free_bitmap(bm);
        return NULL;
    }

    return bm;
}

#else

static Bitmap *outline_to_bitmap_ft(ASS_Renderer *render_priv,
                                    FT_Outline *outline, int bord)
{
    Bitmap *bm;
    int w, h;
    int error;
    FT_BBox bbox;
    FT_Bitmap bitmap;

    FT_Outline_Get_CBox(outline, &bbox);
    if (bbox.xMin >= bbox.xMax || bbox.yMin >= bbox.yMax) {
        bm = alloc_bitmap(2 * bord, 2 * bord);
        if (!bm)
            return NULL;
        bm->left = bm->top = -bord;
        return bm;
    }

    // move glyph to origin (0, 0)
    bbox.xMin &= ~63;
    bbox.yMin &= ~63;
    FT_Outline_Translate(outline, -bbox.xMin, -bbox.yMin);
    if (bbox.xMax > INT_MAX - 63 || bbox.yMax > INT_MAX - 63)
        return NULL;
    // bitmap size
    bbox.xMax = (bbox.xMax + 63) & ~63;
    bbox.yMax = (bbox.yMax + 63) & ~63;
    w = (bbox.xMax - bbox.xMin) >> 6;
    h = (bbox.yMax - bbox.yMin) >> 6;
    // pen offset
    bbox.xMin >>= 6;
    bbox.yMax >>= 6;

    if (w < 0 || h < 0 || w > 8000000 / FFMAX(h, 1) ||
        w > INT_MAX - 2 * bord || h > INT_MAX - 2 * bord) {
        ass_msg(render_priv->library, MSGL_WARN, "Glyph bounding box too large: %dx%dpx",
                w, h);
        return NULL;
    }

    // allocate and set up bitmap
    bm = alloc_bitmap(w + 2 * bord, h + 2 * bord);
    if (!bm)
        return NULL;
    bm->left = bbox.xMin - bord;
    bm->top = -bbox.yMax - bord;
    bitmap.width = w;
    bitmap.rows = h;
    bitmap.pitch = bm->stride;
    bitmap.buffer = bm->buffer + bord + bm->stride * bord;
    bitmap.num_grays = 256;
    bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;

    // render into target bitmap
    if ((error = FT_Outline_Get_Bitmap(render_priv->ftlibrary, outline, &bitmap))) {
        ass_msg(render_priv->library, MSGL_WARN, "Failed to rasterize glyph: %d\n", error);
        ass_free_bitmap(bm);
        return NULL;
    }

    return bm;
}

Bitmap *outline_to_bitmap(ASS_Renderer *render_priv,
                          ASS_Outline *outline, int bord)
{
    size_t n_points = outline->n_points;
    if (n_points > SHRT_MAX) {
        ass_msg(render_priv->library, MSGL_WARN, "Too many outline points: %d",
                outline->n_points);
        n_points = SHRT_MAX;
    }

    size_t n_contours = FFMIN(outline->n_contours, SHRT_MAX);
    short contours_small[EFFICIENT_CONTOUR_COUNT];
    short *contours = contours_small;
    short *contours_large = NULL;
    if (n_contours > EFFICIENT_CONTOUR_COUNT) {
        contours_large = malloc(n_contours * sizeof(short));
        if (!contours_large)
            return NULL;
        contours = contours_large;
    }
    for (size_t i = 0; i < n_contours; ++i)
        contours[i] = FFMIN(outline->contours[i], n_points - 1);

    FT_Outline ftol;
    ftol.n_points = n_points;
    ftol.n_contours = n_contours;
    ftol.points = outline->points;
    ftol.tags = outline->tags;
    ftol.contours = contours;
    ftol.flags = 0;

    Bitmap *bm = outline_to_bitmap_ft(render_priv, &ftol, bord);
    free(contours_large);
    return bm;
}

#endif

/**
 * \brief fix outline bitmap
 *
 * The glyph bitmap is subtracted from outline bitmap. This way looks much
 * better in some cases.
 */
void fix_outline(Bitmap *bm_g, Bitmap *bm_o)
{
    int x, y;
    const int l = bm_o->left > bm_g->left ? bm_o->left : bm_g->left;
    const int t = bm_o->top > bm_g->top ? bm_o->top : bm_g->top;
    const int r =
        bm_o->left + bm_o->stride <
        bm_g->left + bm_g->stride ? bm_o->left + bm_o->stride : bm_g->left + bm_g->stride;
    const int b =
        bm_o->top + bm_o->h <
        bm_g->top + bm_g->h ? bm_o->top + bm_o->h : bm_g->top + bm_g->h;

    unsigned char *g =
        bm_g->buffer + (t - bm_g->top) * bm_g->stride + (l - bm_g->left);
    unsigned char *o =
        bm_o->buffer + (t - bm_o->top) * bm_o->stride + (l - bm_o->left);

    for (y = 0; y < b - t; ++y) {
        for (x = 0; x < r - l; ++x) {
            unsigned char c_g, c_o;
            c_g = g[x];
            c_o = o[x];
            o[x] = (c_o > c_g) ? c_o - (c_g / 2) : 0;
        }
        g += bm_g->stride;
        o += bm_o->stride;
    }
}

/**
 * \brief Shift a bitmap by the fraction of a pixel in x and y direction
 * expressed in 26.6 fixed point
 */
void shift_bitmap(Bitmap *bm, int shift_x, int shift_y)
{
    int x, y, b;
    int w = bm->w;
    int h = bm->h;
    int s = bm->stride;
    unsigned char *buf = bm->buffer;

    assert((shift_x & ~63) == 0 && (shift_y & ~63) == 0);

    // Shift in x direction
    for (y = 0; y < h; y++) {
        for (x = w - 1; x > 0; x--) {
            b = (buf[x + y * s - 1] * shift_x) >> 6;
            buf[x + y * s - 1] -= b;
            buf[x + y * s] += b;
        }
    }

    // Shift in y direction
    for (x = 0; x < w; x++) {
        for (y = h - 1; y > 0; y--) {
            b = (buf[x + (y - 1) * s] * shift_y) >> 6;
            buf[x + (y - 1) * s] -= b;
            buf[x + y * s] += b;
        }
    }
}

/*
 * Convolution filter.  An fast pure C implementation from MPlayer.
 */
void ass_convolve(unsigned char *buffer, unsigned *tmp2,
                  int width, int height, int stride,
                  unsigned *m2, int r, int mwidth)
{

    int x, y;

    unsigned char *s = buffer;
    unsigned *t = tmp2 + 1;
    for (y = 0; y < height; y++) {
        memset(t - 1, 0, (width + 1) * sizeof(unsigned));
        t[-1] = 32768;

        for (x = 0; x < r; x++) {
            const int src = s[x];
            if (src) {
                register unsigned *dstp = t + x - r;
                int mx;
                unsigned *m3 = m2 + src * mwidth;
                for (mx = r - x; mx < mwidth; mx++) {
                    dstp[mx] += m3[mx];
                }
            }
        }

        for (; x < width - r; x++) {
            const int src = s[x];
            if (src) {
                register unsigned *dstp = t + x - r;
                int mx;
                unsigned *m3 = m2 + src * mwidth;
                for (mx = 0; mx < mwidth; mx++) {
                    dstp[mx] += m3[mx];
                }
            }
        }

        for (; x < width; x++) {
            const int src = s[x];
            if (src) {
                register unsigned *dstp = t + x - r;
                int mx;
                const int x2 = r + width - x;
                unsigned *m3 = m2 + src * mwidth;
                for (mx = 0; mx < x2; mx++) {
                    dstp[mx] += m3[mx];
                }
            }
        }

        s += stride;
        t += width + 1;
    }

    t = tmp2;
    for (x = 0; x < width; x++) {
        for (y = 0; y < r; y++) {
            unsigned *srcp = t + y * (width + 1) + 1;
            int src = *srcp;
            if (src) {
                register unsigned *dstp = srcp - 1 - y * (width + 1);
                const int src2 = (src + 32768) >> 16;
                unsigned *m3 = m2 + src2 * mwidth;

                int mx;
                *srcp = 32768;
                for (mx = r - y; mx < mwidth; mx++) {
                    *dstp += m3[mx];
                    dstp += width + 1;
                }
            }
        }
        for (; y < height - r; y++) {
            unsigned *srcp = t + y * (width + 1) + 1;
            int src = *srcp;
            if (src) {
                register unsigned *dstp = srcp - 1 - r * (width + 1);
                const int src2 = (src + 32768) >> 16;
                unsigned *m3 = m2 + src2 * mwidth;

                int mx;
                *srcp = 32768;
                for (mx = 0; mx < mwidth; mx++) {
                    *dstp += m3[mx];
                    dstp += width + 1;
                }
            }
        }
        for (; y < height; y++) {
            unsigned *srcp = t + y * (width + 1) + 1;
            int src = *srcp;
            if (src) {
                const int y2 = r + height - y;
                register unsigned *dstp = srcp - 1 - r * (width + 1);
                const int src2 = (src + 32768) >> 16;
                unsigned *m3 = m2 + src2 * mwidth;

                int mx;
                *srcp = 32768;
                for (mx = 0; mx < y2; mx++) {
                    *dstp += m3[mx];
                    dstp += width + 1;
                }
            }
        }
        t++;
    }

    t = tmp2;
    s = buffer;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            s[x] = t[x] >> 16;
        }
        s += stride;
        t += width + 1;
    }
}

/**
 * \brief Blur with [[1,2,1], [2,4,2], [1,2,1]] kernel
 * This blur is the same as the one employed by vsfilter.
 * Pure C implementation.
 */
void ass_be_blur_c(uint8_t *buf, intptr_t w, intptr_t h,
                   intptr_t stride, uint16_t *tmp)
{
    uint16_t *col_pix_buf = tmp;
    uint16_t *col_sum_buf = tmp + w;
    unsigned x, y, old_pix, old_sum, temp1, temp2;
    uint8_t *src, *dst;
    memset(tmp, 0, sizeof(uint16_t) * w * 2);
    y = 0;

    {
        src=buf+y*stride;

        x = 1;
        old_pix = src[x-1];
        old_sum = old_pix;
        for ( ; x < w; x++) {
            temp1 = src[x];
            temp2 = old_pix + temp1;
            old_pix = temp1;
            temp1 = old_sum + temp2;
            old_sum = temp2;
            col_pix_buf[x-1] = temp1;
            col_sum_buf[x-1] = temp1;
        }
        temp1 = old_sum + old_pix;
        col_pix_buf[x-1] = temp1;
        col_sum_buf[x-1] = temp1;
    }

    for (y++; y < h; y++) {
        src=buf+y*stride;
        dst=buf+(y-1)*stride;

        x = 1;
        old_pix = src[x-1];
        old_sum = old_pix;
        for ( ; x < w; x++) {
            temp1 = src[x];
            temp2 = old_pix + temp1;
            old_pix = temp1;
            temp1 = old_sum + temp2;
            old_sum = temp2;

            temp2 = col_pix_buf[x-1] + temp1;
            col_pix_buf[x-1] = temp1;
            dst[x-1] = (col_sum_buf[x-1] + temp2) >> 4;
            col_sum_buf[x-1] = temp2;
        }
        temp1 = old_sum + old_pix;
        temp2 = col_pix_buf[x-1] + temp1;
        col_pix_buf[x-1] = temp1;
        dst[x-1] = (col_sum_buf[x-1] + temp2) >> 4;
        col_sum_buf[x-1] = temp2;
    }

    {
        dst=buf+(y-1)*stride;
        for (x = 0; x < w; x++)
            dst[x] = (col_sum_buf[x] + col_pix_buf[x]) >> 4;
    }
}

void be_blur_pre(uint8_t *buf, intptr_t w, intptr_t h, intptr_t stride)
{
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            // This is equivalent to (value * 64 + 127) / 255 for all
            // values from 0 to 256 inclusive. Assist vectorizing
            // compilers by noting that all temporaries fit in 8 bits.
            buf[y * stride + x] =
                (uint8_t) ((buf[y * stride + x] >> 1) + 1) >> 1;
        }
    }
}

void be_blur_post(uint8_t *buf, intptr_t w, intptr_t h, intptr_t stride)
{
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            // This is equivalent to (value * 255 + 32) / 64 for all values
            // from 0 to 96 inclusive, and we only care about 0 to 64.
            uint8_t value = buf[y * stride + x];
            buf[y * stride + x] = (value << 2) - (value > 32);
        }
    }
}

/*
 * To find these values, simulate blur on the border between two
 * half-planes, one zero-filled (background) and the other filled
 * with the maximum supported value (foreground). Keep incrementing
 * the \be argument. The necessary padding is the distance by which
 * the blurred foreground image extends beyond the original border
 * and into the background. Initially it increases along with \be,
 * but very soon it grinds to a halt. At some point, the blurred
 * image actually reaches a stationary point and stays unchanged
 * forever after, simply _shifting_ by one pixel for each \be
 * step--moving in the direction of the non-zero half-plane and
 * thus decreasing the necessary padding (although the large
 * padding is still needed for intermediate results). In practice,
 * images are finite rather than infinite like half-planes, but
 * this can only decrease the required padding. Half-planes filled
 * with extreme values are the theoretical limit of the worst case.
 * Make sure to use the right pixel value range in the simulation!
 */
int be_padding(int be)
{
    if (be <= 3)
        return be;
    if (be <= 7)
        return 4;
    if (be <= 123)
        return 5;
    return FFMAX(128 - be, 0);
}

int outline_to_bitmap2(ASS_Renderer *render_priv,
                       ASS_Outline *outline, ASS_Outline *border,
                       Bitmap **bm_g, Bitmap **bm_o)
{
    assert(bm_g && bm_o);

    *bm_g = *bm_o = NULL;

    if (outline)
        *bm_g = outline_to_bitmap(render_priv, outline, 1);
    if (!*bm_g)
        return 1;

    if (border) {
        *bm_o = outline_to_bitmap(render_priv, border, 1);
        if (!*bm_o) {
            return 1;
        }
    }

    return 0;
}

/**
 * \brief Add two bitmaps together at a given position
 * Uses additive blending, clipped to [0,255]. Pure C implementation.
 */
void ass_add_bitmaps_c(uint8_t *dst, intptr_t dst_stride,
                       uint8_t *src, intptr_t src_stride,
                       intptr_t height, intptr_t width)
{
    unsigned out;
    uint8_t* end = dst + dst_stride * height;
    while (dst < end) {
        for (unsigned j = 0; j < width; ++j) {
            out = dst[j] + src[j];
            dst[j] = FFMIN(out, 255);
        }
        dst += dst_stride;
        src += src_stride;
    }
}

void ass_sub_bitmaps_c(uint8_t *dst, intptr_t dst_stride,
                       uint8_t *src, intptr_t src_stride,
                       intptr_t height, intptr_t width)
{
    short out;
    uint8_t* end = dst + dst_stride * height;
    while (dst < end) {
        for (unsigned j = 0; j < width; ++j) {
            out = dst[j] - src[j];
            dst[j] = FFMAX(out, 0);
        }
        dst += dst_stride;
        src += src_stride;
    }
}

void ass_mul_bitmaps_c(uint8_t *dst, intptr_t dst_stride,
                       uint8_t *src1, intptr_t src1_stride,
                       uint8_t *src2, intptr_t src2_stride,
                       intptr_t w, intptr_t h)
{
    uint8_t* end = src1 + src1_stride * h;
    while (src1 < end) {
        for (unsigned x = 0; x < w; ++x) {
            dst[x] = (src1[x] * src2[x] + 255) >> 8;
        }
        dst  += dst_stride;
        src1 += src1_stride;
        src2 += src2_stride;
    }
}
