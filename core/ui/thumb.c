/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/thumb.c — freestanding RGB565 downscalers.
 *
 * Two entry points, no state, no allocation, no libc:
 *   thumb_downscale_rgb565()  nearest-neighbour — cheapest, exact, and the
 *                             right thing for a 1:1 copy or a gentle shrink.
 *   thumb_box_rgb565()        box average — for a big shrink (a 120x120 cover
 *                             into a 28 or 56 box), where nearest-neighbour
 *                             throws away 95% of the source and shimmers as
 *                             the art scrolls.
 *
 * Both drop straight into the freestanding core.elf UI path as well as the
 * host test.
 *
 * Neither one divides per destination pixel. The ARM7TDMI has no hardware
 * divide, so `(x * src_w) / dst_w` per pixel is an __aeabi_idiv call — 3,136
 * of them for the 120->56 album hero alone. The maps are strictly increasing,
 * so the same floor() comes out of an add-and-carry accumulator:
 *
 *     rem += src_w;  while (rem >= dst_w) { rem -= dst_w; s++; }
 *
 * which is bit-identical to floor(x*src_w/dst_w) and costs a couple of adds.
 * The inner while is bounded by (src_w + dst_w) / dst_w iterations — it can
 * only run as many times in total, across a whole row, as there are source
 * columns.
 *
 * Nearest-neighbour stays exact: for a shrink (dst <= src) the largest index
 * the map yields is (dst-1)*src/dst < src, so every read stays in bounds
 * without a clamp; the guards below only reject degenerate (non-positive)
 * dimensions.
 */

#include "thumb.h"

void thumb_downscale_rgb565(const uint16_t *src, int src_w, int src_h,
                            uint16_t *dst, int dst_w, int dst_h)
{
    if (src == 0 || dst == 0) {
        return;
    }
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    int sy = 0, yrem = 0;
    for (int y = 0; y < dst_h; y++) {
        const uint16_t *srow = src + (long)sy * src_w;   /* sy in [0, src_h) */
        uint16_t *drow = dst + (long)y * dst_w;
        int sx = 0, xrem = 0;
        for (int x = 0; x < dst_w; x++) {
            drow[x] = srow[sx];                          /* sx in [0, src_w) */
            xrem += src_w;
            while (xrem >= dst_w) { xrem -= dst_w; sx++; }
        }
        yrem += src_h;
        while (yrem >= dst_h) { yrem -= dst_h; sy++; }
    }
}

/* Ratio at which averaging starts to be worth it, x8 fixed point: 1.5x. Below
 * that a box covers barely more than one source pixel and nearest-neighbour is
 * both cheaper and sharper. */
#define BOX_MIN_RATIO_X8  12

void thumb_box_rgb565(const uint16_t *src, int src_w, int src_h,
                      uint16_t *dst, int dst_w, int dst_h)
{
    if (src == 0 || dst == 0) {
        return;
    }
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    /* Not enough of a shrink to average (this covers dst == src, the 28x28
     * folder.thm chip, which must stay a verbatim copy). */
    if (src_w * 8 < dst_w * BOX_MIN_RATIO_X8 ||
        src_h * 8 < dst_h * BOX_MIN_RATIO_X8) {
        thumb_downscale_rgb565(src, src_w, src_h, dst, dst_w, dst_h);
        return;
    }

    /* Each destination pixel averages the source box [sx0,sx1) x [sy0,sy1),
     * the same half-open spans nearest-neighbour would have sampled the first
     * pixel of. Box edges come from the same add-and-carry accumulator, so
     * the only divides are the two per destination pixel that turn the sums
     * back into a mean — and this runs once per album entry, not per frame. */
    int sy0 = 0, yrem = 0;
    for (int y = 0; y < dst_h; y++) {
        int sy1 = sy0;
        int yr = yrem + src_h;
        while (yr >= dst_h) { yr -= dst_h; sy1++; }
        /* Sampling range, clamped independently of the accumulator so a
         * degenerate box can never empty the sum or walk off `src` (and so the
         * clamp can never desync sy0 from yrem). */
        int j0 = sy0, j1 = sy1;
        if (j1 <= j0)    { j1 = j0 + 1; }
        if (j1 > src_h)  { j1 = src_h; }
        if (j0 >= j1)    { j0 = j1 - 1; }
        int bh = j1 - j0;

        uint16_t *drow = dst + (long)y * dst_w;
        int sx0 = 0, xrem = 0;
        for (int x = 0; x < dst_w; x++) {
            int sx1 = sx0;
            int xr = xrem + src_w;
            while (xr >= dst_w) { xr -= dst_w; sx1++; }
            int i0 = sx0, i1 = sx1;
            if (i1 <= i0)   { i1 = i0 + 1; }
            if (i1 > src_w) { i1 = src_w; }
            if (i0 >= i1)   { i0 = i1 - 1; }
            int bw = i1 - i0;

            unsigned rs = 0, gs = 0, bs = 0;
            for (int j = j0; j < j1; j++) {
                const uint16_t *srow = src + (long)j * src_w;
                for (int i = i0; i < i1; i++) {
                    uint16_t p = srow[i];
                    rs += (unsigned)(p >> 11) & 0x1Fu;
                    gs += (unsigned)(p >> 5)  & 0x3Fu;
                    bs += (unsigned)p         & 0x1Fu;
                }
            }
            unsigned n = (unsigned)(bw * bh);
            unsigned half = n >> 1;                      /* round to nearest */
            unsigned r = (rs + half) / n;
            unsigned g = (gs + half) / n;
            unsigned b = (bs + half) / n;
            drow[x] = (uint16_t)((r << 11) | (g << 5) | b);

            sx0 = sx1;
            xrem = xr;
        }
        sy0 = sy1;
        yrem = yr;
    }
}
