/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/text.h — freestanding antialiased bitmap-font text renderer.
 *
 * Draws real Nunito type (gamma-correct grayscale AA) into an RGB565
 * framebuffer. Backed by the pre-rasterized glyph atlases in
 * core/apps/ui/atlas/ headers (our own data). Builds both host-side (sim
 * test) and freestanding on bare-metal ARM (-DCORE_FREESTANDING): no
 * libc, no libm, no malloc — the gamma blend uses static sRGB<->linear
 * lookup tables, all integer math.
 *
 * Coordinate convention: matches the sim's atlas_render — the pen `y`
 * is the text BASELINE (the bottom of non-descender glyphs), the
 * standard typography origin. `x` is the left edge of the pen.
 */

#ifndef CORE_UI_TEXT_H
#define CORE_UI_TEXT_H

#include <stdint.h>

/* An atlas font handle (opaque wrapper over the atlas.h data). */
typedef struct text_font text_font_t;

/* Non-ASCII atlas extras, embedded in UI strings as private single-byte codes
 * (the atlas is otherwise ASCII-only; see text.c glyph_index + atlas_gen.py).
 * Use as string literals, e.g. ui_text(x, y, UI_GLYPH_LAQUO "Back", …) or a
 * separator: "Artist " UI_GLYPH_MIDDOT " Album". */
#define UI_GLYPH_LAQUO  "\x01"   /* ‹ single left  angle quote  */
#define UI_GLYPH_RAQUO  "\x02"   /* › single right angle quote  */
#define UI_GLYPH_MIDDOT "\x03"   /* · middle dot                */

/* The built-in Nunito faces, backed by the atlas data. One getter
 * per shipped atlas header (regular 9/11/13; bold 9/11/13/17 — there is
 * no regular-17 atlas in the tree). Each returns a stable pointer to a
 * .rodata-resident handle; never NULL. */
const text_font_t *text_font_regular_9(void);
const text_font_t *text_font_regular_11(void);
const text_font_t *text_font_regular_13(void);
const text_font_t *text_font_bold_9(void);
const text_font_t *text_font_bold_11(void);
const text_font_t *text_font_bold_13(void);
const text_font_t *text_font_bold_17(void);

/*
 * Draw ASCII string `s` into a fb_w x fb_h RGB565 framebuffer (row-major
 * uint16_t) at pen (x, y), where `y` is the text BASELINE, in colour
 * `ink` (RGB565). Each glyph's coverage is gamma-correctly alpha-blended
 * over whatever is already in the framebuffer. Clips to [0,fb_w)x[0,fb_h)
 * — never writes out of bounds. Control codes advance by the space width
 * and draw nothing; a codepoint the atlas has no glyph for (CJK, Cyrillic,
 * emoji, …) draws a hollow .notdef box, so text is never invisible.
 *
 * Returns the pen x after the string (x + sum of advances).
 */
int text_draw(uint16_t *fb, int fb_w, int fb_h, int x, int y,
              const char *s, const text_font_t *font, uint16_t ink);

/* Like text_draw but clips output to the horizontal window [clip_x0, clip_x1)
 * — used by the marquee to scroll long text through a fixed region. */
int text_draw_clip(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                   const char *s, const text_font_t *font, uint16_t ink,
                   int clip_x0, int clip_x1);

/* text_draw_clip plus a vertical window [clip_y0, clip_y1) — keeps a marquee's
 * scrolling glyphs inside their list row. */
int text_draw_clip_v(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                     const char *s, const text_font_t *font, uint16_t ink,
                     int clip_x0, int clip_x1, int clip_y0, int clip_y1);

/* Pixel width the string would advance, without drawing. Always agrees with
 * the pen the text_draw* calls return for the same string and font. */
int text_width(const char *s, const text_font_t *font);

/* Recommended row spacing (line height) for this face, in pixels. */
int text_line_height(const text_font_t *font);

/* Pixels above the baseline for this face (for top-origin layout:
 * baseline_y = top_y + text_ascent(font)). */
int text_ascent(const text_font_t *font);

/* Pixels the font descends below the baseline (glyph ink box = ascent+descent,
 * tighter than line_height which includes inter-line leading). */
int text_descent(const text_font_t *font);

#endif /* CORE_UI_TEXT_H */
