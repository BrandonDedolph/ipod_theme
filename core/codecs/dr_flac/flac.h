/*
 * core/codecs/dr_flac/flac.h — FLAC decoder ops, vendored dr_flac under the hood.
 */

#ifndef CORE_CODECS_FLAC_H
#define CORE_CODECS_FLAC_H

#include "../decoder.h"

/*
 * Returns the FLAC decoder ops singleton. Implementation lives in
 * flac.c; uses dr_flac.h to do the actual decoding.
 */
const decoder_ops_t *flac_decoder_ops(void);

/*
 * Streaming open: decode directly from a pull byte source (`src`) instead of
 * an in-RAM buffer, so a multi-MB FLAC never has to be fully resident. On
 * success populates `d` (metadata + opaque drflac handle) and returns
 * DECODER_OK; thereafter drive it with the ops from flac_decoder_ops()
 * (decode/seek/close) — they operate on `d` regardless of how it was opened.
 * `alloc` must be non-NULL on hw (backs dr_flac's internal buffers).
 */
int flac_open_stream(decoder_t *d, decoder_source_t *src,
                     const decoder_alloc_t *alloc);

/*
 * ReplayGain: apply `db_q8` (gain in 1/256 dB, e.g. -1864 for "-7.28 dB") as a
 * digital pre-scale on everything decode() emits from now on. Clipping is
 * handled by saturating to the int16 range, and positive gain is capped at
 * +12 dB so a mis-tagged file can't turn the output into a square wave.
 *
 * Call AFTER open, only when the file actually carries a REPLAYGAIN_* tag —
 * a 0 dB gain is unity, so an untagged file must not call this at all (that
 * keeps the decode path bit-identical to the no-ReplayGain build, which the
 * codec KATs assert).
 *
 * The setting is per-open and global to the wrapper: only one FLAC decoder is
 * ever live in this firmware, and every open() resets it to unity.
 */
void flac_set_gain_db_q8(decoder_t *d, int db_q8);

#endif /* CORE_CODECS_FLAC_H */
