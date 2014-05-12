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
#include "ass_tile.h"
#include <string.h>
#include <assert.h>



static inline int ilog2(uint32_t n)  // XXX: to utils
{
    return __builtin_clz(n) ^ 31;
}



void *alloc_tile(TileEngine *engine)
{
    return ass_aligned_alloc(32, 2 << (2 * engine->tile_order));
}

void *copy_tile(TileEngine *engine, const void *tile)  // XXX: SSE/AVX, ref-count
{
    int16_t *buf = alloc_tile(engine);
    if (buf)
        memcpy(buf, tile, 2 << (2 * engine->tile_order));
    return buf;
}

void free_tile(TileEngine *engine, void *tile)
{
    ass_aligned_free(tile);
}

Quad *alloc_quad(TileEngine *engine, Quad *fill)
{
    assert(!fill || fill == SOLID_TILE);

    Quad *res = malloc(sizeof(Quad));
    if (!res)
        return NULL;

    int i;
    for (i = 0; i < 4; ++i)
        res->child[i] = fill;
    return res;
}

int copy_quad(TileEngine *engine, Quad **dst, const Quad *src, int size_order)  // XXX: ref-count
{
    assert(size_order >= engine->tile_order);

    if (!src || src == SOLID_TILE) {
        *dst = (Quad *)src;
        return 1;
    }
    if (size_order == engine->tile_order) {
        *dst = copy_tile(engine, src);
        return *dst != NULL;
    }

    *dst = alloc_quad(engine, NULL);
    if (!*dst)
        return 0;

    int i;
    for (i = 0; i < 4; ++i)
        if (!copy_quad(engine, &(*dst)->child[i], src->child[i], size_order - 1))
            return 0;
    return 1;
}

void free_empty_quad(TileEngine *engine, Quad *quad)
{
    free(quad);
}

void free_quad(TileEngine *engine, Quad *quad, int size_order)
{
    assert(size_order >= engine->tile_order);

    if (!quad || quad == SOLID_TILE)
        return;

    if (size_order == engine->tile_order) {
        free_tile(engine, quad);
        return;
    }

    int i;
    for (i = 0; i < 4; ++i)
        free_quad(engine, quad->child[i], size_order - 1);
    free_empty_quad(engine, quad);
}

TileTree *alloc_tile_tree(TileEngine *engine, Quad *fill)
{
    assert(!fill || fill == SOLID_TILE);

    TileTree *res = malloc(sizeof(TileTree));
    if (!res)
        return NULL;

    res->size_order = -1;
    res->outside = fill;

    int i;
    for (i = 0; i < 4; ++i)
        res->quad.child[i] = fill;
    return res;
}

TileTree *copy_tile_tree(TileEngine *engine, const TileTree *src)
{
    TileTree *res = alloc_tile_tree(engine, src->outside);
    if (!res)
        return NULL;

    res->x = src->x;
    res->y = src->y;
    res->size_order = src->size_order;

    int i;
    for (i = 0; i < 4; ++i)
        if (!copy_quad(engine, &res->quad.child[i], src->quad.child[i], src->size_order - 1)) {
            free_tile_tree(engine, res);
            return NULL;
        }
    return res;
}

void free_tile_tree(TileEngine *engine, TileTree *tree)
{
    int i;
    for (i = 0; i < 4; ++i)
        free_quad(engine, tree->quad.child[i], tree->size_order - 1);
    free(tree);
}


void finalize_quad(TileEngine *engine, uint8_t *buf, ptrdiff_t stride,
                   const Quad *quad, int size_order)
{
    assert(size_order >= engine->tile_order);

    if (!quad || quad == SOLID_TILE) {
        engine->finalize_solid(buf, stride, size_order, quad != NULL);
        return;
    }

    if (size_order == engine->tile_order) {
        engine->finalize_generic(buf, stride, (const int16_t *)quad);
        return;
    }

    --size_order;
    int offset = 1 << size_order;
    uint8_t *next[] = {
        buf,
        buf + offset,
        buf + offset * stride,
        buf + offset * stride + offset
    };
    int i;
    for (i = 0; i < 4; ++i)
        finalize_quad(engine, next[i], stride, quad->child[i], size_order);
}


static int insert_sub_quad(TileEngine *engine, Quad *dst, Quad *src,
                           int dst_order, int src_order, int delta_x, int delta_y,
                           Quad *outside)
{
    assert(dst_order > src_order && src_order >= engine->tile_order);
    assert(!(delta_x & ((1 << src_order) - 1)) && !(delta_y & ((1 << src_order) - 1)));
    assert(!outside || outside == SOLID_TILE);
    assert(dst && dst != SOLID_TILE);
    assert(src != outside);

    --dst_order;
    int x = (delta_x >> dst_order) & 1;
    int y = (delta_y >> dst_order) & 1;
    Quad **next = &dst->child[x + 2 * y];

    if (src_order == dst_order) {
        assert(*next == outside);
        *next = src;
        return 1;
    }

    if (*next == outside) {
        *next = alloc_quad(engine, outside);
        if (!*next)
            return 0;
    }

    return insert_sub_quad(engine, *next, src,
                           dst_order, src_order,
                           delta_x, delta_y, outside);
}

static Quad *extract_sub_quad(TileEngine *engine, Quad *src,
                              int dst_order, int src_order, int delta_x, int delta_y)
{
    assert(src_order > dst_order && dst_order >= engine->tile_order);
    assert(!(delta_x & ((1 << dst_order) - 1)) && !(delta_y & ((1 << dst_order) - 1)));
    assert(src && src != SOLID_TILE);

    --src_order;
    int x = (delta_x >> src_order) & 1;
    int y = (delta_y >> src_order) & 1;
    Quad **next = &src->child[x + 2 * y];

    if (src_order == dst_order) {
        Quad *res = *next;
        *next = NULL;
        return res;
    }

    if (!*next || *next == SOLID_TILE)
        return *next;

    return extract_sub_quad(engine, *next,
                            dst_order, src_order,
                            delta_x, delta_y);
}

void calc_tree_bounds(TileEngine *engine, TileTree *dst,
                      int x_min, int y_min, int x_max, int y_max)
{
    assert(x_min < x_max && y_min < y_max);

    int x_xor = x_min ^ (x_max - 1);
    int y_xor = y_min ^ (y_max - 1);
    int x_ord = ilog2((x_xor ^ (x_xor & x_min) << 1) | 1);
    int y_ord = ilog2((y_xor ^ (y_xor & y_min) << 1) | 1);
    int ord = FFMAX(engine->tile_order, FFMAX(x_ord, y_ord));
    dst->x = x_min & ~((1 << ord) - 1);
    dst->y = y_min & ~((1 << ord) - 1);
    dst->size_order = ord + 1;
}

static inline int tree_cross(TileEngine *engine, TileTree *dst,
                             const TileTree *src1, const TileTree *src2)
{
    if (src1->size_order < 0 || src2->size_order < 0) {
        dst->size_order = -1;
        return 0;
    }

    int size1 = 1 << src1->size_order;
    int size2 = 1 << src2->size_order;
    int x_min = FFMAX(src1->x, src2->x);
    int y_min = FFMAX(src1->y, src2->y);
    int x_max = FFMIN(src1->x + size1, src2->x + size2);
    int y_max = FFMIN(src1->y + size1, src2->y + size2);
    if (x_min >= x_max || y_min >= y_max) {
        dst->size_order = -1;
        return 0;
    }
    calc_tree_bounds(engine, dst, x_min, y_min, x_max, y_max);
    return 1;
}

static inline int tree_union(TileEngine *engine, TileTree *dst,
                             const TileTree *src1, const TileTree *src2)
{
    if (src1->size_order < 0) {
        dst->x = src2->x;
        dst->y = src2->y;
        dst->size_order = src2->size_order;
        return dst->size_order >= 0;
    }
    if (src2->size_order < 0) {
        dst->x = src1->x;
        dst->y = src1->y;
        dst->size_order = src1->size_order;
        return 1;
    }

    int size1 = 1 << src1->size_order;
    int size2 = 1 << src2->size_order;
    int x_min = FFMIN(src1->x, src2->x);
    int y_min = FFMIN(src1->y, src2->y);
    int x_max = FFMAX(src1->x + size1, src2->x + size2);
    int y_max = FFMAX(src1->y + size1, src2->y + size2);
    calc_tree_bounds(engine, dst, x_min, y_min, x_max, y_max);
    return 1;
}

static int crop_tree(TileEngine *engine, TileTree *dst, const TileTree *src, Quad *dominant_tile[2])
{
    int i;
    TileTree old = *dst;
    if (src->outside == dominant_tile[1]) {
        if (dst->outside != dominant_tile[0]) {
            dst->x = src->x;
            dst->y = src->y;
            dst->size_order = src->size_order;
            dst->outside = dominant_tile[0];
        } else if (!tree_cross(engine, dst, dst, src)) {
            for (i = 0; i < 4; ++i) {
                free_quad(engine, old.quad.child[i], old.size_order - 1);
                dst->quad.child[i] = dst->outside;
            }
            return 1;
        }
    } else {
        if (dst->outside == dominant_tile[0])
            return 1;
        else if (!tree_union(engine, dst, dst, src))
            return 1;
    }
    if (old.size_order < 0 || (dst->x == old.x && dst->y == old.y && dst->size_order == old.size_order))
        return 1;

    for (i = 0; i < 4; ++i)
        dst->quad.child[i] = dst->outside;

    int res = 1;
    if (old.size_order <= dst->size_order) {
        for (i = 0; i < 4; ++i) {
            if (old.quad.child[i] == dst->outside)
                continue;
            int delta_x = old.x - dst->x + (((i >> 0) & 1) << (old.size_order - 1));
            int delta_y = old.y - dst->y + (((i >> 1) & 1) << (old.size_order - 1));
            if ((delta_x | delta_y) >> dst->size_order)
                continue;
            if (!insert_sub_quad(engine, &dst->quad, old.quad.child[i],
                                 dst->size_order, old.size_order - 1,
                                 delta_x, delta_y, dst->outside)) {
                res = 0;
                break;
            }
            old.quad.child[i] = NULL;
        }
    } else {
        for (i = 0; i < 4; ++i) {
            int delta_x = dst->x - old.x + (((i >> 0) & 1) << (dst->size_order - 1));
            int delta_y = dst->y - old.y + (((i >> 1) & 1) << (dst->size_order - 1));
            dst->quad.child[i] = (delta_x | delta_y) >> old.size_order ? dst->outside :
                extract_sub_quad(engine, &old.quad, dst->size_order - 1, old.size_order, delta_x, delta_y);
        }
    }
    for (i = 0; i < 4; ++i)
        free_quad(engine, old.quad.child[i], old.size_order - 1);
    return res;
}


static inline Quad *invert(Quad *solid_tile)
{
    assert(!solid_tile || solid_tile == SOLID_TILE);
    const intptr_t mask = (intptr_t)SOLID_TILE ^ (intptr_t)NULL;
    return (Quad *)((intptr_t)solid_tile ^ mask);
}

static int combine_quad(TileEngine *engine, Quad **dst, const Quad *src, int size_order,
                        TileCombineFunc tile_func, Quad *dominant_tile[2])
{
    assert(size_order >= engine->tile_order);
    assert(!dominant_tile[0] || dominant_tile[0] == SOLID_TILE);
    assert(!dominant_tile[1] || dominant_tile[1] == SOLID_TILE);

    if (*dst == dominant_tile[0] || src == invert(dominant_tile[1]))
        return 1;

    if (src == dominant_tile[1]) {
        free_quad(engine, *dst, size_order);
        *dst = dominant_tile[0];
        return 1;
    }

    if (*dst == invert(dominant_tile[0])) {
        if (dominant_tile[0] == dominant_tile[1])
            return copy_quad(engine, dst, src, size_order);

        if (size_order == engine->tile_order) {
            int16_t *buf = alloc_tile(engine);
            if (!buf)
                return 0;
            if (tile_func(buf, engine->solid_tile[*dst ? 1 : 0], (const int16_t *)src))
                *dst = (Quad *)buf;
            else {
                free_tile(engine, buf);
                *dst = dominant_tile[0];
            }
            return 1;
        }

        *dst = alloc_quad(engine, *dst);
        if (!*dst)
            return 0;

    } else if (size_order == engine->tile_order) {
        int16_t *buf = alloc_tile(engine);
        if (!buf)
            return 0;
        int res = tile_func(buf, (int16_t *)*dst, (const int16_t *)src);
        free_tile(engine, *dst);
        if (res)
            *dst = (Quad *)buf;
        else {
            free_tile(engine, buf);
            *dst = dominant_tile[0];
        }
        return 1;
    }

    int i, empty = 1;
    for (i = 0; i < 4; ++i) {
        if (!combine_quad(engine, &(*dst)->child[i], src->child[i],
                          size_order - 1, tile_func, dominant_tile))
            return 0;
        if ((*dst)->child[i] != dominant_tile[0])
            empty = 0;
    }
    if (empty) {
        free_empty_quad(engine, *dst);
        *dst = dominant_tile[0];
    }
    return 1;
}

static int combine_small_quad(TileEngine *engine, Quad *dst, const Quad *src,
                              int dst_order, int src_order, int delta_x, int delta_y,
                              TileCombineFunc tile_func, Quad *dominant_tile[2])
{
    assert(dst_order > src_order && src_order >= engine->tile_order);
    assert(!(delta_x & ((1 << src_order) - 1)) && !(delta_y & ((1 << src_order) - 1)));
    assert(!dominant_tile[0] || dominant_tile[0] == SOLID_TILE);
    assert(!dominant_tile[1] || dominant_tile[1] == SOLID_TILE);
    assert(src != invert(dominant_tile[1]));
    assert(dst && dst != SOLID_TILE);

    --dst_order;
    int x = (delta_x >> dst_order) & 1;
    int y = (delta_y >> dst_order) & 1;
    Quad **next = &dst->child[x + 2 * y];

    if (src_order == dst_order)
        return combine_quad(engine, next, src, dst_order,
                            tile_func, dominant_tile);

    if (*next == dominant_tile[0])
        return 1;

    if (*next == invert(dominant_tile[0])) {
        *next = alloc_quad(engine, invert(dominant_tile[0]));
        if (!*next)
            return 0;
    }

    if (!combine_small_quad(engine, *next, src,
                            dst_order, src_order, delta_x, delta_y,
                            tile_func, dominant_tile))
        return 0;

    int i, empty = 1;
    for (i = 0; i < 4; ++i) {
        if ((*next)->child[i] != dominant_tile[0])
            empty = 0;
    }
    if (empty) {
        free_empty_quad(engine, *next);
        *next = dominant_tile[0];
    }
    return 1;
}

static int combine_large_quad(TileEngine *engine, Quad **dst, const Quad *src,
                              int dst_order, int src_order, int delta_x, int delta_y,
                              TileCombineFunc tile_func, Quad *dominant_tile[2])
{
    assert(src_order > dst_order && dst_order >= engine->tile_order);
    assert(!(delta_x & ((1 << dst_order) - 1)) && !(delta_y & ((1 << dst_order) - 1)));
    assert(!dominant_tile[0] || dominant_tile[0] == SOLID_TILE);
    assert(!dominant_tile[1] || dominant_tile[1] == SOLID_TILE);
    assert(src && src != SOLID_TILE);
    assert(*dst != dominant_tile[0]);

    --src_order;
    int x = (delta_x >> src_order) & 1;
    int y = (delta_y >> src_order) & 1;
    Quad *next = src->child[x + 2 * y];

    if (src_order == dst_order)
        return combine_quad(engine, dst, next, dst_order,
                            tile_func, dominant_tile);

    if (next == invert(dominant_tile[1]))
        return 1;

    if (next == dominant_tile[1]) {
        free_quad(engine, *dst, dst_order);
        *dst = dominant_tile[0];
        return 1;
    }

    return combine_large_quad(engine, dst, next,
                              dst_order, src_order, delta_x, delta_y,
                              tile_func, dominant_tile);
}

int combine_tile_tree(TileEngine *engine, TileTree *dst, const TileTree *src, int op)
{
    static Quad *dominant_tile[][2] = {
        {NULL,       NULL},        // COMBINE_MUL
        {SOLID_TILE, SOLID_TILE},  // COMBINE_ADD
        {NULL,       SOLID_TILE},  // COMBINE_SUB
    };

    TileCombineFunc tile_func = engine->tile_combine[op];
    if (!crop_tree(engine, dst, src, dominant_tile[op]))
        return 0;

    if (dst->size_order < 0 || src->size_order < 0)
        return 1;

    int i;
    if (src->size_order <= dst->size_order) {
        for (i = 0; i < 4; ++i) {
            if (src->quad.child[i] == invert(dominant_tile[op][1]))
                continue;
            int delta_x = src->x - dst->x + (((i >> 0) & 1) << (src->size_order - 1));
            int delta_y = src->y - dst->y + (((i >> 1) & 1) << (src->size_order - 1));
            if ((delta_x | delta_y) >> dst->size_order)
                continue;
            if (!combine_small_quad(engine, &dst->quad, src->quad.child[i],
                                    dst->size_order, src->size_order - 1, delta_x, delta_y,
                                    tile_func, dominant_tile[op]))
                return 0;
        }
    } else {
        for (i = 0; i < 4; ++i) {
            if (dst->quad.child[i] == dominant_tile[op][0])
                continue;
            int delta_x = dst->x - src->x + (((i >> 0) & 1) << (dst->size_order - 1));
            int delta_y = dst->y - src->y + (((i >> 1) & 1) << (dst->size_order - 1));
            if ((delta_x | delta_y) >> src->size_order)
                continue;
            if (!combine_large_quad(engine, &dst->quad.child[i], &src->quad,
                                    dst->size_order - 1, src->size_order, delta_x, delta_y,
                                    tile_func, dominant_tile[op]))
                return 0;
        }
    }
    return 1;  // XXX: shrink tree
}
