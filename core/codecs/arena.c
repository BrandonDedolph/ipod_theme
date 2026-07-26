/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/arena.c — static bump arena (see arena.h).
 *
 * Layout per allocation: an 8-byte header holding the payload size, then the
 * 8-byte-aligned payload. realloc() finds the old size in the header to copy
 * the right amount; free() is a no-op (the arena frees as a whole in reset()).
 */

#include "arena.h"

#include <stdint.h>

#include "../lib/mem.h"

#define ARENA_ALIGN 8u
#define ARENA_HDR   8u          /* holds the payload size (fits size_t)      */

static size_t align_up(size_t v, size_t a)
{
    return (v + (a - 1u)) & ~(a - 1u);
}

void decoder_arena_init(decoder_arena_t *a, void *buf, size_t cap)
{
    a->base       = (unsigned char *)buf;
    a->cap        = cap;
    a->used       = 0;
    a->high_water = 0;
    a->last       = 0;
    a->oom        = 0;
}

void decoder_arena_reset(decoder_arena_t *a)
{
    a->used = 0;
    a->last = 0;
    /* high_water and oom persist across tracks intentionally. */
}

static void *arena_alloc(void *userdata, size_t bytes)
{
    decoder_arena_t *a = (decoder_arena_t *)userdata;

    size_t hdr  = align_up(a->used, ARENA_ALIGN);
    size_t data = hdr + ARENA_HDR;
    size_t end  = data + align_up(bytes, ARENA_ALIGN);
    if (end > a->cap) {
        a->oom = 1;
        return NULL;
    }
    *(size_t *)(a->base + hdr) = bytes;      /* header records payload size  */
    a->used = end;
    a->last = data;
    if (a->used > a->high_water) {
        a->high_water = a->used;
    }
    return a->base + data;
}

static void *arena_realloc(void *userdata, void *ptr, size_t bytes)
{
    decoder_arena_t *a = (decoder_arena_t *)userdata;

    if (ptr == NULL) {
        return arena_alloc(userdata, bytes);
    }
    /* Grow (or shrink) IN PLACE when this is the newest block — the case the
     * codecs actually hit, since they grow one input buffer repeatedly. The
     * old alloc-new-and-copy path abandoned the previous copy every step, so
     * a few 64 KB growths exhausted a 128 KB arena. Nothing lives above
     * a->used, so the block can simply extend into it. */
    if (a->last != 0 && (unsigned char *)ptr == a->base + a->last) {
        size_t end = a->last + align_up(bytes, ARENA_ALIGN);
        if (end > a->cap) {
            a->oom = 1;                      /* a fresh block wouldn't fit either */
            return NULL;
        }
        *(size_t *)(a->base + a->last - ARENA_HDR) = bytes;
        a->used = end;
        if (a->used > a->high_water) {
            a->high_water = a->used;
        }
        return ptr;
    }

    size_t old = *(size_t *)((unsigned char *)ptr - ARENA_HDR);
    void  *np  = arena_alloc(userdata, bytes);
    if (np != NULL) {
        memcpy(np, ptr, old < bytes ? old : bytes);
    }
    return np;   /* old block is abandoned in the arena — reclaimed at reset */
}

static void arena_free(void *userdata, void *ptr)
{
    (void)userdata;
    (void)ptr;   /* no-op: the arena frees as a whole in decoder_arena_reset */
}

decoder_alloc_t decoder_arena_allocator(decoder_arena_t *a)
{
    decoder_alloc_t cb;
    cb.alloc    = arena_alloc;
    cb.realloc  = arena_realloc;
    cb.free     = arena_free;
    cb.userdata = a;
    return cb;
}
