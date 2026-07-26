/*
 * core/codecs/dr_flac/flac.c — FLAC decoder, wrapping dr_flac.
 *
 * dr_flac decodes natively to int32_t at the source's bit depth
 * (16/20/24-bit). We expose 16-bit signed interleaved PCM.
 *
 * Bit depth handling:
 *   - 16-bit source: drflac_read_pcm_frames_s16 straight through. The
 *     conversion is exact (no bits are dropped), so this stays the
 *     zero-cost, bit-exact path the codec KAT pins.
 *   - >16-bit source: we take the s32 output and round-to-nearest with
 *     TPDF dither instead of letting dr_flac arithmetic-right-shift.
 *     A bare shift truncates toward negative infinity: a constant
 *     -0.5 LSB DC offset plus quantization error correlated with the
 *     signal, which on quiet passages is audible as grain riding the
 *     music rather than as steady noise. Rounding kills the DC term
 *     and ±1 LSB triangular dither decorrelates what's left.
 *
 * ReplayGain (flac_set_gain_db_q8) is a Q12 multiply applied in the
 * same pass, saturating to int16 so a positive gain can't wrap.
 *
 * Allocator: dr_flac exposes drflac_allocation_callbacks so we can
 * thread our static sub-allocator through cleanly. When the caller
 * passes a non-NULL decoder_alloc_t we translate; NULL falls through
 * to dr_flac's default (DRFLAC_MALLOC / DRFLAC_FREE / DRFLAC_REALLOC,
 * which are libc malloc unless we override the macros at compile time).
 *
 * Channels: we hard-cap at 2 (stereo). FLAC supports up to 8 channels;
 * the audio engine downstream only expects mono/stereo, and a 5.1
 * FLAC would silently overflow caller buffers on decode. We refuse
 * to open such streams with DECODER_ERR_UNSUPPORTED.
 */

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO          /* no fopen / fread paths */
#define DR_FLAC_NO_OGG            /* skip Ogg-FLAC for now; can re-enable */
/*
 * CRC-16 verification off. dr_flac otherwise CRCs every decoded frame
 * byte-by-byte on top of the decode itself — real cost on a 80 MHz ARM7TDMI
 * with no spare margin — and we act on the result nowhere: a failed frame
 * comes back from drflac_read_pcm_frames_s16 as 0 frames, which flac_decode
 * reports as end of stream exactly like a genuine EOS. Paying for a check we
 * can't distinguish from the normal end of a track buys nothing.
 */
#define DR_FLAC_NO_CRC

/*
 * Freestanding build (bare-metal ARM, -DCORE_FREESTANDING): route dr_flac's
 * config hooks away from libc. Assertions compile out; the default MALLOC/
 * REALLOC/FREE become NULL/no-op because we ALWAYS pass allocation callbacks
 * (the static arena) so the defaults are never taken; memory ops go through
 * our word-optimised memcpy/memset. FLAC decode is pure integer, so no libm.
 */
#ifdef CORE_FREESTANDING
#include "../../lib/mem.h"
#define DRFLAC_ASSERT(expr)            ((void)0)
#define DRFLAC_MALLOC(sz)             ((void *)0)
#define DRFLAC_REALLOC(p, sz)         ((void *)0)
#define DRFLAC_FREE(p)                 ((void)0)
#define DRFLAC_COPY_MEMORY(dst, src, sz) memcpy((dst), (src), (sz))
#define DRFLAC_ZERO_MEMORY(p, sz)        memset((p), 0, (sz))
#endif

#include "dr_flac.h"

#include "flac.h"
#include "../decoder.h"

#include <stddef.h>
#include <stdint.h>

/* ---------- Allocator translation ---------------------------------- */

static void *flac_malloc_thunk(size_t bytes, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    return a->alloc(a->userdata, bytes);
}
static void *flac_realloc_thunk(void *ptr, size_t bytes, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    return a->realloc(a->userdata, ptr, bytes);
}
static void flac_free_thunk(void *ptr, void *user) {
    const decoder_alloc_t *a = (const decoder_alloc_t *)user;
    a->free(a->userdata, ptr);
}

/* ---------- Output conditioning: ReplayGain + 16-bit quantization -- */

#define FLAC_GAIN_UNITY 4096            /* Q12 */

/* Digital pre-scale in Q12, unity until flac_set_gain_db_q8 says otherwise.
 * A file static rather than per-decoder state because decoder_t.opaque is the
 * bare drflac handle and only one FLAC decoder is ever live; every open()
 * resets it. */
static int32_t g_gain_q12 = FLAC_GAIN_UNITY;

/* Dither LCG state. Free-running across tracks — the sequence just needs to
 * be uncorrelated with the signal, not reproducible. */
static uint32_t g_dither_state = 0x2545F491u;

/*
 * 2^(x/4096) in Q12. Integer octaves come out of a shift; the fractional
 * part is linearly interpolated between 17 tabulated sixteenths, carried in
 * Q20 so the negative-octave shift doesn't eat the precision. Over the
 * ReplayGain range we clamp to (-30 .. +12 dB) the error is under 0.05%,
 * i.e. ~0.004 dB. No libm on the target, and this runs once per track.
 */
static const uint16_t EXP2_Q12[17] = {
    4096, 4277, 4467, 4664, 4871, 5087, 5312, 5547,
    5793, 6049, 6317, 6597, 6889, 7194, 7512, 7845, 8192,
};

static int32_t exp2_q12(int32_t x_q12) {
    int32_t oct = x_q12 >> 12;                    /* floor, works for negatives */
    int32_t fr  = x_q12 - (oct << 12);            /* 0..4095                     */
    int32_t i   = fr >> 8;                        /* 0..15                       */
    int32_t t   = fr & 0xFF;                      /* 0..255                      */
    int32_t lo  = EXP2_Q12[i];
    int32_t v   = lo + ((((int32_t)EXP2_Q12[i + 1] - lo) * t) >> 8);
    v <<= 8;                                      /* Q20 working precision       */
    v = (oct >= 0) ? (v << oct) : (v >> -oct);
    return (v + 128) >> 8;
}

void flac_set_gain_db_q8(decoder_t *d, int db_q8) {
    if (!d) {
        return;
    }
    /* Cap at +12 dB: a mis-tagged (or hostile) "+60 dB" would otherwise
     * saturate every sample into a square wave. -30 dB is the low clamp —
     * below that the Q12 gain is quantization noise anyway. */
    if (db_q8 > 12 * 256) {
        db_q8 = 12 * 256;
    } else if (db_q8 < -30 * 256) {
        db_q8 = -30 * 256;
    }
    /* linear = 10^(dB/20) = 2^(dB * log2(10)/20), and log2(10)/20 = 0.1660964.
     * Converting Q8 dB to the Q12 octaves exp2_q12 wants is a multiply by
     * 0.1660964 * 4096/256 = 2.657542; 680/256 = 2.65625 is 0.05% under. */
    g_gain_q12 = exp2_q12(((int32_t)db_q8 * 680) >> 8);
}

/* Saturating Q12 gain over an interleaved s16 block, in place. */
static void apply_gain_s16(int16_t *p, int samples) {
    int32_t g = g_gain_q12;
    for (int i = 0; i < samples; i++) {
        int32_t v = ((int32_t)p[i] * g) >> 12;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        p[i] = (int16_t)v;
    }
}

/* Triangular (TPDF) dither: the difference of two independent uniform draws,
 * i.e. ±1 output LSB in dr_flac's left-aligned s32 domain (1 LSB = 1<<16). */
static int32_t tpdf_dither(void) {
    g_dither_state = g_dither_state * 1664525u + 1013904223u;
    int32_t a = (int32_t)(g_dither_state >> 16);
    g_dither_state = g_dither_state * 1664525u + 1013904223u;
    int32_t b = (int32_t)(g_dither_state >> 16);
    return a - b;                             /* triangular on (-65536, 65536) */
}

/* s32 (left-aligned to the full 32-bit range) -> s16, round-to-nearest with
 * TPDF dither and the Q12 ReplayGain pre-scale, saturating. Only reached for
 * >16-bit sources, where dr_flac's own conversion would truncate. */
static void convert_s32_dithered(const drflac_int32 *in, int16_t *out, int samples) {
    int32_t g = g_gain_q12;
    for (int i = 0; i < samples; i++) {
        int64_t v = (int64_t)in[i];
        if (g != FLAC_GAIN_UNITY) {
            v = (v * g) >> 12;                /* may exceed s32: clipped below */
        }
        v += (int64_t)tpdf_dither() + (1 << 15);
        v >>= 16;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        out[i] = (int16_t)v;
    }
}

/* ---------- ops ---------------------------------------------------- */

static int flac_open(decoder_t *d, const void *src, size_t src_len,
                     const decoder_alloc_t *alloc) {
    if (!d || !src || src_len == 0) {
        return DECODER_ERR_INVALID;
    }

    drflac *f;
    if (alloc && alloc->alloc && alloc->realloc && alloc->free) {
        drflac_allocation_callbacks cb = {
            .pUserData = (void *)alloc,
            .onMalloc  = flac_malloc_thunk,
            .onRealloc = flac_realloc_thunk,
            .onFree    = flac_free_thunk,
        };
        f = drflac_open_memory(src, src_len, &cb);
    } else {
        f = drflac_open_memory(src, src_len, NULL);
    }
    if (!f) {
        /* dr_flac collapses all open failures (malformed header,
         * truncated stream, OOM) into a NULL return; we can't
         * distinguish them. */
        return DECODER_ERR_INVALID;
    }

    if (f->channels > 2) {
        drflac_close(f);
        return DECODER_ERR_UNSUPPORTED;
    }

    g_gain_q12         = FLAC_GAIN_UNITY;    /* no ReplayGain until asked for */
    d->opaque          = f;
    d->sample_rate     = f->sampleRate;
    d->channels        = (uint16_t)f->channels;
    d->bits_per_sample = (uint16_t)f->bitsPerSample;
    d->total_frames    = f->totalPCMFrameCount;
    return DECODER_OK;
}

/* ---------- Streaming open (decoder_source_t -> dr_flac procs) ------ */

static size_t flac_on_read(void *ud, void *buf, size_t bytes) {
    decoder_source_t *s = (decoder_source_t *)ud;
    return s->read(s->userdata, buf, bytes);
}
static drflac_bool32 flac_on_seek(void *ud, int offset,
                                  drflac_seek_origin origin) {
    decoder_source_t *s = (decoder_source_t *)ud;
    int org;
    if (origin == DRFLAC_SEEK_SET) {
        org = DECODER_SEEK_SET;
    } else if (origin == DRFLAC_SEEK_END) {
        org = DECODER_SEEK_END;
    } else {
        org = DECODER_SEEK_CUR;
    }
    return s->seek(s->userdata, offset, org) ? DRFLAC_TRUE : DRFLAC_FALSE;
}
static drflac_bool32 flac_on_tell(void *ud, drflac_int64 *cursor) {
    decoder_source_t *s = (decoder_source_t *)ud;
    int64_t p = s->tell(s->userdata);
    if (p < 0) {
        return DRFLAC_FALSE;
    }
    *cursor = (drflac_int64)p;
    return DRFLAC_TRUE;
}

int flac_open_stream(decoder_t *d, decoder_source_t *src,
                     const decoder_alloc_t *alloc) {
    if (!d || !src || !src->read || !src->seek || !src->tell) {
        return DECODER_ERR_INVALID;
    }

    drflac_allocation_callbacks cb;
    const drflac_allocation_callbacks *pcb = NULL;
    if (alloc && alloc->alloc && alloc->realloc && alloc->free) {
        cb.pUserData = (void *)alloc;
        cb.onMalloc  = flac_malloc_thunk;
        cb.onRealloc = flac_realloc_thunk;
        cb.onFree    = flac_free_thunk;
        pcb = &cb;
    }

    drflac *f = drflac_open(flac_on_read, flac_on_seek, flac_on_tell,
                            src, pcb);
    if (!f) {
        return DECODER_ERR_INVALID;
    }
    if (f->channels > 2) {
        drflac_close(f);
        return DECODER_ERR_UNSUPPORTED;
    }

    g_gain_q12         = FLAC_GAIN_UNITY;    /* no ReplayGain until asked for */
    d->opaque          = f;
    d->sample_rate     = f->sampleRate;
    d->channels        = (uint16_t)f->channels;
    d->bits_per_sample = (uint16_t)f->bitsPerSample;
    d->total_frames    = f->totalPCMFrameCount;
    d->ops             = flac_decoder_ops();
    return DECODER_OK;
}

/* Frames of s32 scratch for the >16-bit conversion path. 512 stereo frames =
 * 4 KB of .bss; the decode loop just runs a few passes per call. */
#define FLAC_S32_FRAMES 512
static drflac_int32 g_s32_scratch[FLAC_S32_FRAMES * 2];

static int flac_decode(decoder_t *d, int16_t *out, int max_frames) {
    if (!d || !d->opaque || !out || max_frames <= 0) {
        return DECODER_ERR_INVALID;
    }
    drflac *f = (drflac *)d->opaque;

    if (f->bitsPerSample > 16) {
        /* Round + dither instead of dr_flac's arithmetic right shift (see the
         * file header). Chunked through a fixed scratch so nothing scales with
         * max_frames. */
        unsigned ch    = f->channels ? f->channels : 1;
        int      total = 0;
        while (total < max_frames) {
            int want = max_frames - total;
            if (want > FLAC_S32_FRAMES) {
                want = FLAC_S32_FRAMES;
            }
            drflac_uint64 n = drflac_read_pcm_frames_s32(
                f, (drflac_uint64)want, g_s32_scratch);
            if (n == 0) {
                break;
            }
            convert_s32_dithered(g_s32_scratch, out + (size_t)total * ch,
                                 (int)((drflac_uint64)n * ch));
            total += (int)n;
            if ((int)n < want) {
                break;                        /* short read: end of stream */
            }
        }
        return total;
    }

    drflac_uint64 got = drflac_read_pcm_frames_s16(
        f, (drflac_uint64)max_frames, out);
    if (got > 0 && g_gain_q12 != FLAC_GAIN_UNITY) {
        apply_gain_s16(out, (int)(got * (f->channels ? f->channels : 1)));
    }

    /* dr_flac returns 0 for both EOS and mid-stream decode failure;
     * we can't distinguish without inspecting state, so EOS wins.
     * If the stream is corrupt mid-decode, downstream will hear
     * silence and the position counter will drift — acceptable for
     * a music player; we don't promise glitch-free decode of broken
     * files. */
    return (int)got;
}

static int flac_seek(decoder_t *d, uint64_t target_frame) {
    if (!d || !d->opaque) return DECODER_ERR_INVALID;
    drflac *f = (drflac *)d->opaque;
    drflac_bool32 ok = drflac_seek_to_pcm_frame(f, (drflac_uint64)target_frame);
    return ok ? DECODER_OK : DECODER_ERR_INTERNAL;
}

static void flac_close(decoder_t *d) {
    if (!d) return;
    if (d->opaque) {
        drflac_close((drflac *)d->opaque);
        d->opaque = NULL;
    }
    d->ops = NULL;
}

static const decoder_ops_t FLAC_OPS = {
    .name   = "flac",
    .open   = flac_open,
    .decode = flac_decode,
    .seek   = flac_seek,
    .close  = flac_close,
};

const decoder_ops_t *flac_decoder_ops(void) {
    return &FLAC_OPS;
}
