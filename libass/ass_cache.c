/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2011 Grigori Goronzy <greg@chown.ath.cx>
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

#include <inttypes.h>
#include <ft2build.h>
#include FT_OUTLINE_H
#include <assert.h>

#include "ass_utils.h"
#include "ass_font.h"
#include "ass_cache.h"

// type-specific functions
// create hash/compare functions for bitmap, outline and composite cache
#define CREATE_HASH_FUNCTIONS
#include "ass_cache_template.h"
#define CREATE_COMPARISON_FUNCTIONS
#include "ass_cache_template.h"

// font cache
static unsigned font_hash(void *buf, size_t len)
{
    ASS_FontDesc *desc = buf;
    unsigned hval;
    hval = fnv_32a_str(desc->family, FNV1_32A_INIT);
    hval = fnv_32a_buf(&desc->bold, sizeof(desc->bold), hval);
    hval = fnv_32a_buf(&desc->italic, sizeof(desc->italic), hval);
    hval = fnv_32a_buf(&desc->treat_family_as_pattern,
            sizeof(desc->treat_family_as_pattern), hval);
    hval = fnv_32a_buf(&desc->vertical, sizeof(desc->vertical), hval);
    return hval;
}

static unsigned font_compare(void *key1, void *key2, size_t key_size)
{
    ASS_FontDesc *a = key1;
    ASS_FontDesc *b = key2;
    if (strcmp(a->family, b->family) != 0)
        return 0;
    if (a->bold != b->bold)
        return 0;
    if (a->italic != b->italic)
        return 0;
    if (a->treat_family_as_pattern != b->treat_family_as_pattern)
        return 0;
    if (a->vertical != b->vertical)
        return 0;
    return 1;
}

static void font_destruct(void *key, void *value)
{
    ass_font_clear(value);
}

// bitmap cache
static void bitmap_destruct(void *key, void *value)
{
    BitmapHashValue *v = value;
    BitmapHashKey *k = key;
    if (v->bm)
        free_tile_tree(v->engine, v->bm);
    if (v->bm_o)
        free_tile_tree(v->engine, v->bm_o);
    if (v->bm_s)
        free_tile_tree(v->engine, v->bm_s);
    if (k->type == BITMAP_CLIP)
        free(k->u.clip.text);
    else
        ass_cache_dec_ref(k->u.outline.outline);
}

static size_t bitmap_size(void *value, size_t value_size)
{
    size_t res = 0;
    BitmapHashValue *v = value;
    if (v->bm)
        res += calc_tree_size(v->engine, v->bm);
    if (v->bm_o)
        res += calc_tree_size(v->engine, v->bm_o);
    if (v->bm_s)
        res += calc_tree_size(v->engine, v->bm_s);
    return res;
}

static unsigned bitmap_hash(void *key, size_t key_size)
{
    BitmapHashKey *k = key;
    switch (k->type) {
        case BITMAP_OUTLINE: return outline_bitmap_hash(&k->u, key_size);
        case BITMAP_CLIP: return clip_bitmap_hash(&k->u, key_size);
        default: return 0;
    }
}

static unsigned bitmap_compare(void *a, void *b, size_t key_size)
{
    BitmapHashKey *ak = a;
    BitmapHashKey *bk = b;
    if (ak->type != bk->type) return 0;
    switch (ak->type) {
        case BITMAP_OUTLINE: return outline_bitmap_compare(&ak->u, &bk->u, key_size);
        case BITMAP_CLIP: return clip_bitmap_compare(&ak->u, &bk->u, key_size);
        default: return 0;
    }
}

// composite cache

static void composite_destruct(void *key, void *value)
{
    CompositeHashValue *v = value;
    CompositeHashKey *k = key;
    if (v->bm)
        free_tile_tree(v->engine, v->bm);
    if (v->bm_o)
        free_tile_tree(v->engine, v->bm_o);
    if (v->bm_s)
        free_tile_tree(v->engine, v->bm_s);
    free(k->str);
}

static size_t composite_size(void *value, size_t value_size)
{
    return bitmap_size(value, value_size);
}

// final cache

static void final_destruct(void *key, void *value)
{
    FinalHashValue *v = value;
    FinalHashKey *k = key;
    free_image_list(v->bm);
    free_image_list(v->bm_o);
    free_image_list(v->bm_s);
    ass_cache_dec_ref(k->composite);
    free(k->clip_drawing_text);
}

static size_t final_size(void *value, size_t value_size)
{
    size_t res = 0;
    FinalHashValue *v = value;
    res += image_list_size(v->bm);
    res += image_list_size(v->bm_o);
    res += image_list_size(v->bm_s);
    return res;
}

// outline cache

static unsigned outline_hash(void *key, size_t key_size)
{
    OutlineHashKey *k = key;
    switch (k->type) {
        case OUTLINE_GLYPH: return glyph_hash(&k->u, key_size);
        case OUTLINE_DRAWING: return drawing_hash(&k->u, key_size);
        default: return 0;
    }
}

static unsigned outline_compare(void *a, void *b, size_t key_size)
{
    OutlineHashKey *ak = a;
    OutlineHashKey *bk = b;
    if (ak->type != bk->type) return 0;
    switch (ak->type) {
        case OUTLINE_GLYPH: return glyph_compare(&ak->u, &bk->u, key_size);
        case OUTLINE_DRAWING: return drawing_compare(&ak->u, &bk->u, key_size);
        default: return 0;
    }
}

static void outline_destruct(void *key, void *value)
{
    OutlineHashValue *v = value;
    OutlineHashKey *k = key;
    if (v->outline)
        outline_free(v->lib, v->outline);
    if (v->border)
        outline_free(v->lib, v->border);
    if (k->type == OUTLINE_DRAWING)
        free(k->u.drawing.text);
    else
        ass_cache_dec_ref(k->u.glyph.font);
}


// glyph metric cache

static void glyph_metric_destruct(void *key, void *value)
{
    GlyphMetricsHashKey *k = key;
    ass_cache_dec_ref(k->font);
}



// Cache data
typedef struct cache_item {
#ifndef NDEBUG
    size_t check_flag;
#endif
    struct cache *cache;
    struct cache_item *next, **prev;
    struct cache_item *queue_next, **queue_prev;
    size_t size, ref_count;
} CacheItem;

struct cache {
    unsigned buckets;
    CacheItem **map;
    CacheItem *queue_first, **queue_last;

    HashFunction hash_func;
    ItemSize size_func;
    HashCompare compare_func;
    CacheItemDestructor destruct_func;
    size_t key_size;
    size_t value_size;

    size_t cache_size;
    unsigned hits;
    unsigned misses;
    unsigned items;
};

// Hash for a simple (single value or array) type
static unsigned hash_simple(void *key, size_t key_size)
{
    return fnv_32a_buf(key, key_size, FNV1_32A_INIT);
}

// Comparison of a simple type
static unsigned compare_simple(void *a, void *b, size_t key_size)
{
    return memcmp(a, b, key_size) == 0;
}

// Default destructor
static void destruct_simple(void *key, void *value)
{
}


// Create a cache with type-specific hash/compare/destruct/size functions
Cache *ass_cache_create(HashFunction hash_func, HashCompare compare_func,
                        CacheItemDestructor destruct_func, ItemSize size_func,
                        size_t key_size, size_t value_size)
{
    Cache *cache = calloc(1, sizeof(*cache));
    cache->buckets = 0xFFFF;
    cache->hash_func = hash_simple;
    cache->compare_func = compare_simple;
    cache->destruct_func = destruct_simple;
    cache->size_func = size_func;
    if (hash_func)
        cache->hash_func = hash_func;
    if (compare_func)
        cache->compare_func = compare_func;
    if (destruct_func)
        cache->destruct_func = destruct_func;
    cache->key_size = key_size;
    cache->value_size = value_size;
    cache->map = calloc(cache->buckets, sizeof(CacheItem *));
    cache->queue_last = &cache->queue_first;

    return cache;
}

int ass_cache_get(Cache *cache, void *key, void *value_ptr)
{
    char **value = (char **)value_ptr;
    ptrdiff_t key_offs = sizeof(CacheItem) + cache->value_size;
    unsigned bucket = cache->hash_func(key, cache->key_size) % cache->buckets;
    CacheItem *item = cache->map[bucket];
    while (item) {
        if (cache->compare_func(key, (char *)item + key_offs, cache->key_size)) {
#ifndef NDEBUG
            assert(item->check_flag == 22222222);
#endif
            if (!item->queue_prev || item->queue_next) {
                if (item->queue_prev) {
                    item->queue_next->queue_prev = item->queue_prev;
                    *item->queue_prev = item->queue_next;
                }
                *cache->queue_last = item;
                item->queue_prev = cache->queue_last;
                cache->queue_last = &item->queue_next;
                item->queue_next = NULL;
            }
            cache->hits++;
            *value = (char *)item + sizeof(CacheItem);
            return 1;
        }
        item = item->next;
    }
    cache->misses++;

    item = malloc(sizeof(CacheItem) + cache->value_size + cache->key_size);
    if (!item) {
        *value = NULL;
        return 0;
    }
#ifndef NDEBUG
    item->check_flag = 11111111;
#endif
    item->cache = cache;
    *value = (char *)item + sizeof(CacheItem);
    memcpy((char *)item + key_offs, key, cache->key_size);

    CacheItem **bucketptr = &cache->map[bucket];
    if (*bucketptr)
        (*bucketptr)->prev = &item->next;
    item->prev = bucketptr;
    item->next = *bucketptr;
    *bucketptr = item;

    *cache->queue_last = item;
    item->queue_prev = cache->queue_last;
    cache->queue_last = &item->queue_next;
    item->queue_next = NULL;

    item->ref_count = 0;
    return 0;
}

void *ass_cache_get_key(void *value)
{
    CacheItem *item = (CacheItem *)((char *)value - sizeof(CacheItem));
#ifndef NDEBUG
    assert(item->check_flag == 11111111);
#endif
    return (char *)value + item->cache->value_size;
}

void ass_cache_commit(void *value)
{
    CacheItem *item = (CacheItem *)((char *)value - sizeof(CacheItem));
#ifndef NDEBUG
    assert(item->check_flag == 11111111);
    item->check_flag = 22222222;
#endif
    Cache *cache = item->cache;

    ++cache->items;
    if (cache->size_func)
        item->size = cache->size_func(value, cache->value_size);
    else
        item->size = 1;
    cache->cache_size += item->size;
}

void ass_cache_cancel(void *value)
{
    CacheItem *item = (CacheItem *)((char *)value - sizeof(CacheItem));
#ifndef NDEBUG
    assert(item->check_flag == 11111111);
#endif

    if (item->queue_next)
        item->queue_next->queue_prev = item->queue_prev;
    *item->queue_prev = item->queue_next;

    if (item->next)
        item->next->prev = item->prev;
    *item->prev = item->next;
    free(item);
}

void ass_cache_inc_ref(void *value)
{
    CacheItem *item = (CacheItem *)((char *)value - sizeof(CacheItem));
#ifndef NDEBUG
    assert(item->check_flag == 22222222);
#endif
    ++item->ref_count;
}

void ass_cache_dec_ref(void *value)
{
    CacheItem *item = (CacheItem *)((char *)value - sizeof(CacheItem));
#ifndef NDEBUG
    assert(item->check_flag == 22222222);
#endif
    assert(item->ref_count);

    --item->ref_count;
    if (item->ref_count || item->queue_prev)
        return;

    if (item->next)
        item->next->prev = item->prev;
    *item->prev = item->next;

    --item->cache->items;
    item->cache->cache_size -= item->size;
    item->cache->destruct_func((char *)value + item->cache->value_size, value);
    free(item);
}

void ass_cache_cut(Cache *cache, size_t max_size)
{
    if (cache->cache_size <= max_size)
        return;

    do {
        CacheItem *item = cache->queue_first;
        if (!item)
            break;
#ifndef NDEBUG
        assert(item->check_flag == 22222222);
#endif
        assert(item->cache == cache);

        cache->queue_first = item->queue_next;
        if (item->ref_count) {
            item->queue_prev = NULL;
            continue;
        }

        if (item->next)
            item->next->prev = item->prev;
        *item->prev = item->next;

        --cache->items;
        cache->cache_size -= item->size;
        char *value = (char *)item + sizeof(CacheItem);
        cache->destruct_func(value + cache->value_size, value);
        free(item);
    } while (cache->cache_size > max_size);
    if (cache->queue_first)
        cache->queue_first->queue_prev = &cache->queue_first;
    else
        cache->queue_last = &cache->queue_first;
}

void ass_cache_stats(Cache *cache, size_t *size, unsigned *hits,
                     unsigned *misses, unsigned *count)
{
    if (size)
        *size = cache->cache_size;
    if (hits)
        *hits = cache->hits;
    if (misses)
        *misses = cache->misses;
    if (count)
        *count = cache->items;
}

void ass_cache_empty(Cache *cache)
{
    for (int i = 0; i < cache->buckets; ++i) {
        CacheItem *item = cache->map[i];
        while (item) {
#ifndef NDEBUG
            assert(item->check_flag == 22222222);
#endif
            CacheItem *next = item->next;
            char *value = (char *)item + sizeof(CacheItem);
            cache->destruct_func(value + cache->value_size, value);
            free(item);
            item = next;
        }
        cache->map[i] = NULL;
    }

    cache->queue_first = NULL;
    cache->queue_last = &cache->queue_first;
    cache->items = cache->hits = cache->misses = cache->cache_size = 0;
}

void ass_cache_done(Cache *cache)
{
    ass_cache_empty(cache);
    free(cache->map);
    free(cache);
}

// Type-specific creation function
Cache *ass_font_cache_create(void)
{
    return ass_cache_create(font_hash, font_compare, font_destruct,
            (ItemSize)NULL, sizeof(ASS_FontDesc), sizeof(ASS_Font));
}

Cache *ass_outline_cache_create(void)
{
    return ass_cache_create(outline_hash, outline_compare, outline_destruct,
            (ItemSize)NULL, sizeof(OutlineHashKey), sizeof(OutlineHashValue));
}

Cache *ass_glyph_metrics_cache_create(void)
{
    return ass_cache_create(glyph_metrics_hash, glyph_metrics_compare, glyph_metric_destruct,
            (ItemSize)NULL, sizeof(GlyphMetricsHashKey),
            sizeof(GlyphMetricsHashValue));
}

Cache *ass_bitmap_cache_create(void)
{
    return ass_cache_create(bitmap_hash, bitmap_compare, bitmap_destruct,
            bitmap_size, sizeof(BitmapHashKey), sizeof(BitmapHashValue));
}

Cache *ass_composite_cache_create(void)
{
    return ass_cache_create(composite_hash, composite_compare,
            composite_destruct, composite_size, sizeof(CompositeHashKey),
            sizeof(CompositeHashValue));
}

Cache *ass_final_cache_create(void)
{
    return ass_cache_create(final_hash, final_compare,
            final_destruct, final_size, sizeof(FinalHashKey),
            sizeof(FinalHashValue));
}
