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
    struct tree_level *child[4];  // UL, UR, DL, DR
} Quad;

#define SOLID_TILE ((Quad *)1)

typedef struct {
    int x, y;  // aligned to 2^(size_order - 1)
    int size_order;  // larger then tile_order
    Quad *outside;  // NULL or SOLID_TILE
    Quad quad;
} TileTree;


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
// == 0 - dominant tile
// != 0 - generic tile
typedef int (*TileCombineFunc)(int16_t *dst,
                               const int16_t *src1, const int16_t *src2);


enum {
    COMBINE_MUL,
    COMBINE_ADD,
    COMBINE_SUB
};

typedef struct {
    int tile_order;  // log2(tile_size)
    int outline_error;  // acceptable error (in 1/64 pixel units)
    const int16_t *solid_tile[2];
    FinalizeSolidFunc finalize_solid;
    FinalizeGenericTileFunc finalize_generic;
    FillHalfplaneTileFunc fill_halfplane;
    FillGenericTileFunc fill_generic;
    TileCombineFunc tile_combine[3];
} TileEngine;


void *alloc_tile(TileEngine *engine);
void *copy_tile(TileEngine *engine, const void *tile);
void free_tile(TileEngine *engine, void *tile);
Quad *alloc_quad(TileEngine *engine, Quad *fill);
int copy_quad(TileEngine *engine, Quad **dst, const Quad *src, int size_order);
void free_empty_quad(TileEngine *engine, Quad *quad);
void free_quad(TileEngine *engine, Quad *quad, int size_order);
TileTree *alloc_tile_tree(TileEngine *engine, Quad *fill);
TileTree *copy_tile_tree(TileEngine *engine, const TileTree *src);
void free_tile_tree(TileEngine *engine, TileTree *tree);


void finalize_quad(TileEngine *engine, uint8_t *buf, ptrdiff_t stride,
                   const Quad *quad, int size_order);

void calc_tree_bounds(TileEngine *engine, TileTree *dst,
                      int x_min, int y_min, int x_max, int y_max);
int combine_tile_tree(TileEngine *engine, TileTree *dst, const TileTree *src, int op);


#endif                          /* LIBASS_TILE_H */
