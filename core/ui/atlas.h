/*
 * core/ui/atlas.h — pre-rasterized glyph atlas (data layout only).
 *
 * One atlas per (font, size) combo. Generated at fixture-time by
 * tools/atlas_gen.sh; the C side just loads the resulting .h and
 * renders. No runtime TTF parsing, no FreeType dependency in the
 * firmware — exactly what PLAN.md prescribes.
 *
 * Atlases are first-class consts: they live in .rodata, the C
 * compiler resolves them at link time, no init step needed.
 *
 * Pixel format: alpha coverage. 0 = transparent, 255 = full fg.
 * core/ui/text.c alpha-blends fg into whatever the framebuffer
 * already holds at each pixel.
 *
 * This header is pure data description: stdint and nothing else. The
 * seven generated atlas headers include it, so keeping the HAL out of
 * here keeps the HAL out of all of them.
 */

#ifndef CORE_UI_ATLAS_H
#define CORE_UI_ATLAS_H

#include <stdint.h>

/*
 * Per-glyph layout. Indices 0..94 are printable ASCII (0x20..0x7E,
 * indexed by codepoint - 0x20); 95+ are the non-ASCII extras (Latin-1
 * + smart punctuation) appended by tools/atlas_gen.py, reached via the
 * codepoint->index table in atlas/glyphmap.h (see core/ui/text.c).
 *
 * data_offset  byte offset into atlas_t.data where this glyph's
 *              alpha rows begin. Rows are h × w bytes packed with
 *              no padding (width != stride). Bytes are 0..255.
 * w, h         glyph bitmap dimensions in pixels.
 * offset_x     x offset from pen position to top-left of bitmap.
 * offset_y     px BELOW the ascender line where this glyph's ink
 *              starts. PIL convention; positive means "this glyph's
 *              top is N px below the ascender." text.c converts to
 *              baseline-relative coordinates internally.
 * advance      pen advance after drawing this glyph, in 1/64 px
 *              (26.6 fixed point) — NOT whole pixels.
 *
 *              It used to be whole pixels, rounded per glyph by the
 *              generator. At 9-13px a glyph's true advance is
 *              routinely a half pixel (Nunito regular-13 'k' is
 *              6.500, 't' is 4.547), so rounding each one
 *              independently made adjacent gaps in the same word
 *              differ by ~1px — the "letters aren't evenly spaced"
 *              look — and the error accumulated along a string
 *              (+1.81px across "Yesterday", +2.37px across "The
 *              Velvet Underground" at bold-13), which also
 *              mis-centres centred text.
 *
 *              Widening this field is FREE: the struct was 7 bytes
 *              padded to 8, and is still 8.
 */
typedef struct {
    uint16_t data_offset;
    uint8_t  w;
    uint8_t  h;
    int8_t   offset_x;
    int8_t   offset_y;
    uint16_t advance;
} atlas_glyph_t;

/* Fixed-point shift for atlas_glyph_t.advance (1/64 px). */
#define ATLAS_ADV_SHIFT 6
#define ATLAS_ADV_ONE   (1 << ATLAS_ADV_SHIFT)
/* Round a 26.6 pen position to whole pixels. */
#define ATLAS_ADV_PX(v) (((v) + (ATLAS_ADV_ONE / 2)) >> ATLAS_ADV_SHIFT)

typedef struct {
    const atlas_glyph_t *glyphs;          /* 95 ASCII + non-ASCII extras */
    const uint8_t       *data;            /* concatenated glyph alpha rows */
    int8_t               ascent;          /* px above baseline */
    int8_t               descent;         /* px below baseline (positive) */
    int8_t               line_height;     /* recommended row spacing */
} atlas_t;

/* The atlases we ship (declared here, defined in the generated .h files
 * pulled in by core/ui/text.c — its TU is their sole definition site). */
extern const atlas_t NUNITO_REGULAR_9;
extern const atlas_t NUNITO_REGULAR_11;
extern const atlas_t NUNITO_REGULAR_13;
extern const atlas_t NUNITO_BOLD_11;
extern const atlas_t NUNITO_BOLD_13;
extern const atlas_t NUNITO_BOLD_17;

/* Measurement and rendering live in core/ui/text.h (text_width /
 * text_draw*), which is what every caller uses. */

#endif /* CORE_UI_ATLAS_H */
