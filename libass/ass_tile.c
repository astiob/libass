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

void free_tile_tree(const TileEngine *engine, TileTree *tree)
{
    if (tree->size_order >= 0) {
        assert(tree->size_order > engine->tile_order);
        for (int i = 0; i < 4; ++i)
            free_quad(engine, tree->quad.child[i], tree->size_order - 1);
    }
    free(tree);
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

static inline int tree_cross(const TileEngine *engine, TileTree *dst,
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

static inline int tree_union(const TileEngine *engine, TileTree *dst,
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

static int crop_tree(const TileEngine *engine, TileTree *dst,
                     const TileTree *src, int op_flags)
{
    TileTree old = *dst;
    if (src->outside == trivial_quad(op_flags & 2)) {
        if (dst->outside != trivial_quad(op_flags & 1)) {
            dst->x = src->x;
            dst->y = src->y;
            dst->size_order = src->size_order;
            dst->outside = trivial_quad(op_flags & 1);
        } else if (!tree_cross(engine, dst, dst, src)) {
            if (old.size_order < 0)
                return 1;
            for (int i = 0; i < 4; ++i) {
                free_quad(engine, old.quad.child[i], old.size_order - 1);
                dst->quad.child[i] = dst->outside;
            }
            return 1;
        }
    } else {
        if (dst->outside == trivial_quad(op_flags & 1))
            return 1;
        else if (!tree_union(engine, dst, dst, src))
            return 1;
    }
    if (old.size_order < 0 || (dst->x == old.x && dst->y == old.y && dst->size_order == old.size_order))
        return 1;

    for (int i = 0; i < 4; ++i)
        dst->quad.child[i] = dst->outside;

    int res = 1;
    if (old.size_order <= dst->size_order) {
        for (int i = 0; i < 4; ++i) {
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
            old.quad.child[i] = EMPTY_QUAD;
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            int delta_x = dst->x - old.x + (((i >> 0) & 1) << (dst->size_order - 1));
            int delta_y = dst->y - old.y + (((i >> 1) & 1) << (dst->size_order - 1));
            dst->quad.child[i] = (delta_x | delta_y) >> old.size_order ? dst->outside :
                extract_sub_quad(engine, &old.quad, dst->size_order - 1, old.size_order, delta_x, delta_y);
        }
    }
    for (int i = 0; i < 4; ++i)
        free_quad(engine, old.quad.child[i], old.size_order - 1);
    return res;
}


static const Quad *combine_quad(const TileEngine *engine,
                                const Quad *src1, const Quad *src2, int size_order,
                                TileCombineFunc tile_func, int op_flags)
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
                                      TileCombineFunc tile_func, int op_flags)
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
                                      TileCombineFunc tile_func, int op_flags)
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
    static int op_flags[] = {
        0 << 0 | 0 << 1,  // COMBINE_MUL
        1 << 0 | 1 << 1,  // COMBINE_ADD
        0 << 0 | 1 << 1,  // COMBINE_SUB
    };

    TileCombineFunc tile_func = engine->tile_combine[op];
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
    return 1;  // XXX: shrink tree
}
