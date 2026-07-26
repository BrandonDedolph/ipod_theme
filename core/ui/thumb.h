/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/thumb.h — freestanding RGB565 downscalers.
 *
 * Album lists want a small cover chip next to each row, and album-detail
 * screens a mid-size cover — but the device can't decode JPEG at runtime.
 * The host tools/coreart.py pre-renders a full-size folder.art (120x120)
 * and a small folder.thm (28x28, exactly ARTCACHE_DIM) per album. When a
 * dedicated folder.thm isn't present, these shrink the already-loaded
 * folder.art to whatever box the layout needs (e.g. 56x56 detail header,
 * 28x28 list chip).
 *
 * Integer-only, no libc/libm/malloc, no allocation (the caller owns dst),
 * and no divide per destination pixel — the ARM7TDMI has no hardware
 * divide, so the sample maps are built by add-and-carry instead. Links
 * into core.elf and the host thumb_test.
 */

#ifndef CORE_UI_THUMB_H
#define CORE_UI_THUMB_H

#include <stdint.h>

/*
 * Downscale (or copy) the src_w x src_h RGB565 image `src` into the
 * dst_w x dst_h RGB565 box `dst`, both row-major uint16_t. Uses
 * nearest-neighbor sampling:
 *
 *     dst[y*dst_w + x] = src[(y*src_h/dst_h)*src_w + (x*src_w/dst_w)]
 *
 * The map lands in [0,src_w)x[0,src_h) for every dst pixel, so it never
 * reads out of `src` even for non-integer ratios (e.g. 120 -> 28). When
 * dst dims equal src dims it is an exact 1:1 copy. `dst` must hold at
 * least dst_w*dst_h pixels; buffers must not overlap. If any dimension
 * is <= 0 the call is a no-op.
 */
void thumb_downscale_rgb565(const uint16_t *src, int src_w, int src_h,
                            uint16_t *dst, int dst_w, int dst_h);

/*
 * Same contract as thumb_downscale_rgb565, but each destination pixel is the
 * average of the source box that maps to it rather than one sample from its
 * corner. Use this for a BIG shrink — a 120x120 cover into a 28x28 chip or a
 * 56x56 detail header — where nearest-neighbour discards ~95% of the source
 * and makes the art crawl as the list scrolls. It costs two divides per
 * destination pixel, which is nothing at the once-per-album-entry cadence
 * these covers are built at.
 *
 * Shrinks gentler than 1.5x in either axis (including dst == src, the
 * pre-baked 28x28 folder.thm) fall through to thumb_downscale_rgb565 — at
 * that ratio a box is barely wider than one pixel, so averaging would only
 * cost sharpness. The 1:1 case therefore stays an exact copy.
 */
void thumb_box_rgb565(const uint16_t *src, int src_w, int src_h,
                      uint16_t *dst, int dst_w, int dst_h);

#endif /* CORE_UI_THUMB_H */
