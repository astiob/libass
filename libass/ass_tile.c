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
#include <math.h>



static inline int ilog2(uint32_t n)  // XXX: to utils
{
    return __builtin_clz(n) ^ 31;
}


#ifdef NDEBUG
#define REFCOUNT_SIZE      sizeof(unsigned)
#else
#define REFCOUNT_SIZE      (2 * sizeof(unsigned))
#endif

static inline void init_ref_count(const void *ptr, size_t size)
{
    unsigned *count = (unsigned *)((char *)ptr + size);
#ifndef NDEBUG
    *count++ = 0xABCDABCDu;
#endif
    *count = 0;
}

static inline void inc_ref_count(const void *ptr, size_t size)
{
    unsigned *count = (unsigned *)((char *)ptr + size);
#ifndef NDEBUG
    assert(*count == 0xABCDABCDu);
    ++count;
#endif
    ++(*count);
}

static inline int dec_ref_count(const void *ptr, size_t size)
{
    unsigned *count = (unsigned *)((char *)ptr + size);
#ifndef NDEBUG
    assert(*count == 0xABCDABCDu);
    ++count;
#endif
    if (*count) {
        --(*count);
        return 1;
    }
#ifndef NDEBUG
    count[-1] = 0xDEAD1234u;
#endif
    return 0;
}

static inline int is_unique(const void *ptr, size_t size)
{
    unsigned *count = (unsigned *)((char *)ptr + size);
#ifndef NDEBUG
    assert(*count == 0xABCDABCDu);
    ++count;
#endif
    return !*count;
}


static const Quad solid_sub_tile[] = {
    { .child = { EMPTY_QUAD, EMPTY_QUAD, EMPTY_QUAD, EMPTY_QUAD } },
    { .child = { SOLID_QUAD, SOLID_QUAD, SOLID_QUAD, SOLID_QUAD } }
};


void *alloc_tile(const TileEngine *engine)
{
    const int size = 2 << (2 * engine->tile_order);
    void *res = ass_aligned_alloc(engine->tile_alignment, size + REFCOUNT_SIZE);
    if (res)
        init_ref_count(res, size);
    return res;
}

const void *copy_tile(const TileEngine *engine, const void *tile)
{
    const int size = 2 << (2 * engine->tile_order);
    inc_ref_count(tile, size);
    return tile;
}

void free_tile(const TileEngine *engine, const void *tile)
{
    const int size = 2 << (2 * engine->tile_order);
    if (!dec_ref_count(tile, size))
        ass_aligned_free((void *)tile);
}

Quad *alloc_quad(const TileEngine *engine, const Quad *fill)
{
    assert(is_trivial_quad(fill));

    Quad *res = malloc(sizeof(Quad) + REFCOUNT_SIZE);
    if (!res)
        return NULL;
    init_ref_count(res, sizeof(Quad));

    for (int i = 0; i < 4; ++i)
        res->child[i] = fill;
    return res;
}

int copy_quad(const TileEngine *engine, const Quad **dst, const Quad *quad, int size_order)
{
    assert(size_order >= engine->tile_order);

    if (is_trivial_quad(quad))
        return set_trivial_quad(dst, !!quad);

    if (size_order == engine->tile_order) {
        *dst = copy_tile(engine, quad);
        return FLAG_VALID;
    }

    *dst = quad;
    inc_ref_count(quad, sizeof(Quad));
    return FLAG_VALID;
}

void free_quad(const TileEngine *engine, const Quad *quad, int size_order)
{
    assert(size_order >= engine->tile_order);

    if (is_trivial_quad(quad))
        return;

    if (size_order == engine->tile_order) {
        free_tile(engine, quad);
        return;
    }

    if (dec_ref_count(quad, sizeof(Quad)))
        return;

    for (int i = 0; i < 4; ++i)
        free_quad(engine, quad->child[i], size_order - 1);
    free((void *)quad);
}

TileTree *alloc_tile_tree(const TileEngine *engine, const Quad *fill)
{
    assert(is_trivial_quad(fill));

    TileTree *res = malloc(sizeof(TileTree));
    if (!res)
        return NULL;

    res->size_order = -1;
    res->outside = fill;

    for (int i = 0; i < 4; ++i)
        res->quad.child[i] = fill;
    return res;
}

TileTree *copy_tile_tree(const TileEngine *engine, const TileTree *src)
{
    TileTree *res = alloc_tile_tree(engine, src->outside);
    if (!res)
        return NULL;

    res->x = src->x;
    res->y = src->y;
    res->size_order = src->size_order;
    if (src->size_order < 0)
        return res;

    assert(src->size_order > engine->tile_order);
    for (int i = 0; i < 4; ++i)
        copy_quad(engine, &res->quad.child[i], src->quad.child[i], src->size_order - 1);
    return res;
}

static void clear_tile_tree(const TileEngine *engine, TileTree *tree)
{
    if (tree->size_order < 0)
        return;
    for (int i = 0; i < 4; ++i) {
        free_quad(engine, tree->quad.child[i], tree->size_order - 1);
        tree->quad.child[i] = tree->outside;
    }
    tree->size_order = -1;
}

void free_tile_tree(const TileEngine *engine, TileTree *tree)
{
    if (tree->size_order >= 0) {
        assert(tree->size_order > engine->tile_order);
        for (int i = 0; i < 4; ++i)
            free_quad(engine, tree->quad.child[i], tree->size_order - 1);
    }
    free(tree);
}

static size_t calc_quad_size(const TileEngine *engine, const Quad *quad, int size_order)
{
    assert(size_order >= engine->tile_order);

    if (is_trivial_quad(quad))
        return 0;

    if (size_order == engine->tile_order)
        return 2 << (2 * engine->tile_order);

    size_t res = sizeof(Quad);
    for (int i = 0; i < 4; ++i)
        res += calc_quad_size(engine, quad->child[i], size_order - 1);
    return res;
}

size_t calc_tree_size(const TileEngine *engine, const TileTree *tree)
{
    size_t res = sizeof(TileTree);
    if (tree->size_order < 0)
        return res;
    for (int i = 0; i < 4; ++i)
        res += calc_quad_size(engine, tree->quad.child[i], tree->size_order - 1);
    return res;
}

static int is_valid_quad(const TileEngine *engine, const Quad *quad, int size_order)
{
    if (size_order < engine->tile_order)
        return 0;
    if (is_trivial_quad(quad) || size_order == engine->tile_order)
        return 1;

    int flags = 3;
    for (int i = 0; i < 4; ++i) {
        if (!is_valid_quad(engine, quad->child[i], size_order - 1))
            return 0;

        switch ((intptr_t)quad->child[i]) {
            case (intptr_t)EMPTY_QUAD: flags &= 1; break;
            case (intptr_t)SOLID_QUAD: flags &= 2; break;
            default: flags = 0;
        }
    }
    return !flags;
}

int is_valid_tree(const TileEngine *engine, const TileTree *tree)
{
    if (!is_trivial_quad(tree->outside))
        return 0;
    if (tree->size_order < 0)
        return is_trivial_quad(tree->quad.child[0]) &&
               is_trivial_quad(tree->quad.child[1]) &&
               is_trivial_quad(tree->quad.child[2]) &&
               is_trivial_quad(tree->quad.child[3]);
    if (tree->size_order <= engine->tile_order)
        return 0;
    int mask = (1 << (tree->size_order - 1)) - 1;
    /*
    return !(tree->x & mask) && !(tree->y & mask) &&
        is_valid_quad(engine, &tree->quad, tree->size_order);
    */
    if ((tree->x & mask) || (tree->y & mask))
        return 0;
    for (int i = 0; i < 4; ++i)
        if (!is_valid_quad(engine, tree->quad.child[i], tree->size_order - 1))
            return 0;
    return 1;
}


void finalize_quad(const TileEngine *engine, uint8_t *buf, ptrdiff_t stride,
                   const Quad *quad, int size_order)
{
    assert(size_order >= engine->tile_order);

    if (is_trivial_quad(quad)) {
        engine->finalize_solid(buf, stride, size_order, !!quad);
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
    for (int i = 0; i < 4; ++i)
        finalize_quad(engine, next[i], stride, quad->child[i], size_order);
}


typedef struct
{
    int x[4], y[4];
    int size_order;
    const Quad *corner[2][2];
    const Quad *horz_side[2];
    const Quad *vert_side[2];
    const Quad *inside, *outside;
} QuadRect;

static int rectangle_level_up(const TileEngine *engine, QuadRect *rect)
{
    int x[4], y[4];
    for (int j = 0; j < 4; ++j) {
        x[j] = rect->x[j] & 1;
        rect->x[j] = (rect->x[j] + (j & 1)) >> 1;
    }
    for (int i = 0; i < 4; ++i) {
        y[i] = rect->y[i] & 1;
        rect->y[i] = (rect->y[i] + (i & 1)) >> 1;
    }

    int i0[2] = { 0, 1 }, j0[2] = { 0, 1 };
    if (rect->x[2] < rect->x[1]) {
        rect->x[2] = rect->x[1];
        j0[1] = 0;
    }
    if (rect->y[2] < rect->y[1]) {
        rect->y[2] = rect->y[1];
        i0[1] = 0;
    }

    int size_order = rect->size_order;
    ++rect->size_order;

    int error = 0;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            Quad *next = NULL;
            if (rect->x[2 * j + 1] > rect->x[2 * j] &&
                rect->y[2 * i + 1] > rect->y[2 * i]) {
                next = alloc_quad(engine, rect->outside);
                if (!next)
                    error = 1;
                else if (i0[1] && j0[1])
                    next->child[(2 * i + j) ^ 3] = rect->inside;
            }
            const Quad *prev = rect->corner[i][j];
            rect->corner[i][j] = next;
            if (!prev)
                continue;

            next = (Quad *)rect->corner[i0[i]][j0[j]];
            if (next)
                next->child[2 * y[2 * i] + x[2 * j]] = prev;
            else
                free_quad(engine, prev, size_order);
        }

    for (int i = 0; i < 2; ++i) {
        Quad *next = NULL;
        if (rect->x[2] > rect->x[1] &&
            rect->y[2 * i + 1] > rect->y[2 * i]) {
            next = alloc_quad(engine, rect->outside);
            if (!next)
                error = 1;
            else if (i0[1]) {
                next->child[(2 * i + 0) ^ 3] = rect->inside;
                next->child[(2 * i + 1) ^ 3] = rect->inside;
            }
        }
        const Quad *prev = rect->horz_side[i];
        rect->horz_side[i] = next;
        if (!prev)
            continue;

        for (int j = 0; j < 2; ++j)
            if (x[j + 1]) {
                Quad *quad = (Quad *)rect->corner[i0[i]][j];
                if (quad)
                    copy_quad(engine, &quad->child[2 * y[2 * i] + (j ^ 1)], prev, size_order);
            }

        next = (Quad *)rect->horz_side[i0[i]];
        if (next) {
            copy_quad(engine, &prev, prev, size_order);
            next->child[2 * y[2 * i] + 0] = prev;
            next->child[2 * y[2 * i] + 1] = prev;
        } else
            free_quad(engine, prev, size_order);
    }

    for (int j = 0; j < 2; ++j) {
        Quad *next = NULL;
        if (rect->y[2] > rect->y[1] &&
            rect->x[2 * j + 1] > rect->x[2 * j]) {
            next = alloc_quad(engine, rect->outside);
            if (!next)
                error = 1;
            else if (j0[1]) {
                next->child[(2 * 0 + j) ^ 3] = rect->inside;
                next->child[(2 * 1 + j) ^ 3] = rect->inside;
            }
        }
        const Quad *prev = rect->vert_side[j];
        rect->vert_side[j] = next;
        if (!prev)
            continue;

        for (int i = 0; i < 2; ++i)
            if (y[i + 1]) {
                Quad *quad = (Quad *)rect->corner[i][j0[j]];
                if (quad)
                    copy_quad(engine, &quad->child[2 * (i ^ 1) + x[2 * j]], prev, size_order);
            }

        next = (Quad *)rect->vert_side[j0[j]];
        if (next) {
            copy_quad(engine, &prev, prev, size_order);
            next->child[2 * 0 + x[2 * j]] = prev;
            next->child[2 * 1 + x[2 * j]] = prev;
        } else
            free_quad(engine, prev, size_order);
    }

    return !error;
}

static TileTree *rectangle_process(const TileEngine *engine, QuadRect *rect,
                                   int32_t x_min, int32_t y_min, int32_t x_max, int32_t y_max,
                                   int inverse)
{
    assert(x_min < x_max && y_min < y_max);

    memset(rect, 0, sizeof(QuadRect));
    rect->size_order = engine->tile_order;
    rect->inside = trivial_quad(!inverse);
    rect->outside = trivial_quad(inverse);

    int shift = engine->tile_order + 6;
    int32_t mask = ((int32_t)1 << shift) - 1;
    rect->x[0] = (x_min +    0) >> shift;
    rect->x[1] = (x_min + mask) >> shift;
    rect->x[2] = (x_max +    0) >> shift;
    rect->x[3] = (x_max + mask) >> shift;
    rect->y[0] = (y_min +    0) >> shift;
    rect->y[1] = (y_min + mask) >> shift;
    rect->y[2] = (y_max +    0) >> shift;
    rect->y[3] = (y_max + mask) >> shift;

    int32_t ab = (int32_t)1 << 30, scale = inverse ? -ab : ab;
    CombineTileFunc combine = engine->combine[inverse ? COMBINE_ADD : COMBINE_MUL];

    if (x_min & mask) {
        int16_t *buf = alloc_tile(engine);
        if (!buf)
            return NULL;
        engine->fill_halfplane(buf, ab, 0, (int64_t)(x_min & mask) << 30, -scale);
        rect->vert_side[0] = (const Quad *)buf;
    }
    if (x_max & mask) {
        int16_t *buf = alloc_tile(engine);
        if (!buf)
            return NULL;
        engine->fill_halfplane(buf, ab, 0, (int64_t)(x_max & mask) << 30, scale);
        rect->vert_side[1] = (const Quad *)buf;
    }
    if (rect->x[2] < rect->x[1]) {
        rect->x[2] = rect->x[1];
        combine((int16_t *)rect->vert_side[0],
                (int16_t *)rect->vert_side[0], (int16_t *)rect->vert_side[1]);
        free_tile(engine, (int16_t *)rect->vert_side[1]);
        rect->vert_side[1] = NULL;
    }

    if (y_min & mask) {
        int16_t *buf = alloc_tile(engine);
        if (!buf)
            return NULL;
        engine->fill_halfplane(buf, 0, ab, (int64_t)(y_min & mask) << 30, -scale);
        rect->horz_side[0] = (const Quad *)buf;
    }
    if (y_max & mask) {
        int16_t *buf = alloc_tile(engine);
        if (!buf)
            return NULL;
        engine->fill_halfplane(buf, 0, ab, (int64_t)(y_max & mask) << 30, scale);
        rect->horz_side[1] = (const Quad *)buf;
    }
    if (rect->y[2] < rect->y[1]) {
        rect->y[2] = rect->y[1];
        combine((int16_t *)rect->horz_side[0],
                (int16_t *)rect->horz_side[0], (int16_t *)rect->horz_side[1]);
        free_tile(engine, (int16_t *)rect->horz_side[1]);
        rect->horz_side[1] = NULL;
    }

    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            if (rect->horz_side[i] && rect->vert_side[j]) {
                int16_t *buf = alloc_tile(engine);
                if (!buf)
                    return NULL;
                combine(buf, (int16_t *)rect->horz_side[i], (int16_t *)rect->vert_side[j]);
                rect->corner[i][j] = (const Quad *)buf;
            }
    if (rect->x[1] == rect->x[2])
        for (int i = 0; i < 2; ++i)
            if (rect->horz_side[i]) {
                free_tile(engine, rect->horz_side[i]);
                rect->horz_side[i] = NULL;
            }
    if (rect->y[1] == rect->y[2])
        for (int j = 0; j < 2; ++j)
            if (rect->vert_side[j]) {
                free_tile(engine, rect->vert_side[j]);
                rect->vert_side[j] = NULL;
            }

    while (rect->x[3] > rect->x[0] + 2 || rect->y[3] > rect->y[0] + 2)
        if (!rectangle_level_up(engine, rect))
            return NULL;

    int x0 = rect->x[0], y0 = rect->y[0];
    for (int j = 0; j < 4; ++j)
        rect->x[j] -= x0;
    for (int i = 0; i < 4; ++i)
        rect->y[i] -= y0;
    if (!rectangle_level_up(engine, rect))
        return NULL;

    assert(!rect->x[0] && rect->x[3] == 1);
    assert(!rect->y[0] && rect->y[3] == 1);

    TileTree *tree = alloc_tile_tree(engine, rect->inside);
    if (!tree)
        return NULL;
    tree->x = x0 << (rect->size_order - 1);
    tree->y = y0 << (rect->size_order - 1);
    tree->size_order = rect->size_order;
    tree->outside = rect->outside;

    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            const Quad *quad = rect->corner[i][j];
            if (!quad)
                continue;
            for (int k = 0; k < 4; ++k)
                copy_quad(engine, &tree->quad.child[k], quad->child[k], rect->size_order - 1);
            free_quad(engine, quad, rect->size_order);
            return tree;
        }
    for (int i = 0; i < 2; ++i) {
        const Quad *quad = rect->horz_side[i];
        if (!quad)
            continue;
        for (int k = 0; k < 4; ++k)
            copy_quad(engine, &tree->quad.child[k], quad->child[k], rect->size_order - 1);
        free_quad(engine, quad, rect->size_order);
        return tree;
    }
    for (int j = 0; j < 2; ++j) {
        const Quad *quad = rect->vert_side[j];
        if (!quad)
            continue;
        for (int k = 0; k < 4; ++k)
            copy_quad(engine, &tree->quad.child[k], quad->child[k], rect->size_order - 1);
        free_quad(engine, quad, rect->size_order);
        return tree;
    }
    return tree;
}

TileTree *create_rectangle(const TileEngine *engine,
                           int32_t x_min, int32_t y_min, int32_t x_max, int32_t y_max,
                           int inverse)
{
    QuadRect rect;
    TileTree *tree = rectangle_process(engine, &rect,
                                       x_min, y_min, x_max, y_max,
                                       inverse);
    if (tree)
        return tree;

    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            free_quad(engine, rect.corner[i][j], rect.size_order);
    for (int i = 0; i < 2; ++i)
        free_quad(engine, rect.horz_side[i], rect.size_order);
    for (int j = 0; j < 2; ++j)
        free_quad(engine, rect.vert_side[j], rect.size_order);

    return NULL;
}


static inline int get_child_index(int delta_x, int delta_y, int size_order)
{
    int x = (delta_x >> size_order) & 1;
    int y = (delta_y >> size_order) & 1;
    return x + 2 * y;
}

static int insert_sub_quad(const TileEngine *engine, Quad *dst, const Quad *src,
                           int dst_order, int src_order, int delta_x, int delta_y,
                           const Quad *outside)
{
    assert(dst_order > src_order && src_order >= engine->tile_order);
    assert(!(delta_x & ((1 << src_order) - 1)) && !(delta_y & ((1 << src_order) - 1)));
    assert(!is_trivial_quad(dst) && src != outside);
    assert(is_trivial_quad(outside));

    --dst_order;
    const Quad **next = &dst->child[get_child_index(delta_x, delta_y, dst_order)];

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

    assert(!is_trivial_quad(*next) && is_unique(*next, sizeof(Quad)));
    return insert_sub_quad(engine, (Quad *)*next, src,
                           dst_order, src_order,
                           delta_x, delta_y, outside);
}

static int extract_sub_quad(const TileEngine *engine, const Quad **dst, const Quad *src,
                            int dst_order, int src_order, int delta_x, int delta_y)
{
    assert(src_order > dst_order && dst_order >= engine->tile_order);
    assert(!(delta_x & ((1 << dst_order) - 1)) && !(delta_y & ((1 << dst_order) - 1)));
    assert(!is_trivial_quad(src));

    --src_order;
    const Quad *next = src->child[get_child_index(delta_x, delta_y, src_order)];

    if (src_order == dst_order)
        return copy_quad(engine, dst, next, src_order);

    if (is_trivial_quad(next))
        return set_trivial_quad(dst, !!next);

    return extract_sub_quad(engine, dst, next,
                            dst_order, src_order,
                            delta_x, delta_y);
}

void calc_tree_bounds(const TileEngine *engine, TileTree *dst,
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

static int crop_tree(const TileEngine *engine, TileTree *dst,
                     const TileTree *src, int op_flags)
{
    if (src->size_order < 0) {
        if (src->outside != trivial_quad(op_flags & 2))
            return 1;
        dst->outside = trivial_quad(op_flags & 1);
        clear_tile_tree(engine, dst);
        return 1;
    }

    int x_min = src->x;
    int y_min = src->y;
    int x_max = src->x + (1 << src->size_order);
    int y_max = src->y + (1 << src->size_order);

    if (src->outside == trivial_quad(op_flags & 2)) {
        if (dst->outside != trivial_quad(op_flags & 1))
            dst->outside = trivial_quad(op_flags & 1);
        else {
            if (dst->size_order < 0)
                return 1;
            x_min = FFMAX(x_min, dst->x);
            y_min = FFMAX(y_min, dst->y);
            x_max = FFMIN(x_max, dst->x + (1 << dst->size_order));
            y_max = FFMIN(y_max, dst->y + (1 << dst->size_order));
            if (x_min >= x_max || y_min >= y_max) {
                clear_tile_tree(engine, dst);
                return 1;
            }
        }
    } else {
        if (dst->outside == trivial_quad(op_flags & 1))
            return 1;
        if (dst->size_order >= 0) {
            x_min = FFMIN(x_min, dst->x);
            y_min = FFMIN(y_min, dst->y);
            x_max = FFMAX(x_max, dst->x + (1 << dst->size_order));
            y_max = FFMAX(y_max, dst->y + (1 << dst->size_order));
        }
    }

    TileTree old = *dst;
    calc_tree_bounds(engine, dst, x_min, y_min, x_max, y_max);
    if (old.size_order < 0 || (x_min == old.x && y_min == old.y &&
        x_max - x_min == 1 << old.size_order && y_max - y_min == 1 << old.size_order))
        return 1;

    for (int i = 0; i < 4; ++i)
        dst->quad.child[i] = dst->outside;

    int res = 1;
    if (old.size_order <= dst->size_order) {
        for (int i = 0; i < 4; ++i) {
            if (old.quad.child[i] == dst->outside)
                continue;
            int delta_x = old.x + (((i >> 0) & 1) << (old.size_order - 1));
            int delta_y = old.y + (((i >> 1) & 1) << (old.size_order - 1));
            if (delta_x < x_min || delta_x >= x_max ||
                delta_y < y_min || delta_y >= y_max)
                continue;
            delta_x -= dst->x;
            delta_y -= dst->y;
            assert(!((delta_x | delta_y) >> dst->size_order));
            if (!insert_sub_quad(engine, &dst->quad, old.quad.child[i],
                                 dst->size_order, old.size_order - 1,
                                 delta_x, delta_y, dst->outside)) {
                res = 0;
                break;
            }
            old.quad.child[i] = NULL;
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            int delta_x = dst->x + (((i >> 0) & 1) << (dst->size_order - 1));
            int delta_y = dst->y + (((i >> 1) & 1) << (dst->size_order - 1));
            if (delta_x < x_min || delta_x >= x_max ||
                delta_y < y_min || delta_y >= y_max)
                continue;
            delta_x -= old.x;
            delta_y -= old.y;
            assert(!((delta_x | delta_y) >> old.size_order));
            extract_sub_quad(engine, &dst->quad.child[i], &old.quad,
                             dst->size_order - 1, old.size_order, delta_x, delta_y);
        }
    }
    for (int i = 0; i < 4; ++i)
        free_quad(engine, old.quad.child[i], old.size_order - 1);
    return res;
}


static int combine_quad(const TileEngine *engine, const Quad **dst,
                        const Quad *src1, const Quad *src2, int size_order,
                        CombineTileFunc tile_func, int op_flags)
{
    assert(size_order >= engine->tile_order);

    if (src1 == trivial_quad(op_flags & 1) || src2 == trivial_quad(op_flags & 2))
        return set_trivial_quad(dst, op_flags & 1);
    if (src2 == trivial_quad(~op_flags & 2))
        return copy_quad(engine, dst, src1, size_order) | FLAG_SRC1;

    const int16_t *tile = (const int16_t *)src1;
    if (src1 == trivial_quad(~op_flags & 1)) {
        if (trivial_quad(op_flags & 1) == trivial_quad(op_flags & 2))
            return copy_quad(engine, dst, src2, size_order) | FLAG_SRC2;
        tile = engine->solid_tile[src1 ? 1 : 0];
        src1 = &solid_sub_tile[src1 ? 1 : 0];
    }

    if (size_order == engine->tile_order) {
        int16_t *buf = alloc_tile(engine);
        *dst = (const Quad *)buf;
        if (!buf)
            return 0;
        if (!tile_func(buf, tile, (const int16_t *)src2))
            return FLAG_VALID;
        free_tile(engine, buf);
        return set_trivial_quad(dst, op_flags & 1);
    }

    Quad *quad = alloc_quad(engine, trivial_quad(op_flags & 1));
    *dst = quad;
    if (!quad)
        return 0;

    int flags = FLAG_ALL_COMBINE;
    for (int i = 0; i < 4; ++i) {
        flags &= combine_quad(engine, &quad->child[i],
                              src1->child[i], src2->child[i],
                              size_order - 1, tile_func, op_flags);
        if (!(flags & FLAG_VALID))
            return 0;
    }
    switch (flags & ~FLAG_VALID)
    {
        case FLAG_EMPTY: *dst = EMPTY_QUAD; break;
        case FLAG_SOLID: *dst = SOLID_QUAD; break;
        case FLAG_SRC1: copy_quad(engine, dst, src1, size_order); break;
        case FLAG_SRC2: copy_quad(engine, dst, src2, size_order); break;
        default: assert(flags == FLAG_VALID); return FLAG_VALID;
    }
    free_quad(engine, quad, size_order);
    return flags;
}

static int combine_small_quad(const TileEngine *engine, const Quad **dst,
                              const Quad *src1, const Quad *src2,
                              int src1_order, int src2_order,
                              int delta_x, int delta_y,
                              CombineTileFunc tile_func, int op_flags)
{
    assert(src1_order > src2_order && src2_order >= engine->tile_order);
    assert(!(delta_x & ((1 << src2_order) - 1)) && !(delta_y & ((1 << src2_order) - 1)));
    assert(src2 != trivial_quad(~op_flags & 2));

    --src1_order;
    int index = get_child_index(delta_x, delta_y, src1_order);

    const Quad *next = src1;
    if (!is_trivial_quad(src1))
        next = src1->child[index];

    const Quad *dominant_quad = trivial_quad(op_flags & 1);
    if (next == dominant_quad)
        return copy_quad(engine, dst, src1, src1_order + 1) | FLAG_SRC1;

    int flags;
    const Quad *quad;
    if (src1_order == src2_order)
        flags = combine_quad(engine, &quad, next, src2,
                             src2_order, tile_func, op_flags);
    else
        flags = combine_small_quad(engine, &quad, next, src2,
                                   src1_order, src2_order, delta_x, delta_y,
                                   tile_func, op_flags);
    if (!(flags & FLAG_VALID))
        return 0;
    if (flags & FLAG_SRC1) {
        free_quad(engine, quad, src1_order);
        return copy_quad(engine, dst, src1, src1_order + 1) | FLAG_SRC1;
    }

    if (quad == dominant_quad && src1 != trivial_quad(~op_flags & 1)) {
        if (src1 == dominant_quad)
            return set_trivial_quad(dst, op_flags & 1);
        int empty = 1;
        for (int i = 0; i < 4; ++i)
            if (i != index && src1->child[i] != dominant_quad)
                empty = 0;
        if (empty)
            return set_trivial_quad(dst, op_flags & 1);
    }

    if (is_trivial_quad(src1)) {
        Quad *res = alloc_quad(engine, src1);
        if (!res)
            return 0;
        res->child[index] = quad;
        *dst = res;
        return FLAG_VALID;
    }

    Quad *res = alloc_quad(engine, NULL);
    if (!res)
        return 0;
    for (int i = 0; i < 4; ++i) {
        if (i == index)
            res->child[i] = quad;
        else
            copy_quad(engine, &res->child[i], src1->child[i], src1_order);
    }
    *dst = res;
    return FLAG_VALID;
}

static int combine_large_quad(const TileEngine *engine, const Quad **dst,
                              const Quad *src1, const Quad *src2,
                              int src1_order, int src2_order,
                              int delta_x, int delta_y,
                              CombineTileFunc tile_func, int op_flags)
{
    assert(src2_order > src1_order && src1_order >= engine->tile_order);
    assert(!(delta_x & ((1 << src1_order) - 1)) && !(delta_y & ((1 << src1_order) - 1)));
    assert(src1 != trivial_quad(op_flags & 1) && !is_trivial_quad(src2));

    --src2_order;
    int index = get_child_index(delta_x, delta_y, src2_order);

    const Quad *next = src2;
    if (!is_trivial_quad(src2))
        next = src2->child[index];

    if (src1_order == src2_order)
        return combine_quad(engine, dst, src1, next,
                            src1_order, tile_func, op_flags);

    if (next == trivial_quad(op_flags & 2))
        return set_trivial_quad(dst, op_flags & 1);
    if (next == trivial_quad(~op_flags & 2))
        return copy_quad(engine, dst, src1, src1_order);

    return combine_large_quad(engine, dst, src1, next,
                              src1_order, src2_order, delta_x, delta_y,
                              tile_func, op_flags);
}

int combine_tile_tree(const TileEngine *engine, TileTree *dst, const TileTree *src, int op)
{
    assert(is_valid_tree(engine, dst) && is_valid_tree(engine, src));

    static int op_flags[] = {
        0 << 0 | 0 << 1,  // COMBINE_MUL
        1 << 0 | 1 << 1,  // COMBINE_ADD
        0 << 0 | 1 << 1,  // COMBINE_SUB
    };

    CombineTileFunc tile_func = engine->combine[op];
    if (!crop_tree(engine, dst, src, op_flags[op]))
        return 0;

    if (dst->size_order < 0 || src->size_order < 0)
        return 1;

    if (src->size_order < dst->size_order) {
        for (int i = 0; i < 4; ++i) {
            if (src->quad.child[i] == trivial_quad(~op_flags[op] & 2))
                continue;

            int delta_x = src->x - dst->x + (((i >> 0) & 1) << (src->size_order - 1));
            int delta_y = src->y - dst->y + (((i >> 1) & 1) << (src->size_order - 1));
            if ((delta_x | delta_y) >> dst->size_order)
                continue;

            const Quad *quad;
            int index = get_child_index(delta_x, delta_y, dst->size_order - 1);
            int flags = combine_small_quad(engine, &quad,
                                           dst->quad.child[index], src->quad.child[i],
                                           dst->size_order - 1, src->size_order - 1,
                                           delta_x, delta_y, tile_func, op_flags[op]);
            if (!(flags & FLAG_VALID))
                return 0;
            free_quad(engine, dst->quad.child[index], dst->size_order - 1);
            dst->quad.child[index] = quad;
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            if (dst->quad.child[i] == trivial_quad(op_flags[op] & 1))
                continue;

            int delta_x = dst->x - src->x + (((i >> 0) & 1) << (dst->size_order - 1));
            int delta_y = dst->y - src->y + (((i >> 1) & 1) << (dst->size_order - 1));
            if ((delta_x | delta_y) >> src->size_order)
                continue;

            const Quad *quad;
            int flags = combine_large_quad(engine, &quad,
                                           dst->quad.child[i], &src->quad,
                                           dst->size_order - 1, src->size_order,
                                           delta_x, delta_y, tile_func, op_flags[op]);
            if (!(flags & FLAG_VALID))
                return 0;
            free_quad(engine, dst->quad.child[i], dst->size_order - 1);
            dst->quad.child[i] = quad;
        }
    }
    assert(is_valid_tree(engine, dst));
    return 1;  // XXX: shrink tree
}


static int create_tree_from_grid(const TileEngine *engine, TileTree *tree,
                                 int base1, int base2, int size_order, int dir,
                                 int min1, int max1, int min2, int max2,
                                 const Quad **grid, int stride)
{
    for (int i = 0; i < 4; ++i) {
        free_quad(engine, tree->quad.child[i], tree->size_order - 1);
        tree->quad.child[i] = tree->outside;
    }

    if (min1 > max1 || min2 > max2) {
        tree->size_order = -1;
        assert(is_valid_tree(engine, tree));
        return 1;
    }

    calc_tree_bounds(engine, tree,
                     base1 + (min1 << size_order),
                     base2 + (min2 << size_order),
                     base1 + ((max1 + 1) << size_order),
                     base2 + ((max2 + 1) << size_order));
    if (dir == 1) {
        int tmp = tree->x;
        tree->x = tree->y;
        tree->y = tmp;
    }
    if (tree->size_order == size_order) {
        const Quad *quad = grid[min1 * stride + min2];
        assert(!is_trivial_quad(quad));
        for (int i = 0; i < 4; ++i)
            copy_quad(engine, &tree->quad.child[i], quad->child[i], size_order - 1);
        free_quad(engine, quad, size_order);
        assert(is_valid_tree(engine, tree));
        return 1;
    }

    for (int i = min1; i <= max1; ++i)
        for (int j = min2; j <= max2; ++j) {
            if (grid[i * stride + j] == tree->outside)
                continue;
            int delta_x = base1 + (i << size_order);
            int delta_y = base2 + (j << size_order);
            if (dir == 1) {
                int tmp = delta_x;
                delta_x = delta_y;
                delta_y = tmp;
            }
            delta_x -= tree->x;
            delta_y -= tree->y;
            assert(!((delta_x | delta_y) >> tree->size_order));
            if (!insert_sub_quad(engine, &tree->quad, grid[i * stride + j],
                                 tree->size_order, size_order,
                                 delta_x, delta_y, tree->outside))
                return 0;
            grid[i * stride + j] = NULL;
        }
    assert(is_valid_tree(engine, tree));
    return 1;
}

static void clear_quad_grid(const TileEngine *engine, int size_order,
                            const Quad **grid, int n1, int n2)
{
    for (int i = 0; i < n1; ++i)
        for (int j = 0; j < n2; ++j)
            free_quad(engine, grid[i * n2 + j], size_order);
}


static int shrink_quad(const TileEngine *engine, const Quad **dst,
                       const Quad *side1, const Quad *src1,
                       const Quad *src2, const Quad *side2,
                       int size_order, int dir)
{
    assert(size_order >= engine->tile_order);
    assert(dir == 1 || dir == 2);

    if (size_order == engine->tile_order) {
        const int16_t *tile[4] = {
            (const int16_t *)side1, (const int16_t *)src1,
            (const int16_t *)src2, (const int16_t *)side2
        };
        if (is_trivial_quad(side1))
            tile[0] = engine->solid_tile[side1 ? 1 : 0];
        if (is_trivial_quad(side2))
            tile[3] = engine->solid_tile[side2 ? 1 : 0];
        if (is_trivial_quad(src1)) {
            if (src2 == src1) {
                if (side1 == src1 && side2 == src1)
                    return set_trivial_quad(dst, !!src1);

                int16_t *buf = alloc_tile(engine);
                *dst = (const Quad *)buf;
                if (!buf)
                    return 0;
                if (!engine->shrink_solid[dir - 1](buf, tile[0], !!src1, tile[3]))
                    return FLAG_VALID;
                free_tile(engine, buf);
                return set_trivial_quad(dst, !!src1);
            }
            tile[1] = engine->solid_tile[src1 ? 1 : 0];
        }
        if (is_trivial_quad(src2))
            tile[2] = engine->solid_tile[src2 ? 1 : 0];

        int16_t *buf = alloc_tile(engine);
        *dst = (const Quad *)buf;
        if (!buf)
            return 0;
        engine->shrink[dir - 1](buf, tile[0], tile[1], tile[2], tile[3]);
        return FLAG_VALID;
    }

    const Quad *input[4] = { side1, src1, src2, side2 };
    if (is_trivial_quad(side1))
        input[0] = &solid_sub_tile[side1 ? 1 : 0];
    if (is_trivial_quad(side2))
        input[3] = &solid_sub_tile[side2 ? 1 : 0];
    if (is_trivial_quad(src1)) {
        if (src2 == src1)  // XXX: flags
            if (side1 == src1 && side2 == src1)
                return set_trivial_quad(dst, !!src1);
        input[1] = &solid_sub_tile[src1 ? 1 : 0];
    }
    if (is_trivial_quad(src2))
        input[2] = &solid_sub_tile[src2 ? 1 : 0];

    Quad *quad = alloc_quad(engine, NULL);
    *dst = quad;
    if (!quad)
        return 0;

    int flags = FLAG_ALL;
    int k[2][2] = { { 0, dir }, { dir ^ 3, 3 } };
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            flags &= shrink_quad(engine, &quad->child[k[i][j]],
                                 input[j + 0]->child[k[i][1]], input[j + 1]->child[k[i][0]],
                                 input[j + 1]->child[k[i][1]], input[j + 2]->child[k[i][0]],
                                 size_order - 1, dir);
            if (!(flags & FLAG_VALID))
                return 0;
        }
    if (flags == FLAG_VALID)
        return FLAG_VALID;
    free_quad(engine, quad, size_order);
    return set_trivial_quad(dst, flags & FLAG_SOLID);
}

int shrink_tile_tree(const TileEngine *engine, TileTree *tree, int dir)
{
    assert(is_valid_tree(engine, tree));
    assert(dir == 1 || dir == 2);

    if (tree->size_order < 0)
        return 1;

    assert(tree->size_order > engine->tile_order);

    int base1 = tree->x, base2 = tree->y;
    if (dir == 1) {
        int tmp = base1;
        base1 = base2;
        base2 = tmp;
    }
    int size_order = tree->size_order - 1;
    int size = 1 << size_order, j0 = 0;
    base2 = (base2 >> 1) - size;
    if (base2 & (size - 1)) {
        base2 &= ~(size - 1);
        j0 = 1;
    }

    const Quad *src[] = {
        tree->outside, tree->outside, tree->outside,
        tree->quad.child[0], tree->quad.child[dir],
        tree->outside, tree->outside, tree->outside,
        tree->quad.child[dir ^ 3], tree->quad.child[3],
        tree->outside, tree->outside, tree->outside,
    };
    const int n = 3;
    const Quad **ptr = src - j0, *grid[2 * n];
    memset(grid, 0, sizeof(grid));

    int mask = trivial_quad_flag(!!tree->outside);
    int min1 = 2, max1 = -1, min2 = n, max2 = -1;
    for (int i = 0; i < 2; ++i) {
        for (int j = j0; j < n; ++j) {
            int res = shrink_quad(engine, &grid[i * n + j],
                                  ptr[2 * j + 0], ptr[2 * j + 1],
                                  ptr[2 * j + 2], ptr[2 * j + 3],
                                  size_order, dir);
            if (!(res & FLAG_VALID)) {
                clear_quad_grid(engine, size_order, grid, 2, n);
                return 0;
            }
            if ((res & mask) == FLAG_VALID) {
                min1 = FFMIN(min1, i);
                max1 = FFMAX(max1, i);
                min2 = FFMIN(min2, j);
                max2 = FFMAX(max2, j);
            }
        }
        ptr += 5;
    }

    if (create_tree_from_grid(engine, tree,
                              base1, base2, size_order, dir,
                              min1, max1, min2, max2, grid, n))
        return 1;
    clear_quad_grid(engine, size_order, grid, 2, n);
    return 0;
}


static int expand_quad(const TileEngine *engine,
                       const Quad **dst1, const Quad **dst2,
                       const Quad *side1, const Quad *src, const Quad *side2,
                       int size_order, int dir)
{
    assert(size_order >= engine->tile_order);
    assert(dir == 1 || dir == 2);

    if (size_order == engine->tile_order) {
        const int16_t *tile[3] = {
            (const int16_t *)side1, (const int16_t *)src, (const int16_t *)side2
        };
        if (is_trivial_quad(src)) {
            if (is_trivial_quad(side1))
                tile[0] = engine->solid_tile[side1 ? 1 : 0];
            if (is_trivial_quad(side2))
                tile[2] = engine->solid_tile[side2 ? 1 : 0];

            *dst1 = *dst2 = src;
            int flags = trivial_quad_flag(!!src);
            if (side1 != src) {
                int16_t *buf = alloc_tile(engine);
                if (!buf)
                    return 0;
                if (engine->expand_solid[0][dir - 1](buf, tile[0], !!src))
                    free_tile(engine, buf);
                else {
                    *dst1 = (const Quad *)buf;
                    flags &= FLAG_VALID;
                }
            }
            if (side2 != src) {
                int16_t *buf = alloc_tile(engine);
                if (!buf)
                    return 0;
                if (engine->expand_solid[1][dir - 1](buf, tile[2], !!src))
                    free_tile(engine, buf);
                else {
                    *dst2 = (const Quad *)buf;
                    flags &= FLAG_VALID;
                }
            }
            return flags;
        }

        int res = 0;
        if (is_trivial_quad(side1)) {
            tile[0] = engine->solid_tile[side1 ? 1 : 0];
            res |= 1;
        }
        if (is_trivial_quad(side2)) {
            tile[2] = engine->solid_tile[side2 ? 1 : 0];
            res |= 2;
        }

        int16_t *buf1 = alloc_tile(engine);
        *dst1 = (const Quad *)buf1;
        int16_t *buf2 = alloc_tile(engine);
        *dst2 = (const Quad *)buf2;
        if (!buf1 || !buf2)
            return 0;
        res &= engine->expand[dir - 1](buf1, buf2, tile[0], tile[1], tile[2]);
        int flags = FLAG_ALL;
        if (res & 1) {
            free_tile(engine, buf1);
            *dst1 = side1;
            flags &= trivial_quad_flag(!!side1);
        } else
            flags = FLAG_VALID;
        if (res & 2) {
            free_tile(engine, buf2);
            *dst2 = side2;
            flags &= trivial_quad_flag(!!side2);
        } else
            flags = FLAG_VALID;
        return flags;
    }

    const Quad *input[3] = { side1, src, side2 };
    if (is_trivial_quad(side1))
        input[0] = &solid_sub_tile[side1 ? 1 : 0];
    if (is_trivial_quad(side2))
        input[2] = &solid_sub_tile[side2 ? 1 : 0];
    if (is_trivial_quad(src)) {
        if (side1 == src && side2 == src) {
            *dst1 = *dst2 = src;
            return trivial_quad_flag(!!src);
        }
        input[1] = &solid_sub_tile[src ? 1 : 0];
    }

    Quad *quad1 = alloc_quad(engine, NULL);
    *dst1 = quad1;
    Quad *quad2 = alloc_quad(engine, NULL);
    *dst2 = quad2;
    if (!quad1 || !quad2)
        return 0;

    int flags1 = FLAG_ALL, flags2 = FLAG_ALL;
    int k[2][2] = { { 0, dir }, { dir ^ 3, 3 } };
    for (int i = 0; i < 2; ++i) {
        flags1 &= expand_quad(engine, &quad1->child[k[i][0]], &quad1->child[k[i][1]],
                         input[0]->child[k[i][1]], input[1]->child[k[i][0]], input[1]->child[k[i][1]],
                         size_order - 1, dir);
        if (!(flags1 & FLAG_VALID))
            return 0;
        flags2 &= expand_quad(engine, &quad2->child[k[i][0]], &quad2->child[k[i][1]],
                         input[1]->child[k[i][0]], input[1]->child[k[i][1]], input[2]->child[k[i][0]],
                         size_order - 1, dir);
        if (!(flags2 & FLAG_VALID))
            return 0;
    }
    if (flags1 & ~FLAG_VALID) {
        free_quad(engine, quad1, size_order);
        set_trivial_quad(dst1, flags1 & FLAG_SOLID);
    }
    if (flags2 & ~FLAG_VALID) {
        free_quad(engine, quad2, size_order);
        set_trivial_quad(dst2, flags2 & FLAG_SOLID);
    }
    return flags1 & flags2;
}

int expand_tile_tree(const TileEngine *engine, TileTree *tree, int dir)
{
    assert(is_valid_tree(engine, tree));
    assert(dir == 1 || dir == 2);

    if (tree->size_order < 0)
        return 1;

    assert(tree->size_order > engine->tile_order);

    int base1 = tree->x, base2 = tree->y;
    if (dir == 1) {
        int tmp = base1;
        base1 = base2;
        base2 = tmp;
    }
    int size_order = tree->size_order - 1;
    base2 = (base2 - (1 << size_order)) << 1;

    const Quad *src[] = {
        tree->outside, tree->outside,
        tree->quad.child[0], tree->quad.child[dir],
        tree->outside, tree->outside,
        tree->quad.child[dir ^ 3], tree->quad.child[3],
        tree->outside, tree->outside,
    };
    const int n = 8;
    const Quad **ptr = src, *grid[2 * n];
    memset(grid, 0, sizeof(grid));

    int min1 = 2, max1 = -1, min2 = n, max2 = -1;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < n / 2; ++j) {
            int res = expand_quad(engine, &grid[i * n + 2 * j + 0], &grid[i * n + 2 * j + 1],
                                  ptr[j + 0], ptr[j + 1], ptr[j + 2], size_order, dir);
            if (!(res & FLAG_VALID)) {
                clear_quad_grid(engine, size_order, grid, 2, n);
                return 0;
            }
            if (grid[i * n + 2 * j + 0] != tree->outside) {
                min1 = FFMIN(min1, i);
                max1 = FFMAX(max1, i);
                min2 = FFMIN(min2, 2 * j + 0);
                max2 = FFMAX(max2, 2 * j + 0);
            }
            if (grid[i * n + 2 * j + 1] != tree->outside) {
                min1 = FFMIN(min1, i);
                max1 = FFMAX(max1, i);
                min2 = FFMIN(min2, 2 * j + 1);
                max2 = FFMAX(max2, 2 * j + 1);
            }
        }
        assert(grid[i * n + 0] == tree->outside && grid[i * n + 7] == tree->outside);
        ptr += 4;
    }

    if (create_tree_from_grid(engine, tree,
                              base1, base2, size_order, dir,
                              min1, max1, min2, max2, grid, n))
        return 1;
    clear_quad_grid(engine, size_order, grid, 2, n);
    return 0;
}


static int filter_quad(const TileEngine *engine, const Quad **dst,
                       const Quad *side1, const Quad *src, const Quad *side2,
                       int size_order, int dir, const FilterTileFunc tile_func[2],
                       const FilterSolidTileFunc solid_tile_func[2], void *param)
{
    assert(size_order >= engine->tile_order);
    assert(dir == 1 || dir == 2);

    if (size_order == engine->tile_order) {
        const int16_t *tile[3] = {
            (const int16_t *)side1, (const int16_t *)src, (const int16_t *)side2
        };
        if (is_trivial_quad(side1))
            tile[0] = engine->solid_tile[side1 ? 1 : 0];
        if (is_trivial_quad(side2))
            tile[2] = engine->solid_tile[side2 ? 1 : 0];
        if (is_trivial_quad(src)) {
            if (side1 == src && side2 == src)
                return set_trivial_quad(dst, !!src);

            int16_t *buf = alloc_tile(engine);
            *dst = (const Quad *)buf;
            if (!buf)
                return 0;
            if (!solid_tile_func[dir - 1](buf, tile[0], !!src, tile[2], param))
                return FLAG_VALID;
            free_tile(engine, buf);
            return set_trivial_quad(dst, !!src);
        }

        int16_t *buf = alloc_tile(engine);
        *dst = (const Quad *)buf;
        if (!buf)
            return 0;
        tile_func[dir - 1](buf, tile[0], tile[1], tile[2], param);
        return FLAG_VALID;
    }

    const Quad *input[3] = { side1, src, side2 };
    if (is_trivial_quad(side1))
        input[0] = &solid_sub_tile[side1 ? 1 : 0];
    if (is_trivial_quad(side2))
        input[2] = &solid_sub_tile[side2 ? 1 : 0];
    if (is_trivial_quad(src)) {
        if (side1 == src && side2 == src)
            return set_trivial_quad(dst, !!src);
        input[1] = &solid_sub_tile[src ? 1 : 0];
    }

    Quad *quad = alloc_quad(engine, NULL);
    *dst = quad;
    if (!quad)
        return 0;

    int flags = FLAG_ALL;
    int k[2][2] = { { 0, dir }, { dir ^ 3, 3 } };
    for (int i = 0; i < 2; ++i) {
        flags &= filter_quad(engine, &quad->child[k[i][0]],
                             input[0]->child[k[i][1]], input[1]->child[k[i][0]], input[1]->child[k[i][1]],
                             size_order - 1, dir, tile_func, solid_tile_func, param);
        if (!(flags & FLAG_VALID))
            return 0;
        flags &= filter_quad(engine, &quad->child[k[i][1]],
                             input[1]->child[k[i][0]], input[1]->child[k[i][1]], input[2]->child[k[i][0]],
                             size_order - 1, dir, tile_func, solid_tile_func, param);
        if (!(flags & FLAG_VALID))
            return 0;
    }
    if (flags == FLAG_VALID)
        return FLAG_VALID;
    free_quad(engine, quad, size_order);
    return set_trivial_quad(dst, flags & FLAG_SOLID);
}

int filter_tile_tree(const TileEngine *engine, TileTree *tree, int dir,
                     const FilterTileFunc tile_func[2],
                     const FilterSolidTileFunc solid_tile_func[2],
                     void *param)
{
    assert(is_valid_tree(engine, tree));
    assert(dir == 1 || dir == 2);

    if (tree->size_order < 0)
        return 1;

    assert(tree->size_order > engine->tile_order);

    int base1 = tree->x, base2 = tree->y;
    if (dir == 1) {
        int tmp = base1;
        base1 = base2;
        base2 = tmp;
    }
    int size_order = tree->size_order - 1;
    base2 -= 1 << size_order;

    const Quad *src[] = {
        tree->outside, tree->outside,
        tree->quad.child[0], tree->quad.child[dir],
        tree->outside, tree->outside,
        tree->quad.child[dir ^ 3], tree->quad.child[3],
        tree->outside, tree->outside,
    };
    const int n = 4;
    const Quad **ptr = src, *grid[2 * n];
    memset(grid, 0, sizeof(grid));

    int mask = trivial_quad_flag(!!tree->outside);
    int min1 = 2, max1 = -1, min2 = n, max2 = -1;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < n; ++j) {
            int res = filter_quad(engine, &grid[i * n + j],
                                  ptr[j + 0], ptr[j + 1], ptr[j + 2],
                                  size_order, dir, tile_func,
                                  solid_tile_func, param);
            if (!(res & FLAG_VALID)) {
                clear_quad_grid(engine, size_order, grid, 2, n);
                return 0;
            }
            if ((res & mask) == FLAG_VALID) {
                min1 = FFMIN(min1, i);
                max1 = FFMAX(max1, i);
                min2 = FFMIN(min2, j);
                max2 = FFMAX(max2, j);
            }
        }
        ptr += 4;
    }

    if (create_tree_from_grid(engine, tree,
                              base1, base2, size_order, dir,
                              min1, max1, min2, max2, grid, n))
        return 1;
    clear_quad_grid(engine, size_order, grid, 2, n);
    return 0;
}


static const double pi = 3.14159265358979323846264338327950288;

static void calc_gauss(double *res, double r2, int n)
{
    double alpha = 0.5 / r2;
    double norm = sqrt(alpha / pi);
    for (int i = 0; i < n; ++i)
        res[i] = norm * exp(-alpha * (i * i));  // XXX: optimize
}

static void calc_matrix(double mat[4][4], const double *base_exp, const int *index)
{
    for (int i = 0; i < 4; ++i) {
        mat[i][i] = base_exp[2 * index[i]] + 3 * base_exp[0] - 4 * base_exp[index[i]];
        for (int j = i + 1; j < 4; ++j)
            mat[i][j] = mat[j][i] =
                base_exp[index[i] + index[j]] + base_exp[index[j] - index[i]] +
                2 * (base_exp[0] - base_exp[index[i]] - base_exp[index[j]]);
    }

    // invert transpose
    for (int k = 0; k < 4; ++k) {
        int ip = k, jp = k;  // pivot
        double z = 1 / mat[ip][jp];
        mat[ip][jp] = 1;
        for (int i = 0; i < 4; ++i) {
            if (i == ip)
                continue;

            double mul = mat[i][jp] * z;
            mat[i][jp] = 0;
            for (int j = 0; j < 4; ++j)
                mat[i][j] -= mat[ip][j] * mul;
        }
        for (int j = 0; j < 4; ++j)
            mat[ip][j] *= z;
    }
}

static void calc_coeff(double mu[4], const int index[4], int prefilter, int level, double r2)
{
    double mul = pow(0.25, level);
    double r2b = (5.0 / 6) * (1 - mul);
    r2b += 0.5 * prefilter;
    r2 *= mul;

    double base_exp[13], avg_exp[7], mat[4][4];
    calc_gauss(base_exp, 2 * r2b, 13);  // XXX: overkill
    calc_gauss(avg_exp, r2 + r2b, 7);  // XXX: overkill
    calc_matrix(mat, base_exp, index);

    double vec[4];
    for (int i = 0; i < 4; ++i)
        vec[i] = 2 * (base_exp[0] - base_exp[index[i]] - avg_exp[0] + avg_exp[index[i]]);

    for (int i = 0; i < 4; ++i) {
        double res = 0;
        for (int j = 0; j < 4; ++j)
            res += mat[i][j] * vec[j];
        mu[i] = res;
    }
}

int blur_tile_tree(const TileEngine *engine, TileTree *tree, double r2)
{
    static const int index[][4] = {
        { 1, 2, 3, 4 },
        { 1, 2, 3, 5 },
        { 1, 2, 4, 6 },
    };

    int level, prefilter, filter;
    double mu[4];

    if (r2 < 2.4) {  // XXX: use compact filters
        level = prefilter = filter = 0;

        if (r2 < 0.5) {
            mu[1] = 0.17 * r2 * r2 * r2;
            mu[0] = r2 - 4 * mu[1];
            mu[2] = mu[3] = 0;
        } else {
            double alpha = 0.5 / r2;
            double norm = 2 * sqrt(alpha / pi);
            for (int i = 1; i <= 4; ++i)
                mu[i - 1] = norm * exp(-alpha * (i * i));  // XXX: optimize
        }
    } else {
        if (r2 < 8) {
            level = 0;

            if (r2 < 3.5)
                prefilter = 1;
            else if (r2 < 5.3)
                prefilter = 2;
            else
                prefilter = 3;

            filter = prefilter - 1;
        } else {
            level = 1;
            double val = r2 + 1;
            for (; val > 33; val /= 4)
                ++level;

            if (val < 14.5)
                prefilter = 0;
            else if (val < 22.5)
                prefilter = 1;
            else
                prefilter = 2;

            filter = prefilter;
        }
        calc_coeff(mu, index[filter], prefilter, level, r2);
    }

    int16_t coeff[4];
    for (int i = 0; i < 4; ++i)
        coeff[i] = (int)(0x8000 * mu[i] + 0.5);

    for (int i = 0; i < level; ++i)
        if (!shrink_tile_tree(engine, tree, 2))
            return 0;
    for (int i = 0; i < level; ++i)
        if (!shrink_tile_tree(engine, tree, 1))
            return 0;

    if (prefilter) {
        if (!filter_tile_tree(engine, tree, 2,
                              engine->pre_blur[prefilter - 1],
                              engine->pre_blur_solid[prefilter - 1],
                              NULL))
            return 0;
        if (!filter_tile_tree(engine, tree, 1,
                              engine->pre_blur[prefilter - 1],
                              engine->pre_blur_solid[prefilter - 1],
                              NULL))
            return 0;
    }

    if (!filter_tile_tree(engine, tree, 2,
                          engine->main_blur[filter],
                          engine->main_blur_solid[filter],
                          coeff))
        return 0;
    if (!filter_tile_tree(engine, tree, 1,
                          engine->main_blur[filter],
                          engine->main_blur_solid[filter],
                          coeff))
        return 0;

    for (int i = 0; i < level; ++i)
        if (!expand_tile_tree(engine, tree, 1))
            return 0;
    for (int i = 0; i < level; ++i)
        if (!expand_tile_tree(engine, tree, 2))
            return 0;

    return 1;
}


static int shift_quad(const TileEngine *engine, const Quad **dst,
                      const Quad *src0, const Quad *src1,
                      const Quad *src2, const Quad *src3,
                      int size_order, int dx, int dy)
{
    assert(size_order >= engine->tile_order);
    assert(dx >= 0 && dx < (64 << size_order));
    assert(dy >= 0 && dy < (64 << size_order));

    if (!dx && !dy)
        return copy_quad(engine, dst, src0, size_order);

    const Quad *src[] = { src0, src1, src2, src3 };
    if (size_order == engine->tile_order) {
        int flags = FLAG_EMPTY | FLAG_SOLID;
        const int16_t *tile[4];
        for (int i = 0; i < 4; ++i) {
            if (is_trivial_quad(src[i])) {
                tile[i] = engine->solid_tile[src[i] ? 1 : 0];
                flags &= trivial_quad_flag(!!src[i]);
            } else {
                tile[i] = (const int16_t *)src[i];
                flags = 0;
            }
        }
        if (flags)
            return set_trivial_quad(dst, !!src0);

        int16_t *buf = alloc_tile(engine);
        *dst = (const Quad *)buf;
        if (!buf)
            return 0;
        int res = engine->shift(buf, tile[0], tile[1], tile[2], tile[3], dx, dy);
        if (!res)
            return FLAG_VALID;
        free_tile(engine, buf);
        return set_trivial_quad(dst, res > 0);
    }

    int flags = FLAG_EMPTY | FLAG_SOLID;
    for (int i = 0; i < 4; ++i) {
        if (is_trivial_quad(src[i])) {
            src[i] = &solid_sub_tile[src[i] ? 1 : 0];
            flags &= trivial_quad_flag(!!src[i]);
        } else
            flags = 0;
    }
    if (flags)
        return set_trivial_quad(dst, !!src0);

    Quad *quad = alloc_quad(engine, NULL);
    *dst = quad;
    if (!quad)
        return 0;

    int mask = (32 << size_order) - 1;
    int offs = 0;
    if (dx & ~mask)
        offs |= 1;
    if (dy & ~mask)
        offs |= 2;
    dx &= mask;
    dy &= mask;

    const Quad *input[3][3] = {
        { src[0]->child[offs], src[(offs & 1) | 0]->child[offs ^ 1], src[1]->child[offs] },
        { src[(offs & 2) | 0]->child[offs ^ 2], src[offs]->child[offs ^ 3], src[(offs & 2) | 1]->child[offs ^ 2] },
        { src[2]->child[offs], src[(offs & 1) | 2]->child[offs ^ 1], src[3]->child[offs] },
    };

    flags = FLAG_ALL;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            flags &= shift_quad(engine, &quad->child[2 * i + j],
                                input[i + 0][j + 0], input[i + 0][j + 1],
                                input[i + 1][j + 0], input[i + 1][j + 1],
                                size_order - 1, dx, dy);
            if (!(flags & FLAG_VALID))
                return 0;
        }
    if (flags == FLAG_VALID)
        return FLAG_VALID;
    free_quad(engine, quad, size_order);
    return set_trivial_quad(dst, flags & FLAG_SOLID);
}

int shift_tile_tree(const TileEngine *engine, TileTree *tree, int dx, int dy)
{
    assert(is_valid_tree(engine, tree));

    if (tree->size_order < 0)
        return 1;

    assert(tree->size_order > engine->tile_order);

    dx = -dx;
    dy = -dy;
    int size_order = tree->size_order - 1;
    int base1 = tree->y - (((dy >> (size_order + 6)) + 1) << size_order);
    int base2 = tree->x - (((dx >> (size_order + 6)) + 1) << size_order);
    dx &= (64 << size_order) - 1;
    dy &= (64 << size_order) - 1;

    const Quad *src[] = {
        tree->outside, tree->outside, tree->outside, tree->outside,
        tree->quad.child[0], tree->quad.child[1], tree->outside,
        tree->quad.child[2], tree->quad.child[3], tree->outside,
        tree->outside, tree->outside, tree->outside,
    };
    const int n = 3;
    const Quad **ptr = src, *grid[n * n];
    memset(grid, 0, sizeof(grid));

    int mask = trivial_quad_flag(!!tree->outside);
    int min1 = n, max1 = -1, min2 = n, max2 = -1;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            int res = shift_quad(engine, &grid[i * n + j],
                                 ptr[j + 0], ptr[j + 1], ptr[j + 3], ptr[j + 4],
                                 size_order, dx, dy);
            if (!(res & FLAG_VALID)) {
                clear_quad_grid(engine, size_order, grid, n, n);
                return 0;
            }
            if ((res & mask) == FLAG_VALID) {
                min1 = FFMIN(min1, i);
                max1 = FFMAX(max1, i);
                min2 = FFMIN(min2, j);
                max2 = FFMAX(max2, j);
            }
        }
        ptr += 3;
    }

    if (create_tree_from_grid(engine, tree,
                              base1, base2, size_order, 1,
                              min1, max1, min2, max2, grid, n))
        return 1;
    clear_quad_grid(engine, size_order, grid, n, n);
    return 0;
}
