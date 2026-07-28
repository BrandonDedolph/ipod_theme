/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/tests/ui/text_test.c — host test for the freestanding text renderer.
 *
 * Renders a known string into a small RGB565 buffer over a known bg in a
 * known ink and asserts:
 *   (a) text_width > 0 and is additive (== sum of per-glyph substrings),
 *       and equals the pen advance returned by text_draw;
 *   (b) at least one interior pixel got blended toward the ink;
 *   (c) pixels fully outside glyph coverage are untouched;
 *   (d) drawing partly off every edge clips safely — no OOB write (checked
 *       with sentinel guard bytes bracketing the logical framebuffer).
 * Exit 0 on success, non-zero on the first failed check.
 */

#include "text.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define W 48
#define H 32
#define PAD 64                    /* guard cells before and after the fb */
#define BG  0xFFFFu               /* white  */
#define INK 0x0000u               /* black  */
#define SENT 0xA55Au              /* sentinel guard value */

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); fails++; } \
} while (0)

int main(void) {
    const text_font_t *font = text_font_bold_17();
    CHECK(font != NULL, "font handle is NULL");

    /* ---- (a) width: positive, matches draw advance ----------------- */
    int w_ag = text_width("Ag", font);
    int w_a  = text_width("A",  font);
    int w_g  = text_width("g",  font);
    CHECK(w_ag > 0, "text_width(\"Ag\") not positive");
    CHECK(w_a > 0 && w_g > 0, "single-glyph widths not positive");
    /*
     * Width is additive only to within kerning and a half pixel of rounding.
     *
     * This used to assert exact additivity (w_ag == w_a + w_g), which stopped
     * being true when kerning landed — a kerned pair is deliberately NARROWER
     * than its glyphs measured apart, and advances now accumulate in 26.6 and
     * round once per call. That assertion survived only because "Ag" happens
     * not to be a kern pair in Nunito; it would have passed while every kerned
     * pair on the device rendered differently from what it claimed.
     */
    CHECK(w_ag <= w_a + w_g + 1 && w_ag >= w_a + w_g - 3,
          "text_width wildly off the sum of its glyphs");

    /*
     * ...and kerning is actually APPLIED. "To" is a real Nunito pair (about
     * -1px at 13px), so the pair must measure narrower than the two glyphs
     * measured separately. Without this, an atlas regenerated with an empty
     * kern table (e.g. Pillow falling back to the basic layout engine, which
     * silently applies no kerning) would look correct to every other check
     * here.
     */
    int w_To = text_width("To", font);
    int w_T  = text_width("T",  font);
    int w_o  = text_width("o",  font);
    CHECK(w_To < w_T + w_o, "kerning not applied ('To' not tightened)");

    /*
     * The clipped tail must agree with text_width() to the pixel.
     *
     * text_draw_c() has THREE pen paths: the main loop, the "left of the clip
     * window" skip, and the "past the right edge" tail that stops rasterizing
     * but keeps accumulating so the returned pen stays the caller's contract
     * (the marquee chains on it). Tracking and kerning have to be applied in
     * all three; getting the tail wrong is invisible until text is clipped,
     * which is exactly when a marquee is running.
     *
     * Drawn entirely to the LEFT of a zero-width window, so every glyph takes
     * the tail path while the pen must still come out equal to the measure.
     */
    {
        static uint16_t probe[W * H];
        const char *kerned = "To Ta Ye AV";     /* real kern pairs + spaces */
        int wk = text_width(kerned, font);
        /* clip window [0,0) is empty, so every glyph takes the tail path. */
        int pen_clip = text_draw_clip(probe, W, H, 0, text_ascent(font),
                                      kerned, font, INK, 0, 0);
        CHECK(pen_clip == wk,
              "clipped-tail pen != text_width (tracking/kerning path drift)");
    }
    CHECK(text_line_height(font) > 0, "line_height not positive");
    CHECK(text_ascent(font) > 0, "ascent not positive");

    /* ---- buffer with guard cells on both sides --------------------- */
    static uint16_t mem[PAD + W * H + PAD];
    for (size_t i = 0; i < sizeof(mem) / sizeof(mem[0]); i++) mem[i] = SENT;
    uint16_t *fb = &mem[PAD];
    for (int i = 0; i < W * H; i++) fb[i] = BG;

    /* Draw "Ag" with a comfortable margin from the edges. Baseline near
     * the bottom so the whole face fits. */
    int baseline = text_ascent(font) + 2;
    int pen = text_draw(fb, W, H, 4, baseline, "Ag", font, INK);
    CHECK(pen == 4 + w_ag, "text_draw advance != x + text_width");

    /* ---- (b) at least one pixel blended toward ink ----------------- */
    int changed = 0, toward_ink = 0;
    for (int i = 0; i < W * H; i++) {
        if (fb[i] != BG) {
            changed++;
            /* ink is black, bg white: any change darkens toward ink. */
            if (fb[i] < BG) toward_ink++;
        }
    }
    CHECK(changed > 0, "no pixels were drawn");
    CHECK(toward_ink > 0, "no pixel blended toward ink");

    /* An antialiased face must produce partial-coverage (grey) pixels,
     * not just fully-on ones — proves the gamma blend actually ran. */
    int grey = 0;
    for (int i = 0; i < W * H; i++) {
        if (fb[i] != BG && fb[i] != INK) grey++;
    }
    CHECK(grey > 0, "no antialiased (partial-coverage) pixels found");

    /* ---- (c) far corner untouched ---------------------------------- */
    CHECK(fb[(H - 1) * W + (W - 1)] == BG, "far corner unexpectedly modified");

    /* ---- (d) clip safety: draw partly off every edge --------------- */
    /* Re-fill, then hammer all four edges + a huge string. If any write
     * escaped [0,W)x[0,H) it would corrupt a guard cell. */
    for (int i = 0; i < W * H; i++) fb[i] = BG;
    text_draw(fb, W, H, -6, baseline, "Ag", font, INK);          /* off left  */
    text_draw(fb, W, H, W - 3, baseline, "Ag", font, INK);       /* off right */
    text_draw(fb, W, H, 4, -2, "Ag", font, INK);                 /* off top   */
    text_draw(fb, W, H, 4, H + 20, "Ag", font, INK);             /* off bottom*/
    text_draw(fb, W, H, -100, -100, "clipme", font, INK);        /* fully out */
    text_draw(fb, W, H, 0, baseline,
              "The quick brown fox jumps over 0123456789!",       /* overflow  */
              font, INK);

    int guard_ok = 1;
    for (int i = 0; i < PAD; i++) {
        if (mem[i] != SENT) guard_ok = 0;
        if (mem[PAD + W * H + i] != SENT) guard_ok = 0;
    }
    CHECK(guard_ok, "OOB write detected (guard cell clobbered)");

    if (fails) {
        fprintf(stderr, "text_test: %d check(s) failed\n", fails);
        return 1;
    }
    printf("text_test: OK (w_ag=%d, changed=%d, grey=%d)\n", w_ag, changed, grey);
    return 0;
}
