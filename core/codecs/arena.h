/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/codecs/arena.h — static bump arena backing decoder_alloc_t.
 *
 * The freestanding build has no heap, but dr_flac (and other codecs) want an
 * allocator at open() time. A decoder allocates a handful of buffers once
 * when a track opens and frees them all at close — a perfect fit for a bump
 * allocator over a caller-provided static buffer: alloc() bumps a pointer,
 * free() is a no-op, and the whole arena is reclaimed by reset() between
 * tracks. realloc() is supported (alloc-new + copy) since dr_flac uses it.
 *
 * high_water records the peak bytes in use so the arena can be sized from a
 * real decode rather than guessed. On exhaustion alloc() returns NULL (which
 * dr_flac treats as an open failure) and sets `oom`.
 *
 * realloc() has a GROW-IN-PLACE fast path for the arena's most recent block,
 * which is the case that actually matters: dr_mp3 grows its input buffer in
 * 64 KB steps and dr_flac grows during header parse, always the newest block.
 * Without it each growth abandoned the old copy, so a couple of steps ran a
 * 128 KB arena dry — and an OOM there looks exactly like a normal end of
 * track (the decoder reports 0 frames, the wrapper reports EOS). `oom` is the
 * only evidence, so surface it (player_stats()).
 */
#ifndef CORE_CODECS_ARENA_H
#define CORE_CODECS_ARENA_H

#include <stddef.h>

#include "decoder.h"

typedef struct decoder_arena {
    unsigned char *base;
    size_t         cap;
    size_t         used;
    size_t         high_water;   /* peak `used`, for sizing                 */
    size_t         last;         /* payload offset of the newest block, 0 = */
                                 /*   none (a payload never starts at 0)     */
    int            oom;          /* set once an allocation didn't fit       */
} decoder_arena_t;

/* Bind `buf` (cap bytes) as the arena's backing store, empty. */
void decoder_arena_init(decoder_arena_t *a, void *buf, size_t cap);

/* Reclaim everything (keeps high_water). Call between tracks. */
void decoder_arena_reset(decoder_arena_t *a);

/* A decoder_alloc_t whose alloc/realloc/free are backed by `a`. Pass the
 * result (by &) to a codec open(). */
decoder_alloc_t decoder_arena_allocator(decoder_arena_t *a);

#endif /* CORE_CODECS_ARENA_H */
