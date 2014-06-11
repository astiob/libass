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

#ifndef LIBASS_TILE_H
#define LIBASS_TILE_H

#include <stddef.h>
#include <stdint.h>


// maximal tile value = 2^14

typedef struct tree_level {
    // pointers to int16_t tile[tile_size^2] at last level
    const struct tree_level *child[4];  // UL, UR, DL, DR
} Quad;

#define EMPTY_QUAD   ((const Quad *)0)
#define SOLID_QUAD   ((const Quad *)1)

typedef struct {
    int x, y;  // aligned to 2^(size_order - 1)
    int size_order;  // larger then tile_order
    const Quad *outside;  // NULL or SOLID_QUAD
    Quad quad;
} TileTree;

enum {
    FLAG_EMPTY = 1 << 0,
    FLAG_SOLID = 1 << 1,
    FLAG_SRC1  = 1 << 2,
    FLAG_SRC2  = 1 << 3,
    FLAG_VALID = 1 << 4,
    FLAG_ALL = FLAG_EMPTY | FLAG_SOLID | FLAG_VALID,
    FLAG_ALL_COMBINE = FLAG_ALL | FLAG_SRC1 | FLAG_SRC2
};


struct segment;

typedef void (*FinalizeSolidFunc)(uint8_t *buf, ptrdiff_t stride,
                                  int size_order, int set);
typedef void (*FinalizeGenericTileFunc)(uint8_t *buf, ptrdiff_t stride,
                                        const int16_t *src);
typedef void (*FillHalfplaneTileFunc)(int16_t *buf,
                                      int32_t a, int32_t b, int64_t c, int32_t scale);
typedef void (*FillGenericTileFunc)(int16_t *buf,
                                    const struct segment *line, size_t n_lines,
                                    int winding);
// return value:
// == 0 - generic tile
// != 0 - trivial tile
typedef int (*CombineTileFunc)(int16_t *dst,
                               const int16_t *src1, const int16_t *src2);
typedef void (*ShrinkTileFunc)(int16_t *dst,
                               const int16_t *side1, const int16_t *src1,
                               const int16_t *src2, const int16_t *side2);
typedef int (*ShrinkSolidTileFunc)(int16_t *dst,
                                   const int16_t *side1, int set, const int16_t *side2);
typedef int (*ExpandTileFunc)(int16_t *dst1, int16_t *dst2,
                              const int16_t *side1, const int16_t *src, const int16_t *side2);
typedef int (*ExpandSolidTileFunc)(int16_t *dst, const int16_t *side, int set);
typedef void (*FilterTileFunc)(int16_t *dst,
                               const int16_t *side1, const int16_t *src, const int16_t *side2,
                               void *param);
typedef int (*FilterSolidTileFunc)(int16_t *dst,
                                   const int16_t *side1, int set, const int16_t *side2,
                                   void *param);
// return value:
// = 0 - generic tile
// < 0 - empty tile
// > 0 - solid tile
typedef int (*ShiftTileFunc)(int16_t *dst,
                             const int16_t *src0, const int16_t *src1,
                             const int16_t *src2, const int16_t *src3,
                             int dx, int dy);


enum {
    COMBINE_MUL,
    COMBINE_ADD,
    COMBINE_SUB
};

typedef struct {
    int tile_order;  // log2(tile_size)
    int tile_alignment;
    const int16_t *solid_tile[2];
    FinalizeSolidFunc finalize_solid;
    FinalizeGenericTileFunc finalize_generic;
    FillHalfplaneTileFunc fill_halfplane;
    FillGenericTileFunc fill_generic;
    CombineTileFunc combine[3];
    ShrinkTileFunc shrink[2];
    ShrinkSolidTileFunc shrink_solid[2];
    ExpandTileFunc expand[2];
    ExpandSolidTileFunc expand_solid[2][2];
    FilterTileFunc pre_blur[3][2];
    FilterSolidTileFunc pre_blur_solid[3][2];
    FilterTileFunc main_blur[3][2];
    FilterSolidTileFunc main_blur_solid[3][2];
    ShiftTileFunc shift;
} TileEngine;


void prepare_solid_tiles(void);  // XXX: do static filling
extern const TileEngine ass_engine_tile16_c;
extern const TileEngine ass_engine_tile32_c;
#if (defined(__i386__) || defined(__x86_64__)) && CONFIG_ASM
extern const TileEngine ass_engine_tile16_sse2;
extern const TileEngine ass_engine_tile32_sse2;
extern const TileEngine ass_engine_tile16_avx2;
extern const TileEngine ass_engine_tile32_avx2;
#endif


static inline const Quad *trivial_quad(int solid)
{
    return solid ? SOLID_QUAD : EMPTY_QUAD;
}

static inline int trivial_quad_flag(int solid)
{
    return solid ? FLAG_SOLID | FLAG_VALID : FLAG_EMPTY | FLAG_VALID;
}

static inline int set_trivial_quad(const Quad **dst, int solid)
{
    *dst = trivial_quad(solid);
    return trivial_quad_flag(solid);
}

static inline int is_trivial_quad(const Quad *quad)
{
    return !quad || quad == SOLID_QUAD;
}


void *alloc_tile(const TileEngine *engine);
const void *copy_tile(const TileEngine *engine, const void *tile);
void free_tile(const TileEngine *engine, const void *tile);
Quad *alloc_quad(const TileEngine *engine, const Quad *fill);
int copy_quad(const TileEngine *engine, const Quad **dst, const Quad *quad, int size_order);
void free_quad(const TileEngine *engine, const Quad *quad, int size_order);
TileTree *alloc_tile_tree(const TileEngine *engine, const Quad *fill);
TileTree *copy_tile_tree(const TileEngine *engine, const TileTree *src);
void free_tile_tree(const TileEngine *engine, TileTree *tree);
size_t calc_tree_size(const TileEngine *engine, const TileTree *tree);
int is_valid_tree(const TileEngine *engine, const TileTree *tree);


void finalize_quad(const TileEngine *engine, uint8_t *buf, ptrdiff_t stride,
                   const Quad *quad, int size_order);
TileTree *create_rectangle(const TileEngine *engine,
                           int32_t x_min, int32_t y_min, int32_t x_max, int32_t y_max,
                           int inverse);
void calc_tree_bounds(const TileEngine *engine, TileTree *dst,
                      int x_min, int y_min, int x_max, int y_max);
int combine_tile_tree(const TileEngine *engine, TileTree *dst, const TileTree *src, int op);
int blur_tile_tree(const TileEngine *engine, TileTree *tree, double r2);
int shift_tile_tree(const TileEngine *engine, TileTree *tree, int dx, int dy);


#endif                          /* LIBASS_TILE_H */
