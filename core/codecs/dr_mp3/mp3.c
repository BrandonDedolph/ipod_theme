/*
 * core/codecs/dr_mp3/mp3.c — MP3 decoder, wrapping dr_mp3.
 *
 * dr_mp3 decodes natively to int16_t PCM (no extra downconversion
 * step needed, unlike dr_flac which has to drop bits for >16-bit
 * source). It uses one allocation at init for the bitstream / IMDCT
 * working memory and (in our config) doesn't allocate on the
 * decode hot path.
 *
 * Allocator: dr_mp3 mirrors dr_flac's drmp3_allocation_callbacks
 * struct. Same translation pattern as flac.c. When the caller passes
 * NULL for decoder_alloc_t, we fall through to libc malloc — fine on
 * sim; on hw (no malloc) the caller MUST pass a non-NULL struct
 * backed by the audio engine's static sub-allocator, otherwise dr_mp3
 * will silently fail at its first internal alloc.
 *
 * Channels: dr_mp3 only ever emits mono or stereo (MP3 doesn't have
 * higher channel counts in the formats we care about), so no guard
 * needed beyond the "channels > 2" check we apply to all wrappers
 * for consistency.
 *
 * Gapless: dr_mp3 already applies the LAME/Xing encoder delay and end
 * padding at DECODE time (drmp3_read_pcm_frames_raw skips
 * delayInPCMFrames at the head and stops paddingInPCMFrames short of
 * totalPCMFrameCount), so a tagged MP3 comes out trimmed — no trim of
 * our own is needed or wanted here. It only works while
 * totalPCMFrameCount is the tag's value, which is why the estimate in
 * mp3_open_stream() writes only decoder_t.total_frames and never
 * touches the drmp3 field.
 *
 * Lossy codec testing: MP3 isn't bit-stable across decoder
 * implementations, so the KAT compares against PCM captured *from
 * dr_mp3 itself* on first generation, not against external truth.
 * That gives us regression protection (any change to dr_mp3 or our
 * wrapper will trip the KAT) without requiring a perfect oracle.
 */

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO           /* no fopen / fread paths */

/*
 * Freestanding build (bare-metal ARM, -DCORE_FREESTANDING): route dr_mp3's
 * config hooks away from libc. Assertions compile out; the default MALLOC/
 * REALLOC/FREE become NULL/no-op because we ALWAYS pass allocation callbacks
 * (the static arena) so the defaults are never taken; memory ops go through
 * our word-optimised memcpy/memset/memmove.
 *
 * MP3's synthesis filter is floating point, unlike FLAC's pure-integer path.
 * dr_mp3 uses only plain float arithmetic (multiply/add over precomputed
 * tables) — it includes no <math.h> and calls no libm function — so the
 * compiler lowers those ops to libgcc's soft-float runtime (__aeabi_fmul,
 * __aeabi_fadd, __aeabi_f2iz, …), which links via -lgcc. No libm, no libc.
 */
#ifdef CORE_FREESTANDING
#include "../../lib/mem.h"
#define DRMP3_ASSERT(expr)               ((void)0)
#define DRMP3_MALLOC(sz)                 ((void *)0)
#define DRMP3_REALLOC(p, sz)             ((void *)0)
#define DRMP3_FREE(p)                    ((void)0)
#define DRMP3_COPY_MEMORY(dst, src, sz)  memcpy((dst), (src), (sz))
#define DRMP3_MOVE_MEMORY(dst, src, sz)  memmove((dst), (src), (sz))
#define DRMP3_ZERO_MEMORY(p, sz)         memset((p), 0, (sz))
#endif

#include "dr_mp3.h"

#include "mp3.h"
#include "../decoder.h"

#include <stddef.h>
#include <stdint.h>
#ifdef CORE_FREESTANDING
#include "../../lib/mem.h"       /* memset for the wrapper's own zeroing */
#else
#include <stdlib.h>             /* malloc/free fallback (sim only) */
#include <string.h>             /* memset */
#endif

/* ---------- Allocator translation ---------------------------------- */

static void *mp3_malloc_thunk(size_t bytes, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    return a->alloc(a->userdata, bytes);
}
static void *mp3_realloc_thunk(void *ptr, size_t bytes, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    return a->realloc(a->userdata, ptr, bytes);
}
static void mp3_free_thunk(void *ptr, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    a->free(a->userdata, ptr);
}

/* ---------- ops ---------------------------------------------------- */

/*
 * dr_mp3 wants its own drmp3 struct held by the caller (it doesn't
 * heap-allocate the struct itself — the caller does). We wrap it in
 * a small holder so decoder_t.opaque has somewhere to live and so
 * we can free it on close().
 */
typedef struct {
    drmp3                    decoder;
    const decoder_alloc_t   *alloc;       /* may be NULL */
    drmp3_allocation_callbacks cb_storage; /* if alloc != NULL */
} mp3_state_t;

static void *mp3_state_alloc(const decoder_alloc_t *alloc) {
    if (alloc && alloc->alloc) {
        return alloc->alloc(alloc->userdata, sizeof(mp3_state_t));
    }
#ifdef CORE_FREESTANDING
    /* No libc heap on hw; the audio engine always supplies an allocator. */
    return NULL;
#else
    return malloc(sizeof(mp3_state_t));
#endif
}

static void mp3_state_free(mp3_state_t *s) {
    if (!s) return;
    if (s->alloc && s->alloc->free) {
        s->alloc->free(s->alloc->userdata, s);
    }
#ifndef CORE_FREESTANDING
    else {
        free(s);
    }
#endif
}

static int mp3_open(decoder_t *d, const void *src, size_t src_len,
                    const decoder_alloc_t *alloc) {
    if (!d || !src || src_len == 0) {
        return DECODER_ERR_INVALID;
    }

    mp3_state_t *s = mp3_state_alloc(alloc);
    if (!s) return DECODER_ERR_INTERNAL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;

    drmp3_allocation_callbacks *pcb = NULL;
    if (alloc && alloc->alloc && alloc->realloc && alloc->free) {
        s->cb_storage.pUserData = (void *)alloc;
        s->cb_storage.onMalloc  = mp3_malloc_thunk;
        s->cb_storage.onRealloc = mp3_realloc_thunk;
        s->cb_storage.onFree    = mp3_free_thunk;
        pcb = &s->cb_storage;
    }

    if (!drmp3_init_memory(&s->decoder, src, src_len, pcb)) {
        mp3_state_free(s);
        return DECODER_ERR_INVALID;
    }

    if (s->decoder.channels > 2) {
        drmp3_uninit(&s->decoder);
        mp3_state_free(s);
        return DECODER_ERR_UNSUPPORTED;
    }

    /* total_frames: cheap when the file has a Xing/Info LAME tag (the
     * vast majority of MP3s in the wild) — dr_mp3 already cached the
     * count during init. For tagless files this scans the stream to
     * count frames, then seeks back to frame 0; that scan is O(file)
     * but the bytes are already in memory at this point so no I/O.
     * Worst case is ~tens of ms per untagged track, paid once at play
     * time. The UI's progress bar and remaining-time label require it.
     *
     * A future tagcache build pass should compute and cache this
     * offline so we never scan on-device, but the on-the-fly path
     * keeps the firmware honest in the meantime. */
    drmp3_uint64 pcm_frames = drmp3_get_pcm_frame_count(&s->decoder);

    d->opaque          = s;
    d->sample_rate     = s->decoder.sampleRate;
    d->channels        = (uint16_t)s->decoder.channels;
    d->bits_per_sample = 16;          /* dr_mp3 emits s16 directly */
    d->total_frames    = (uint64_t)pcm_frames;
    return DECODER_OK;
}

/* ---------- Streaming open (decoder_source_t -> dr_mp3 procs) ------ */

static size_t mp3_on_read(void *ud, void *buf, size_t bytes) {
    decoder_source_t *s = (decoder_source_t *)ud;
    return s->read(s->userdata, buf, bytes);
}
static drmp3_bool32 mp3_on_seek(void *ud, int offset, drmp3_seek_origin origin) {
    decoder_source_t *s = (decoder_source_t *)ud;
    int org;
    if (origin == DRMP3_SEEK_SET) {
        org = DECODER_SEEK_SET;
    } else if (origin == DRMP3_SEEK_END) {
        /* dr_mp3 seeks to END at init() to probe for ID3v1/APE trailer
         * tags, so the source MUST honour END or init misjudges the
         * stream layout. (Same requirement dr_flac has for sizing.) */
        org = DECODER_SEEK_END;
    } else {
        org = DECODER_SEEK_CUR;
    }
    return s->seek(s->userdata, offset, org) ? DRMP3_TRUE : DRMP3_FALSE;
}
static drmp3_bool32 mp3_on_tell(void *ud, drmp3_int64 *cursor) {
    decoder_source_t *s = (decoder_source_t *)ud;
    int64_t p = s->tell(s->userdata);
    if (p < 0) {
        return DRMP3_FALSE;
    }
    *cursor = (drmp3_int64)p;
    return DRMP3_TRUE;
}

int mp3_open_stream(decoder_t *d, decoder_source_t *src,
                    const decoder_alloc_t *alloc) {
    if (!d || !src || !src->read || !src->seek || !src->tell) {
        return DECODER_ERR_INVALID;
    }

    mp3_state_t *s = mp3_state_alloc(alloc);
    if (!s) return DECODER_ERR_INTERNAL;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;

    drmp3_allocation_callbacks *pcb = NULL;
    if (alloc && alloc->alloc && alloc->realloc && alloc->free) {
        s->cb_storage.pUserData = (void *)alloc;
        s->cb_storage.onMalloc  = mp3_malloc_thunk;
        s->cb_storage.onRealloc = mp3_realloc_thunk;
        s->cb_storage.onFree    = mp3_free_thunk;
        pcb = &s->cb_storage;
    }

    /* dr_mp3's streaming init takes an extra onMeta callback (NULL = we
     * don't want ID3/APE metadata here) and the source as pUserData; the
     * source must outlive the decoder — we borrow, we don't copy. */
    if (!drmp3_init(&s->decoder, mp3_on_read, mp3_on_seek, mp3_on_tell,
                    NULL, src, pcb)) {
        mp3_state_free(s);
        return DECODER_ERR_INVALID;
    }

    if (s->decoder.channels > 2) {
        drmp3_uninit(&s->decoder);
        mp3_state_free(s);
        return DECODER_ERR_UNSUPPORTED;
    }

    /*
     * total_frames WITHOUT an O(file) scan.
     *
     * mp3_open()'s comment ("the bytes are already in memory so no I/O") is
     * true there and FALSE here: on a streaming source the exact-count helper
     * decodes every frame through the fat32 -> anti-skip -> read-ahead chain
     * and then seeks back to 0, outside the buffered window, forcing the whole
     * file to be re-read. At PIO rates a 10 MB CBR MP3 costs ~22 s of silence
     * before the first sample.
     *
     * So: take the count only when the Xing/Info tag already gave it to us
     * (drmp3_init cached it, and drmp3_get_pcm_frame_count then just subtracts
     * the LAME encoder delay/padding — no I/O). Otherwise estimate the length
     * from the first frame's bitrate and the size of the audio region. That is
     * exact for CBR, and for VBR it is a length readout being a bit off rather
     * than twenty seconds of dead air.
     */
    drmp3_uint64 pcm_frames;
    if (s->decoder.totalPCMFrameCount != DRMP3_UINT64_MAX) {
        pcm_frames = drmp3_get_pcm_frame_count(&s->decoder);
    } else {
        /* drmp3_init has decoded the first frame, so decoder.header holds its
         * 4-byte MPEG header — bitrate straight out of it, no extra reads. */
        unsigned kbps = drmp3_hdr_bitrate_kbps(s->decoder.decoder.header);
        drmp3_uint64 audio_bytes =
            (s->decoder.streamLength > s->decoder.streamStartOffset)
                ? (s->decoder.streamLength - s->decoder.streamStartOffset) : 0;
        pcm_frames = (kbps > 0 && audio_bytes > 0)
                   ? (audio_bytes * s->decoder.sampleRate) / (kbps * 125u)
                   : 0;                       /* 0 = unknown; the UI copes */
    }

    d->opaque          = s;
    d->sample_rate     = s->decoder.sampleRate;
    d->channels        = (uint16_t)s->decoder.channels;
    d->bits_per_sample = 16;          /* dr_mp3 emits s16 directly */
    d->total_frames    = (uint64_t)pcm_frames;
    d->ops             = mp3_decoder_ops();
    return DECODER_OK;
}

static int mp3_decode(decoder_t *d, int16_t *out, int max_frames) {
    if (!d || !d->opaque || !out || max_frames <= 0) {
        return DECODER_ERR_INVALID;
    }
    mp3_state_t *s = (mp3_state_t *)d->opaque;

    drmp3_uint64 got = drmp3_read_pcm_frames_s16(
        &s->decoder, (drmp3_uint64)max_frames, out);

    return (int)got;   /* 0 on EOS, > 0 on success */
}

static int mp3_seek(decoder_t *d, uint64_t target_frame) {
    if (!d || !d->opaque) return DECODER_ERR_INVALID;
    mp3_state_t *s = (mp3_state_t *)d->opaque;
    drmp3_bool32 ok = drmp3_seek_to_pcm_frame(
        &s->decoder, (drmp3_uint64)target_frame);
    return ok ? DECODER_OK : DECODER_ERR_INTERNAL;
}

static void mp3_close(decoder_t *d) {
    if (!d) return;
    if (d->opaque) {
        mp3_state_t *s = (mp3_state_t *)d->opaque;
        drmp3_uninit(&s->decoder);
        mp3_state_free(s);
        d->opaque = NULL;
    }
    d->ops = NULL;
}

static const decoder_ops_t MP3_OPS = {
    .name   = "mp3",
    .open   = mp3_open,
    .decode = mp3_decode,
    .seek   = mp3_seek,
    .close  = mp3_close,
};

const decoder_ops_t *mp3_decoder_ops(void) {
    return &MP3_OPS;
}
