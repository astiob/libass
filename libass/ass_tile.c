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


static inline const Quad *trivial_quad(int solid)
{
    return solid ? SOLID_QUAD : EMPTY_QUAD;
}

static inline int is_trivial_quad(const Quad *quad)
{
    return !quad || quad == SOLID_QUAD || quad == INVALID_QUAD;
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

const Quad *copy_quad(const TileEngine *engine, const Quad *quad, int size_order)
{
    assert(size_order >= engine->tile_order && quad != INVALID_QUAD);

    if (!quad || quad == SOLID_QUAD)
        return quad;

    if (size_order == engine->tile_order)
        return copy_tile(engine, quad);

    inc_ref_count(quad, sizeof(Quad));
    return quad;
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
        res->quad.child[i] = copy_quad(engine, src->quad.child[i], src->size_order - 1);
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

static inline int is_valid_tree(const TileEngine *engine, const TileTree *tree)
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
    return !(tree->x & mask) && !(tree->y & mask);
}


void finalize_quad(const TileEngine *engine, uint8_t *buf, ptrdiff_t stride,
                   const Quad *quad, int size_order)
{
    assert(size_order >= engine->tile_order && quad != INVALID_QUAD);

    if (!quad || quad == SOLID_QUAD) {
        engine->finalize_solid(buf, stride, size_order, quad != EMPTY_QUAD);
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


static inline int find_split(int *split, int start, int count, int value)
{
    int res;
    for (res = start; res < count; ++res)
        if (split[res] >= value)
            break;
    return res;
}

static const Quad *build_axis_aligned_quad(const TileEngine *engine,
                                           int x, int y, int size_order,
                                           int *x_split, int x_count,
                                           int *y_split, int y_count,
                                           const Quad **fill, int stride)
{
    if (!x_count && !y_count)
        return copy_quad(engine, *fill, size_order);

    assert(size_order > engine->tile_order);

    int size = 1 << (size_order - 1);

    int n_x[2], offs_x[2];
    offs_x[0] = 0;
    n_x[0] = find_split(x_split, offs_x[0], x_count, x + size);
    offs_x[1] = find_split(x_split, n_x[0], x_count, x + size + 1);
    n_x[1] = x_count - offs_x[1];

    int n_y[2], offs_y[2];
    offs_y[0] = 0;
    n_y[0] = find_split(y_split, offs_y[0], y_count, y + size);
    offs_y[1] = find_split(y_split, n_y[0], y_count, y + size + 1);
    n_y[1] = y_count - offs_y[1];

    Quad *quad = alloc_quad(engine, INVALID_QUAD);
    if (!quad)
        return INVALID_QUAD;

    int flag = 3;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            const Quad *res = build_axis_aligned_quad(engine,
                                                      x + j * size, y + i * size, size_order - 1,
                                                      x_split + offs_x[j], n_x[j],
                                                      y_split + offs_y[i], n_y[i],
                                                      fill + offs_y[i] * stride + offs_x[j], stride);
            quad->child[2 * i + j] = res;
            if (res == INVALID_QUAD) {
                free_quad(engine, quad, size_order);
                return INVALID_QUAD;
            }
            if (res == EMPTY_QUAD)
                flag &= 1;
            else if (res == SOLID_QUAD)
                flag &= 2;
            else
                flag = 0;
        }
    if (!flag)
        return quad;
    free_quad(engine, quad, size_order);
    return flag == 1 ? EMPTY_QUAD : SOLID_QUAD;
}

static int build_axis_aligned_tree(const TileEngine *engine, Quad *quad,
                                   int x, int y, int size_order,
                                   int *x_split, int x_count,
                                   int *y_split, int y_count,
                                   const Quad **fill, int stride)
{
    assert(size_order > engine->tile_order);

    int size = 1 << (size_order - 1);

    int n_x[2], offs_x[2];
    offs_x[0] = find_split(x_split, 0, x_count, x + 1);
    n_x[0] = find_split(x_split, offs_x[0], x_count, x + size);
    offs_x[1] = find_split(x_split, n_x[0], x_count, x + size + 1);
    n_x[1] = find_split(x_split, offs_x[1], x_count, x + 2 * size);
    n_x[0] -= offs_x[0];
    n_x[1] -= offs_x[1];

    int n_y[2], offs_y[2];
    offs_y[0] = find_split(y_split, 0, y_count, y + 1);
    n_y[0] = find_split(y_split, offs_y[0], y_count, y + size);
    offs_y[1] = find_split(y_split, n_y[0], y_count, y + size + 1);
    n_y[1] = find_split(y_split, offs_y[1], y_count, y + 2 * size);
    n_y[0] -= offs_y[0];
    n_y[1] -= offs_y[1];

    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            const Quad *res = build_axis_aligned_quad(engine,
                                                      x + j * size, y + i * size, size_order - 1,
                                                      x_split + offs_x[j], n_x[j],
                                                      y_split + offs_y[i], n_y[i],
                                                      fill + offs_y[i] * stride + offs_x[j], stride);
            quad->child[2 * i + j] = res;
            if (res == INVALID_QUAD)
                return 0;
        }
    return 1;
}

TileTree *create_rectangle(const TileEngine *engine,
                           int x_min, int y_min, int x_max, int y_max,
                           int inverse)
{
    assert(x_min < x_max && y_min < y_max);

    int tile_size = 1 << engine->tile_order;
    int mask = (tile_size << 6) - 1;

    enum { MIN_POS, CNT_POS, MAX_POS, ALL_POS };

    int x[4], n_x = 1;
    int x_pos[4] = { -1, -1, -1, -1 };
    x[0] = (x_min & ~mask) >> 6;
    if (x_min & mask) {
        x_pos[MIN_POS] = n_x;
        x[n_x] = x[n_x - 1] + tile_size;
        ++n_x;
    }
    x[n_x] = (x_max & ~mask) >> 6;
    if (x[n_x] < x[n_x - 1])
        x_pos[MAX_POS] = x_pos[ALL_POS] = n_x;
    else {
        if (x[n_x] > x[n_x - 1]) {
            x_pos[CNT_POS] = n_x;
            ++n_x;
        }
        if (x_max & mask) {
            x_pos[MAX_POS] = n_x;
            x[n_x] = x[n_x - 1] + tile_size;
            ++n_x;
        }
    }

    int y[4], n_y = 1;
    int y_pos[4] = { -1, -1, -1, -1 };
    y[0] = (y_min & ~mask) >> 6;
    if (y_min & mask) {
        y_pos[MIN_POS] = n_y;
        y[n_y] = y[n_y - 1] + tile_size;
        ++n_y;
    }
    y[n_y] = (y_max & ~mask) >> 6;
    if (y[n_y] < y[n_y - 1])
        y_pos[MAX_POS] = y_pos[ALL_POS] = n_y;
    else {
        if (y[n_y] > y[n_y - 1]) {
            y_pos[CNT_POS] = n_y;
            ++n_y;
        }
        if (y_max & mask) {
            y_pos[MAX_POS] = n_y;
            y[n_y] = y[n_y - 1] + tile_size;
            ++n_y;
        }
    }

    int32_t ab = (int32_t)1 << 30, scale = inverse ? -ab : ab;
    CombineTileFunc combine = engine->combine[inverse ? COMBINE_ADD : COMBINE_MUL];
    int error = 0;

    int16_t *tile_x[2] = { NULL, NULL };
    if (x_pos[MIN_POS] >= 0) {
        tile_x[0] = alloc_tile(engine);
        if (tile_x[0])
            engine->fill_halfplane(tile_x[0], ab, 0, (int64_t)(x_min & mask) << 30, -scale);
        else
            error = 1;
    }
    if (x_pos[MAX_POS] >= 0) {
        tile_x[1] = alloc_tile(engine);
        if (tile_x[1])
            engine->fill_halfplane(tile_x[1], ab, 0, (int64_t)(x_max & mask) << 30, scale);
        else
            error = 1;
    }
    if (x_pos[ALL_POS] >= 0) {
        combine(tile_x[0], tile_x[0], tile_x[1]);
        x_pos[MAX_POS] = -1;
    }

    int16_t *tile_y[2] = { NULL, NULL };
    if (y_pos[MIN_POS] >= 0) {
        tile_y[0] = alloc_tile(engine);
        if (tile_y[0])
            engine->fill_halfplane(tile_y[0], 0, ab, (int64_t)(y_min & mask) << 30, -scale);
        else
            error = 1;
    }
    if (y_pos[MAX_POS] >= 0) {
        tile_y[1] = alloc_tile(engine);
        if (tile_y[1])
            engine->fill_halfplane(tile_y[1], 0, ab, (int64_t)(y_max & mask) << 30, scale);
        else
            error = 1;
    }
    if (y_pos[ALL_POS] >= 0) {
        combine(tile_y[0], tile_y[0], tile_y[1]);
        y_pos[MAX_POS] = -1;
    }

    if (error) {
        for (int i = 0; i < 2; ++i)
            if (tile_x[i])
                free_tile(engine, tile_x[i]);
        for (int i = 0; i < 2; ++i)
            if (tile_y[i])
                free_tile(engine, tile_y[i]);
        return NULL;
    }

    const int n = 5;
    const Quad *fill[n * n];
    const Quad *empty = inverse ? SOLID_QUAD : EMPTY_QUAD;
    for (int k = 0; k < n * n; ++k)
        fill[k] = empty;

    if (y_pos[MIN_POS] >= 0) {
        if (x_pos[MIN_POS] >= 0) {
            int16_t *buf = alloc_tile(engine);
            fill[n * y_pos[MIN_POS] + x_pos[MIN_POS]] = (const Quad *)buf;
            if (buf)
                combine(buf, tile_x[0], tile_y[0]);
            else
                error = 1;
        }
        if (x_pos[CNT_POS] >= 0)
            fill[n * y_pos[MIN_POS] + x_pos[CNT_POS]] = (const Quad *)copy_tile(engine, tile_y[0]);
        if (x_pos[MAX_POS] >= 0) {
            int16_t *buf = alloc_tile(engine);
            fill[n * y_pos[MIN_POS] + x_pos[MAX_POS]] = (const Quad *)buf;
            if (buf)
                combine(buf, tile_x[1], tile_y[0]);
            else
                error = 1;
        }
    }
    if (y_pos[CNT_POS] >= 0) {
        if (x_pos[MIN_POS] >= 0)
            fill[n * y_pos[CNT_POS] + x_pos[MIN_POS]] = (const Quad *)copy_tile(engine, tile_x[0]);
        if (x_pos[CNT_POS] >= 0)
            fill[n * y_pos[CNT_POS] + x_pos[CNT_POS]] = inverse ? EMPTY_QUAD : SOLID_QUAD;
        if (x_pos[MAX_POS] >= 0)
            fill[n * y_pos[CNT_POS] + x_pos[MAX_POS]] = (const Quad *)copy_tile(engine, tile_x[1]);
    }
    if (y_pos[MAX_POS] >= 0) {
        if (x_pos[MIN_POS] >= 0) {
            int16_t *buf = alloc_tile(engine);
            fill[n * y_pos[MAX_POS] + x_pos[MIN_POS]] = (const Quad *)buf;
            if (buf)
                combine(buf, tile_x[0], tile_y[1]);
            else
                error = 1;
        }
        if (x_pos[CNT_POS] >= 0)
            fill[n * y_pos[MAX_POS] + x_pos[CNT_POS]] = (const Quad *)copy_tile(engine, tile_y[1]);
        if (x_pos[MAX_POS] >= 0) {
            int16_t *buf = alloc_tile(engine);
            fill[n * y_pos[MAX_POS] + x_pos[MAX_POS]] = (const Quad *)buf;
            if (buf)
                combine(buf, tile_x[1], tile_y[1]);
            else
                error = 1;
        }
    }

    for (int i = 0; i < 2; ++i)
        if (tile_x[i])
            free_tile(engine, tile_x[i]);
    for (int i = 0; i < 2; ++i)
        if (tile_y[i])
            free_tile(engine, tile_y[i]);

    TileTree *tree = error ? NULL : alloc_tile_tree(engine, empty);
    if (tree) {
        calc_tree_bounds(engine, tree, x[0], y[0], x[n_x - 1], y[n_y - 1]);
        if (!build_axis_aligned_tree(engine, &tree->quad,
                                     tree->x, tree->y, tree->size_order,
                                     x, n_x, y, n_y, fill, n)) {
            free_tile_tree(engine, tree);
            tree = NULL;
        }
    }
    for (int k = 0; k < n * n; ++k)
        free_quad(engine, fill[k], engine->tile_order);
    return tree;
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
    assert(!is_trivial_quad(dst) && src != outside && src != INVALID_QUAD);
    assert(!outside || outside == SOLID_QUAD);

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

static const Quad *extract_sub_quad(const TileEngine *engine, const Quad *src,
                                    int dst_order, int src_order, int delta_x, int delta_y)
{
    assert(src_order > dst_order && dst_order >= engine->tile_order);
    assert(!(delta_x & ((1 << dst_order) - 1)) && !(delta_y & ((1 << dst_order) - 1)));
    assert(!is_trivial_quad(src));

    --src_order;
    const Quad *next = src->child[get_child_index(delta_x, delta_y, src_order)];

    if (src_order == dst_order)
        return copy_quad(engine, next, src_order);

    if (!next || next == SOLID_QUAD)
        return next;

    assert(next != INVALID_QUAD);
    return extract_sub_quad(engine, next,
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
            old.quad.child[i] = INVALID_QUAD;
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
            dst->quad.child[i] = extract_sub_quad(engine, &old.quad, dst->size_order - 1,
                                                  old.size_order, delta_x, delta_y);
        }
    }
    for (int i = 0; i < 4; ++i)
        free_quad(engine, old.quad.child[i], old.size_order - 1);
    return res;
}


static const Quad *combine_quad(const TileEngine *engine,
                                const Quad *src1, const Quad *src2, int size_order,
                                CombineTileFunc tile_func, int op_flags)
{
    assert(size_order >= engine->tile_order);
    assert(src1 != INVALID_QUAD && src2 != INVALID_QUAD);

    if (src1 == trivial_quad(op_flags & 1) || src2 == trivial_quad(op_flags & 2))
        return trivial_quad(op_flags & 1);
    if (src2 == trivial_quad(~op_flags & 2))
        return copy_quad(engine, src1, size_order);

    const int16_t *tile = (const int16_t *)src1;
    if (src1 == trivial_quad(~op_flags & 1)) {
        if (trivial_quad(op_flags & 1) == trivial_quad(op_flags & 2))
            return copy_quad(engine, src2, size_order);
        tile = engine->solid_tile[src1 ? 1 : 0];
        src1 = &solid_sub_tile[src1 ? 1 : 0];
    }

    if (size_order == engine->tile_order) {
        int16_t *buf = alloc_tile(engine);
        if (!buf)
            return INVALID_QUAD;
        if (tile_func(buf, tile, (const int16_t *)src2))
            return (const Quad *)buf;
        free_tile(engine, buf);
        return trivial_quad(op_flags & 1);
    }

    Quad *quad = alloc_quad(engine, trivial_quad(op_flags & 1));
    if (!quad)
        return INVALID_QUAD;

    enum {
        SRC1_FLAG  = 1 << 0,
        SRC2_FLAG  = 1 << 1,
        EMPTY_FLAG = 1 << 2,
        SOLID_FLAG = 1 << 3,
        ERROR_FLAG = -1
    };

    int flags = SRC1_FLAG | SRC2_FLAG | EMPTY_FLAG | SOLID_FLAG;
    for (int i = 0; i < 4; ++i) {
        quad->child[i] = combine_quad(engine, src1->child[i], src2->child[i],
                                      size_order - 1, tile_func, op_flags);
        if (quad->child[i] != src1->child[i])
            flags &= ~SRC1_FLAG;
        if (quad->child[i] != src2->child[i])
            flags &= ~SRC2_FLAG;
        switch ((intptr_t)quad->child[i]) {
            case (intptr_t)EMPTY_QUAD: flags &= ~SOLID_FLAG; continue;
            case (intptr_t)SOLID_QUAD: flags &= ~EMPTY_FLAG; continue;
            case (intptr_t)INVALID_QUAD: break;
            default: flags &= ~(EMPTY_FLAG | SOLID_FLAG); continue;
        }
        flags = ERROR_FLAG;
        break;
    }
    const Quad *res;
    switch (flags)
    {
        case 0: return quad;
        case SRC1_FLAG: res = copy_quad(engine, src1, size_order); break;
        case SRC2_FLAG: res = copy_quad(engine, src2, size_order); break;
        case EMPTY_FLAG: res = EMPTY_QUAD; break;
        case SOLID_FLAG: res = SOLID_QUAD; break;
        default: assert(flags == ERROR_FLAG); res = INVALID_QUAD;
    }
    free_quad(engine, quad, size_order);
    return res;
}

static const Quad *combine_small_quad(const TileEngine *engine, const Quad *src1, const Quad *src2,
                                      int src1_order, int src2_order, int delta_x, int delta_y,
                                      CombineTileFunc tile_func, int op_flags)
{
    assert(src1_order > src2_order && src2_order >= engine->tile_order);
    assert(!(delta_x & ((1 << src2_order) - 1)) && !(delta_y & ((1 << src2_order) - 1)));
    assert(src1 != INVALID_QUAD && src2 != INVALID_QUAD);
    assert(src2 != trivial_quad(~op_flags & 2));

    --src1_order;
    int index = get_child_index(delta_x, delta_y, src1_order);

    const Quad *next = src1;
    if (src1 && src1 != SOLID_QUAD)
        next = src1->child[index];

    const Quad *dominant_quad = trivial_quad(op_flags & 1);
    if (next == dominant_quad)
        return copy_quad(engine, src1, src1_order + 1);

    const Quad *quad;
    if (src1_order == src2_order)
        quad = combine_quad(engine, next, src2,
                            src2_order, tile_func, op_flags);
    else
        quad = combine_small_quad(engine, next, src2,
                                  src1_order, src2_order, delta_x, delta_y,
                                  tile_func, op_flags);
    if (quad == INVALID_QUAD)
        return INVALID_QUAD;
    if (quad == next)
        return copy_quad(engine, src1, src1_order + 1);

    if (quad == dominant_quad && src1 != trivial_quad(~op_flags & 1)) {
        if (src1 == dominant_quad)
            return dominant_quad;
        int empty = 1;
        for (int i = 0; i < 4; ++i)
            if (i != index && src1->child[i] == dominant_quad)
                empty = 0;
        if (empty)
            return dominant_quad;
    }

    if (!src1 || src1 == SOLID_QUAD) {
        Quad *res = alloc_quad(engine, src1);
        if (!res)
            return INVALID_QUAD;
        res->child[index] = quad;
        return res;
    }

    Quad *res = alloc_quad(engine, INVALID_QUAD);
    if (!res)
        return INVALID_QUAD;
    for (int i = 0; i < 4; ++i)
        res->child[i] = (i == index ? quad :
            copy_quad(engine, src1->child[i], src1_order));
    return res;
}

static const Quad *combine_large_quad(const TileEngine *engine, const Quad *src1, const Quad *src2,
                                      int src1_order, int src2_order, int delta_x, int delta_y,
                                      CombineTileFunc tile_func, int op_flags)
{
    assert(src2_order > src1_order && src1_order >= engine->tile_order);
    assert(!(delta_x & ((1 << src1_order) - 1)) && !(delta_y & ((1 << src1_order) - 1)));
    assert(src1 != INVALID_QUAD && src1 != trivial_quad(op_flags & 1));
    assert(!is_trivial_quad(src2));

    --src2_order;
    int index = get_child_index(delta_x, delta_y, src2_order);

    const Quad *next = src2;
    if (src2 && src2 != SOLID_QUAD)
        next = src2->child[index];

    if (src1_order == src2_order)
        return combine_quad(engine, src1, next,
                            src1_order, tile_func, op_flags);

    if (next == trivial_quad(op_flags & 2))
        return trivial_quad(op_flags & 1);
    if (next == trivial_quad(~op_flags & 2))
        return copy_quad(engine, src1, src1_order);

    return combine_large_quad(engine, src1, next,
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
            int index = get_child_index(delta_x, delta_y, dst->size_order - 1);
            const Quad *quad = combine_small_quad(engine,
                                                  dst->quad.child[index], src->quad.child[i],
                                                  dst->size_order - 1, src->size_order - 1,
                                                  delta_x, delta_y, tile_func, op_flags[op]);
            if (quad == INVALID_QUAD)
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
            const Quad *quad = combine_large_quad(engine, dst->quad.child[i], &src->quad,
                                                  dst->size_order - 1, src->size_order,
                                                  delta_x, delta_y, tile_func, op_flags[op]);
            if (quad == INVALID_QUAD)
                return 0;
            free_quad(engine, dst->quad.child[i], dst->size_order - 1);
            dst->quad.child[i] = quad;
        }
    }
    assert(is_valid_tree(engine, dst));
    return 1;  // XXX: shrink tree
}


int create_tree_from_grid(const TileEngine *engine, TileTree *tree,
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
            tree->quad.child[i] = copy_quad(engine, quad->child[i], size_order - 1);
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
            grid[i * stride + j] = INVALID_QUAD;
        }
    assert(is_valid_tree(engine, tree));
    return 1;
}

void clear_quad_grid(const TileEngine *engine, int size_order,
                     int min1, int max1, int min2, int max2,
                     const Quad **grid, int stride)
{
    for (int i = min1; i <= max1; ++i)
        for (int j = min2; j <= max2; ++j)
            free_quad(engine, grid[i * stride + j], size_order);
}


static const Quad *shrink_quad(const TileEngine *engine,
                               const Quad *side1, const Quad *src1,
                               const Quad *src2, const Quad *side2,
                               int size_order, int dir)
{
    assert(size_order >= engine->tile_order);
    assert(side1 != INVALID_QUAD && side2 != INVALID_QUAD);
    assert(src1 != INVALID_QUAD && src2 != INVALID_QUAD);
    assert(dir == 1 || dir == 2);

    if (size_order == engine->tile_order) {
        const int16_t *tile[4] = {
            (const int16_t *)side1, (const int16_t *)src1,
            (const int16_t *)src2, (const int16_t *)side2
        };
        if (!side1 || side1 == SOLID_QUAD)
            tile[0] = engine->solid_tile[side1 ? 1 : 0];
        if (!side2 || side2 == SOLID_QUAD)
            tile[3] = engine->solid_tile[side2 ? 1 : 0];
        if (!src1 || src1 == SOLID_QUAD) {
            if (src2 == src1) {
                if (side1 == src1 && side2 == src1)
                    return src1;

                int16_t *buf = alloc_tile(engine);
                if (!buf)
                    return INVALID_QUAD;
                if (engine->shrink_solid[dir - 1](buf, tile[0], src1 != EMPTY_QUAD, tile[3]))
                    return (const Quad *)buf;
                free_tile(engine, buf);
                return src1;
            }
            tile[1] = engine->solid_tile[src1 ? 1 : 0];
        }
        if (!src2 || src2 == SOLID_QUAD)
            tile[2] = engine->solid_tile[src2 ? 1 : 0];

        int16_t *buf = alloc_tile(engine);
        if (!buf)
            return INVALID_QUAD;
        engine->shrink[dir - 1](buf, tile[0], tile[1], tile[2], tile[3]);
        return (const Quad *)buf;
    }

    const Quad *empty = INVALID_QUAD;
    const Quad *input[4] = { side1, src1, src2, side2 };
    if (!side1 || side1 == SOLID_QUAD)
        input[0] = &solid_sub_tile[side1 ? 1 : 0];
    if (!side2 || side2 == SOLID_QUAD)
        input[3] = &solid_sub_tile[side2 ? 1 : 0];
    if (!src1 || src1 == SOLID_QUAD) {
        if (src2 == src1) {
            if (side1 == src1 && side2 == src1)
                return src1;
            empty = src1;
        }
        input[1] = &solid_sub_tile[src1 ? 1 : 0];
    }
    if (!src2 || src2 == SOLID_QUAD)
        input[2] = &solid_sub_tile[src2 ? 1 : 0];

    Quad *quad = alloc_quad(engine, INVALID_QUAD);
    if (!quad)
        return INVALID_QUAD;

    int k[2][2] = { { 0, dir }, { dir ^ 3, 3 } };
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            const Quad *res = shrink_quad(engine,
                                          input[j + 0]->child[k[i][1]], input[j + 1]->child[k[i][0]],
                                          input[j + 1]->child[k[i][1]], input[j + 2]->child[k[i][0]],
                                          size_order - 1, dir);
            if (res == INVALID_QUAD) {
                free_quad(engine, quad, size_order);
                return INVALID_QUAD;
            }
            if (res != empty)
                empty = INVALID_QUAD;
            quad->child[k[i][j]] = res;
        }
    if (empty != INVALID_QUAD) {
        free_quad(engine, quad, size_order);
        return empty;
    }
    return quad;
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

    int error = 0;
    int min1 = 2, max1 = -1, min2 = n, max2 = -1;
    for (int i = 0; i < 2; ++i) {
        for (int j = j0; j < n; ++j) {
            grid[i * n + j] = shrink_quad(engine,
                                           ptr[2 * j + 0], ptr[2 * j + 1],
                                           ptr[2 * j + 2], ptr[2 * j + 3],
                                           size_order, dir);
            if (grid[i * n + j] == INVALID_QUAD)
                error = 1;
            else if (grid[i * n + j] != tree->outside) {
                min1 = FFMIN(min1, i);
                max1 = FFMAX(max1, i);
                min2 = FFMIN(min2, j);
                max2 = FFMAX(max2, j);
            }
        }
        ptr += 5;
    }

    if (!error && create_tree_from_grid(engine, tree,
                                        base1, base2, size_order, dir,
                                        min1, max1, min2, max2, grid, n))
        return 1;
    clear_quad_grid(engine, size_order, min1, max1, min2, max2, grid, n);
    return 0;
}


static int expand_quad(const TileEngine *engine,
                       const Quad **dst1, const Quad **dst2,
                       const Quad *side1, const Quad *src, const Quad *side2,
                       int size_order, int dir)
{
    assert(size_order >= engine->tile_order);
    assert(side1 != INVALID_QUAD && side2 != INVALID_QUAD);
    assert(src != INVALID_QUAD);
    assert(dir == 1 || dir == 2);

    if (size_order == engine->tile_order) {
        const int16_t *tile[3] = {
            (const int16_t *)side1, (const int16_t *)src, (const int16_t *)side2
        };
        if (!src || src == SOLID_QUAD) {
            if (!side1 || side1 == SOLID_QUAD)
                tile[0] = engine->solid_tile[side1 ? 1 : 0];
            if (!side2 || side2 == SOLID_QUAD)
                tile[2] = engine->solid_tile[side2 ? 1 : 0];

            if (side1 == src)
                *dst1 = src;
            else {
                int16_t *buf = alloc_tile(engine);
                if (!buf)
                    return 0;
                if (engine->expand_solid_in[0][dir - 1](buf, tile[0], src != EMPTY_QUAD))
                    *dst1 = (const Quad *)buf;
                else {
                    free_tile(engine, buf);
                    *dst1 = src;
                }
            }
            if (side2 == src)
                *dst2 = src;
            else {
                int16_t *buf = alloc_tile(engine);
                if (!buf)
                    return 0;
                if (engine->expand_solid_in[1][dir - 1](buf, tile[2], src != EMPTY_QUAD))
                    *dst2 = (const Quad *)buf;
                else {
                    free_tile(engine, buf);
                    *dst2 = src;
                }
            }
            return 1;
        }

        int16_t *buf1 = alloc_tile(engine);
        *dst1 = (const Quad *)buf1;
        int16_t *buf2 = alloc_tile(engine);
        *dst2 = (const Quad *)buf2;
        if (!buf1 || !buf2)
            return 0;

        if (side1 && side1 != SOLID_QUAD)
            engine->expand[0][dir - 1](buf1, tile[0], tile[1]);
        else if (!engine->expand_solid_out[0][dir - 1](buf1, tile[1], side1 != EMPTY_QUAD)) {
            free_tile(engine, buf1);
            *dst1 = side1;
        }
        if (side2 && side2 != SOLID_QUAD)
            engine->expand[1][dir - 1](buf2, tile[2], tile[1]);
        else if (!engine->expand_solid_out[1][dir - 1](buf2, tile[1], side2 != EMPTY_QUAD)) {
            free_tile(engine, buf2);
            *dst2 = side2;
        }
        return 1;
    }

    const Quad *input[3] = { side1, src, side2 };
    if (!side1 || side1 == SOLID_QUAD)
        input[0] = &solid_sub_tile[side1 ? 1 : 0];
    if (!side2 || side2 == SOLID_QUAD)
        input[2] = &solid_sub_tile[side2 ? 1 : 0];
    if (!src || src == SOLID_QUAD) {
        if (side1 == src && side2 == src) {
            *dst1 = *dst2 = src;
            return 1;
        }
        input[1] = &solid_sub_tile[src ? 1 : 0];
    }

    Quad *quad1 = alloc_quad(engine, INVALID_QUAD);
    *dst1 = quad1;
    Quad *quad2 = alloc_quad(engine, INVALID_QUAD);
    *dst2 = quad2;
    if (!quad1 || !quad2)
        return 0;

    int k[2][2] = { { 0, dir }, { dir ^ 3, 3 } };
    for (int i = 0; i < 2; ++i) {
        if (!expand_quad(engine, &quad1->child[k[i][0]], &quad1->child[k[i][1]],
                         input[0]->child[k[i][1]], input[1]->child[k[i][0]], input[1]->child[k[i][1]],
                         size_order - 1, dir))
            return 0;
        if (!expand_quad(engine, &quad2->child[k[i][0]], &quad2->child[k[i][1]],
                         input[1]->child[k[i][0]], input[1]->child[k[i][1]], input[2]->child[k[i][0]],
                         size_order - 1, dir))
            return 0;
    }
    int flag = 15;
    for (int i = 0; i < 4; ++i) {
        assert(quad1->child[i] != INVALID_QUAD && quad2->child[i] != INVALID_QUAD);
        if (quad1->child[i] != EMPTY_QUAD)
            flag &= ~(1 << 0);
        if (quad1->child[i] != SOLID_QUAD)
            flag &= ~(1 << 1);
        if (quad2->child[i] != EMPTY_QUAD)
            flag &= ~(1 << 2);
        if (quad2->child[i] != SOLID_QUAD)
            flag &= ~(1 << 3);
    }
    if (flag & (3 << 0)) {
        free_quad(engine, quad1, size_order);
        *dst1 = flag & (1 << 1) ? SOLID_QUAD : EMPTY_QUAD;
    }
    if (flag & (3 << 2)) {
        free_quad(engine, quad2, size_order);
        *dst2 = flag & (1 << 3) ? SOLID_QUAD : EMPTY_QUAD;
    }
    return 1;
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

    int error = 0;
    int min1 = 2, max1 = -1, min2 = n, max2 = -1;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < n / 2; ++j) {
            if (!expand_quad(engine, &grid[i * n + 2 * j + 0], &grid[i * n + 2 * j + 1],
                             ptr[j + 0], ptr[j + 1], ptr[j + 2], size_order, dir))
                error = 1;
            else {
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
        }
        assert(grid[i * n + 0] == tree->outside && grid[i * n + 7] == tree->outside);
        ptr += 4;
    }

    if (!error && create_tree_from_grid(engine, tree,
                                        base1, base2, size_order, dir,
                                        min1, max1, min2, max2, grid, n))
        return 1;
    clear_quad_grid(engine, size_order, min1, max1, min2, max2, grid, n);
    return 0;
}


static const Quad *filter_quad(const TileEngine *engine,
                               const Quad *side1, const Quad *src, const Quad *side2,
                               int size_order, int dir,
                               const FilterTileFunc tile_func[2],
                               const FilterSolidTileFunc solid_tile_func[2],
                               void *param)
{
    assert(size_order >= engine->tile_order);
    assert(side1 != INVALID_QUAD && side2 != INVALID_QUAD);
    assert(src != INVALID_QUAD);
    assert(dir == 1 || dir == 2);

    if (size_order == engine->tile_order) {
        const int16_t *tile[3] = {
            (const int16_t *)side1, (const int16_t *)src, (const int16_t *)side2
        };
        if (!side1 || side1 == SOLID_QUAD)
            tile[0] = engine->solid_tile[side1 ? 1 : 0];
        if (!side2 || side2 == SOLID_QUAD)
            tile[2] = engine->solid_tile[side2 ? 1 : 0];
        if (!src || src == SOLID_QUAD) {
            if (side1 == src && side2 == src)
                return src;

            int16_t *buf = alloc_tile(engine);
            if (!buf)
                return INVALID_QUAD;
            if (solid_tile_func[dir - 1](buf, tile[0], src != EMPTY_QUAD, tile[2], param))
                return (const Quad *)buf;
            free_tile(engine, buf);
            return src;
        }

        int16_t *buf = alloc_tile(engine);
        if (!buf)
            return INVALID_QUAD;
        tile_func[dir - 1](buf, tile[0], tile[1], tile[2], param);
        return (const Quad *)buf;
    }

    const Quad *empty = INVALID_QUAD;
    const Quad *input[3] = { side1, src, side2 };
    if (!side1 || side1 == SOLID_QUAD)
        input[0] = &solid_sub_tile[side1 ? 1 : 0];
    if (!side2 || side2 == SOLID_QUAD)
        input[2] = &solid_sub_tile[side2 ? 1 : 0];
    if (!src || src == SOLID_QUAD) {
        if (side1 == src && side2 == src)
            return src;
        empty = src;
        input[1] = &solid_sub_tile[src ? 1 : 0];
    }

    Quad *quad = alloc_quad(engine, INVALID_QUAD);
    if (!quad)
        return INVALID_QUAD;

    int k[2][2] = { { 0, dir }, { dir ^ 3, 3 } };
    for (int i = 0; i < 2; ++i) {
        const Quad *res1 = filter_quad(engine,
                                       input[0]->child[k[i][1]], input[1]->child[k[i][0]], input[1]->child[k[i][1]],
                                       size_order - 1, dir, tile_func, solid_tile_func, param);
        quad->child[k[i][0]] = res1;
        const Quad *res2 = filter_quad(engine,
                                       input[1]->child[k[i][0]], input[1]->child[k[i][1]], input[2]->child[k[i][0]],
                                       size_order - 1, dir, tile_func, solid_tile_func, param);
        quad->child[k[i][1]] = res2;
        if (res1 == INVALID_QUAD || res2 == INVALID_QUAD) {
            free_quad(engine, quad, size_order);
            return INVALID_QUAD;
        }
        if (res1 != empty || res2 != empty)
            empty = INVALID_QUAD;
    }
    if (empty != INVALID_QUAD) {
        free_quad(engine, quad, size_order);
        return empty;
    }
    return quad;
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

    int error = 0;
    int min1 = 2, max1 = -1, min2 = n, max2 = -1;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < n; ++j) {
            grid[i * n + j] = filter_quad(engine, ptr[j + 0], ptr[j + 1], ptr[j + 2],
                                    size_order, dir, tile_func, solid_tile_func, param);
            if (grid[i * n + j] == INVALID_QUAD)
                error = 1;
            else if (grid[i * n + j] != tree->outside) {
                min1 = FFMIN(min1, i);
                max1 = FFMAX(max1, i);
                min2 = FFMIN(min2, j);
                max2 = FFMAX(max2, j);
            }
        }
        ptr += 4;
    }

    if (!error && create_tree_from_grid(engine, tree,
                                        base1, base2, size_order, dir,
                                        min1, max1, min2, max2, grid, n))
        return 1;
    clear_quad_grid(engine, size_order, min1, max1, min2, max2, grid, n);
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

    int16_t coeff[5];
    int rest = 0x10000;
    for (int i = 1; i <= 4; ++i) {
        coeff[i] = (int)(0x8000 * mu[i - 1] + 0.5);
        rest -= 2 * coeff[i];
    }
    coeff[0] = rest;

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


static const Quad *shift_quad(const TileEngine *engine,
                              const Quad *src0, const Quad *src1,
                              const Quad *src2, const Quad *src3,
                              int size_order, int dx, int dy)
{
    assert(size_order >= engine->tile_order);
    assert(src0 != INVALID_QUAD && src1 != INVALID_QUAD);
    assert(src2 != INVALID_QUAD && src3 != INVALID_QUAD);
    assert(dx >= 0 && dx < (64 << size_order));
    assert(dy >= 0 && dy < (64 << size_order));

    if (!dx && !dy)
        return copy_quad(engine, src0, size_order);

    const Quad *src[] = { src0, src1, src2, src3 };
    if (size_order == engine->tile_order) {
        int flag = 3;
        const int16_t *tile[4];
        for (int i = 0; i < 4; ++i) {
            if (!src[i] || src[i] == SOLID_QUAD) {
                int index = src[i] ? 1 : 0;
                tile[i] = engine->solid_tile[index];
                flag &= 1 << index;
            } else {
                tile[i] = (const int16_t *)src[i];
                flag = 0;
            }
        }
        if (flag)
            return src0;

        int16_t *buf = alloc_tile(engine);
        if (!buf)
            return INVALID_QUAD;
        int res = engine->shift(buf, tile[0], tile[1], tile[2], tile[3], dx, dy);
        if (!res)
            return (const Quad *)buf;
        free_tile(engine, buf);
        return res < 0 ? EMPTY_QUAD : SOLID_QUAD;
    }

    int flag = 3;
    for (int i = 0; i < 4; ++i) {
        if (!src[i] || src[i] == SOLID_QUAD) {
            int index = src[i] ? 1 : 0;
            src[i] = &solid_sub_tile[index];
            flag &= 1 << index;
        } else
            flag = 0;
    }
    if (flag)
        return src0;

    Quad *quad = alloc_quad(engine, INVALID_QUAD);
    if (!quad)
        return INVALID_QUAD;

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

    flag = 3;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            const Quad *res = shift_quad(engine,
                                         input[i + 0][j + 0], input[i + 0][j + 1],
                                         input[i + 1][j + 0], input[i + 1][j + 1],
                                         size_order - 1, dx, dy);
            quad->child[2 * i + j] = res;
            if (res == INVALID_QUAD) {
                free_quad(engine, quad, size_order);
                return INVALID_QUAD;
            }
            if (res == EMPTY_QUAD)
                flag &= 1;
            else if (res == SOLID_QUAD)
                flag &= 2;
            else
                flag = 0;
        }
    if (!flag)
        return quad;
    free_quad(engine, quad, size_order);
    return flag == 1 ? EMPTY_QUAD : SOLID_QUAD;
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

    int error = 0;
    int min1 = n, max1 = -1, min2 = n, max2 = -1;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            grid[i * n + j] = shift_quad(engine,
                                         ptr[j + 0], ptr[j + 1], ptr[j + 3], ptr[j + 4],
                                         size_order, dx, dy);
            if (grid[i * n + j] == INVALID_QUAD)
                error = 1;
            else if (grid[i * n + j] != tree->outside) {
                min1 = FFMIN(min1, i);
                max1 = FFMAX(max1, i);
                min2 = FFMIN(min2, j);
                max2 = FFMAX(max2, j);
            }
        }
        ptr += 3;
    }

    if (!error && create_tree_from_grid(engine, tree,
                                        base1, base2, size_order, 1,
                                        min1, max1, min2, max2, grid, n))
        return 1;
    clear_quad_grid(engine, size_order, min1, max1, min2, max2, grid, n);
    return 0;
}
