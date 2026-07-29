/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/text.c — freestanding antialiased text rendering.
 *
 * A gamma-correct alpha blend and glyph layout that is libc-free and draws
 * into a caller-supplied RGB565 framebuffer with explicit bounds (so it
 * needs neither lcd_framebuffer() nor the compile-time LCD_WIDTH/HEIGHT).
 * It links into core.elf on the device and into the host text_test.
 *
 * The atlas data (glyph metrics + concatenated alpha-coverage rows) lives
 * in the generated core/ui/atlas/ headers; including them here makes the
 * const atlas_t handles resolve at link time — no init step, no TTF
 * parsing, no allocation. Everything is .rodata.
 */

#include "text.h"

/* atlas_glyph_t / atlas_t and the generated atlas handles. The generated
 * headers each pull in ../atlas.h (which needs only stdint — freestanding-
 * safe), then define their static coverage data + a const atlas_t. This TU
 * is the sole definition site for those atlas_t symbols, so there is no
 * clash between the hw link and the host text_test link. */
#include "atlas.h"
#include "atlas/glyphmap.h"        /* ATLAS_CPMAP[]: codepoint -> glyph index */
#include "atlas/nunito_regular_9.h"
#include "atlas/nunito_regular_11.h"
#include "atlas/nunito_regular_13.h"
#include "atlas/nunito_bold_11.h"
#include "atlas/nunito_bold_13.h"
#include "atlas/nunito_bold_17.h"

/* text_font_t is just a thin, opaque wrapper over an atlas_t so the public
 * header need not expose the atlas layout. */
struct text_font {
    const atlas_t *a;
};

static const struct text_font FONT_REGULAR_9  = { &NUNITO_REGULAR_9  };
static const struct text_font FONT_REGULAR_11 = { &NUNITO_REGULAR_11 };
static const struct text_font FONT_REGULAR_13 = { &NUNITO_REGULAR_13 };
static const struct text_font FONT_BOLD_11    = { &NUNITO_BOLD_11    };
static const struct text_font FONT_BOLD_13    = { &NUNITO_BOLD_13    };
static const struct text_font FONT_BOLD_17    = { &NUNITO_BOLD_17    };

const text_font_t *text_font_regular_9(void)  { return &FONT_REGULAR_9;  }
const text_font_t *text_font_regular_11(void) { return &FONT_REGULAR_11; }
const text_font_t *text_font_regular_13(void) { return &FONT_REGULAR_13; }
const text_font_t *text_font_bold_11(void)    { return &FONT_BOLD_11;    }
const text_font_t *text_font_bold_13(void)    { return &FONT_BOLD_13;    }
const text_font_t *text_font_bold_17(void)    { return &FONT_BOLD_17;    }

/* ---------- Gamma LUTs ---------------------------------------------- *
 * Glyph alpha is linear-light coverage, but RGB565 channels are sRGB-
 * encoded. Blending in sRGB space yields anemic, grey-fringed text. So:
 * decode each channel to linear, mix by coverage there, re-encode to
 * sRGB on store. gamma=2.2 approximation, pre-baked so the hot path
 * never touches <math.h> (matters on the FPU-less ARM7). Tables are the
 * same ones atlas.c uses:
 *   srgb5_to_linear[i]  = round(((i/31) ** 2.2) * 255)
 *   srgb6_to_linear[i]  = round(((i/63) ** 2.2) * 255)
 *   linear_to_srgb5[i]  = round(((i/255) ** (1/2.2)) * 31)
 *   linear_to_srgb6[i]  = round(((i/255) ** (1/2.2)) * 63)
 */
static const uint8_t srgb5_to_linear[32] = {
      0,   0,   1,   1,   3,   5,   7,  10,  13,  17,  21,  26,  32,  38,  44,  52,
     60,  68,  77,  87,  97, 108, 120, 132, 145, 159, 173, 188, 204, 220, 237, 255,
};

static const uint8_t srgb6_to_linear[64] = {
      0,   0,   0,   0,   1,   1,   1,   2,   3,   4,   4,   5,   7,   8,   9,  11,
     13,  14,  16,  18,  20,  23,  25,  28,  31,  33,  36,  40,  43,  46,  50,  54,
     57,  61,  66,  70,  74,  79,  84,  89,  94,  99, 105, 110, 116, 122, 128, 134,
    140, 147, 153, 160, 167, 174, 182, 189, 197, 205, 213, 221, 229, 238, 246, 255,
};

static const uint8_t linear_to_srgb5[256] = {
      0,   2,   3,   4,   5,   5,   6,   6,   6,   7,   7,   7,   8,   8,   8,   9,
      9,   9,   9,  10,  10,  10,  10,  10,  11,  11,  11,  11,  11,  12,  12,  12,
     12,  12,  12,  13,  13,  13,  13,  13,  13,  14,  14,  14,  14,  14,  14,  14,
     15,  15,  15,  15,  15,  15,  15,  15,  16,  16,  16,  16,  16,  16,  16,  16,
     17,  17,  17,  17,  17,  17,  17,  17,  17,  18,  18,  18,  18,  18,  18,  18,
     18,  18,  19,  19,  19,  19,  19,  19,  19,  19,  19,  19,  20,  20,  20,  20,
     20,  20,  20,  20,  20,  20,  20,  21,  21,  21,  21,  21,  21,  21,  21,  21,
     21,  21,  21,  22,  22,  22,  22,  22,  22,  22,  22,  22,  22,  22,  23,  23,
     23,  23,  23,  23,  23,  23,  23,  23,  23,  23,  23,  24,  24,  24,  24,  24,
     24,  24,  24,  24,  24,  24,  24,  24,  25,  25,  25,  25,  25,  25,  25,  25,
     25,  25,  25,  25,  25,  25,  26,  26,  26,  26,  26,  26,  26,  26,  26,  26,
     26,  26,  26,  26,  26,  27,  27,  27,  27,  27,  27,  27,  27,  27,  27,  27,
     27,  27,  27,  27,  28,  28,  28,  28,  28,  28,  28,  28,  28,  28,  28,  28,
     28,  28,  28,  28,  29,  29,  29,  29,  29,  29,  29,  29,  29,  29,  29,  29,
     29,  29,  29,  29,  29,  30,  30,  30,  30,  30,  30,  30,  30,  30,  30,  30,
     30,  30,  30,  30,  30,  30,  30,  31,  31,  31,  31,  31,  31,  31,  31,  31,
};

static const uint8_t linear_to_srgb6[256] = {
      0,   5,   7,   8,  10,  11,  11,  12,  13,  14,  14,  15,  16,  16,  17,  17,
     18,  18,  19,  19,  20,  20,  21,  21,  22,  22,  22,  23,  23,  23,  24,  24,
     25,  25,  25,  26,  26,  26,  27,  27,  27,  27,  28,  28,  28,  29,  29,  29,
     29,  30,  30,  30,  31,  31,  31,  31,  32,  32,  32,  32,  33,  33,  33,  33,
     34,  34,  34,  34,  35,  35,  35,  35,  35,  36,  36,  36,  36,  37,  37,  37,
     37,  37,  38,  38,  38,  38,  38,  39,  39,  39,  39,  39,  40,  40,  40,  40,
     40,  41,  41,  41,  41,  41,  42,  42,  42,  42,  42,  42,  43,  43,  43,  43,
     43,  44,  44,  44,  44,  44,  44,  45,  45,  45,  45,  45,  45,  46,  46,  46,
     46,  46,  46,  47,  47,  47,  47,  47,  47,  48,  48,  48,  48,  48,  48,  48,
     49,  49,  49,  49,  49,  49,  49,  50,  50,  50,  50,  50,  50,  51,  51,  51,
     51,  51,  51,  51,  52,  52,  52,  52,  52,  52,  52,  53,  53,  53,  53,  53,
     53,  53,  54,  54,  54,  54,  54,  54,  54,  54,  55,  55,  55,  55,  55,  55,
     55,  56,  56,  56,  56,  56,  56,  56,  56,  57,  57,  57,  57,  57,  57,  57,
     57,  58,  58,  58,  58,  58,  58,  58,  58,  59,  59,  59,  59,  59,  59,  59,
     59,  60,  60,  60,  60,  60,  60,  60,  60,  60,  61,  61,  61,  61,  61,  61,
     61,  61,  62,  62,  62,  62,  62,  62,  62,  62,  62,  63,  63,  63,  63,  63,
};

/* Decode one UTF-8 sequence at *p, advance *p past it, and return the codepoint
 * (or -1 at the terminating NUL). Malformed bytes yield U+FFFD and advance one
 * byte so a bad string can't stall. UI strings are UTF-8; ASCII is the 1-byte
 * fast path (the common case).
 *
 * A truncated sequence advances ONE byte and re-syncs on the next call: the
 * continuation check reads only bytes at or before the NUL (the NUL fails the
 * check), so a chopped tag can never walk off the end of its buffer. */
static int utf8_next(const unsigned char **p) {
    unsigned char c = **p;
    if (c == 0) return -1;
    if (c < 0x80) { (*p)++; return c; }
    int n, cp, min;
    if      ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; min = 0x80;    }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; min = 0x800;   }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; min = 0x10000; }
    else { (*p)++; return 0xFFFD; }               /* stray continuation/lead */
    const unsigned char *q = *p + 1;
    for (int i = 0; i < n; i++) {
        if ((q[i] & 0xC0) != 0x80) { (*p)++; return 0xFFFD; }   /* truncated */
        cp = (cp << 6) | (q[i] & 0x3F);
    }
    *p += n + 1;
    /* Reject non-minimal ("overlong") forms: C0 80 must NOT decode to U+0000
     * and E0 80 AF must NOT decode to '/', or a crafted tag could smuggle a
     * NUL/path separator past a byte-level check. Same for the UTF-16
     * surrogate halves (never valid in UTF-8) and anything past U+10FFFF. */
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0xFFFD;
    }
    return cp;
}

/* Map a Unicode codepoint to an atlas glyph index, or -1 for "no glyph"
 * (rendered as space-width). ASCII 0x20..0x7E map linearly; the three legacy
 * private control bytes 0x01/0x02/0x03 alias ‹ › · (UI_GLYPH_* predate UTF-8);
 * everything else is looked up in the generated, codepoint-sorted ATLAS_CPMAP
 * (tools/atlas_gen.py EXTRAS -> atlas/glyphmap.h) by binary search. */
static int glyph_index(int cp) {
    if (cp >= 0x20 && cp <= 0x7E) return cp - 0x20;
    switch (cp) {                                 /* legacy private aliases */
        case 0x01: cp = 0x2039; break;            /* ‹ */
        case 0x02: cp = 0x203A; break;            /* › */
        case 0x03: cp = 0x00B7; break;            /* · */
        default: break;
    }
    int lo = 0, hi = ATLAS_CPMAP_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1, mc = ATLAS_CPMAP[mid].cp;
        if      (cp < mc) hi = mid - 1;
        else if (cp > mc) lo = mid + 1;
        else return ATLAS_CPMAP[mid].idx;
    }
    return -1;
}

/* ---------- .notdef ------------------------------------------------- *
 * The atlas covers ASCII + Latin-1 + smart punctuation and nothing else, so a
 * CJK / Cyrillic / Greek / emoji track name used to advance by a space and draw
 * nothing at all: the row rendered as a run of invisible blanks, with no hint
 * that there was ever a title there. Draw the typographic fallback instead — a
 * hollow box at cap height, one per unrenderable codepoint.
 *
 * Control codes are NOT boxed: a stray \n or \t smuggled in by an ID3 tag is
 * meant to be nothing, and a column of boxes there would be worse than a gap.
 * They keep the old space-width blank. */
static int is_blank_cp(int cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
}

/* Box geometry, derived from the face so every size gets a proportionate one:
 * cap height is ~3/4 of the ascent, and the classic .notdef is ~2:3. */
static int notdef_h(const atlas_t *a) {
    int h = (a->ascent * 3) / 4;
    return h < 4 ? 4 : h;
}

static int notdef_w(const atlas_t *a) {
    int w = (notdef_h(a) * 2) / 3;
    return w < 3 ? 3 : w;
}

/* Pen advance for the box, incl. a 1px side bearing either side, in 26.6 like
 * every other advance. text_width and text_draw_c MUST agree on this or the
 * returned pen drifts from the measure. */
static int notdef_advance(const atlas_t *a) {
    return (notdef_w(a) + 2) << ATLAS_ADV_SHIFT;
}

/*
 * Kerning adjustment between two glyph indices, in 26.6. Binary search over
 * the atlas's sorted pair table (<=11 probes for ~1500 pairs), keyed on
 * (left<<8)|right. Returns 0 when either side has no glyph or the face ships
 * no table.
 *
 * MUST be applied identically by text_width() and text_draw_c() — they are
 * contractually required to return the same pen for the same string, and the
 * marquee chains on that value.
 */
static int kern_adv(const atlas_t *a, int prev_gi, int gi) {
    if (prev_gi < 0 || gi < 0 || a->kern_n == 0 ||
        prev_gi > 0xFF || gi > 0xFF) {
        return 0;
    }
    unsigned key = ((unsigned)prev_gi << 8) | (unsigned)gi;
    int lo = 0, hi = (int)a->kern_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const atlas_kern_t *k = &a->kern[mid];
        unsigned kk = ((unsigned)k->left << 8) | (unsigned)k->right;
        if (kk == key) {
            return ATLAS_KERN_TO_ADV(k->adj);
        }
        if (kk < key) lo = mid + 1;
        else          hi = mid - 1;
    }
    return 0;
}

/*
 * Glyph 0 is U+0020: the atlas bakes PRINTABLE = 0x20..0x7E in order, so index
 * 0 is the space by construction (tools/atlas_gen.py).
 */
static int glyph_is_space(int gi) { return gi == 0; }

/*
 * Tracking to insert between `prev_gi` and `gi`, in 26.6.
 *
 * A space counts as ONE tracking unit, not zero and not two, and that middle
 * course is the whole point. Getting it wrong breaks word spacing in opposite
 * directions:
 *
 *   - track both sides (the naive reading of "letter-spacing"): every word gap
 *     widens by 2x the tracking. At 9px that is +1.8px on a 2.3px space, a 78%
 *     wider word gap, and the text reads as deliberately letterspaced.
 *   - exempt the space entirely (what this did first, reasoning that a space
 *     has no ink for the AA fringe to eat): letter gaps grow while the word gap
 *     stays put, so words visually MERGE. Measured at bold-11: 1.59px between
 *     letters against a 2.98px space — "Now Playing" read as one word.
 *
 * Applying it on entry to the space but not on exit moves the word gap by
 * exactly the same amount as every letter gap, so the ratio the face was
 * designed with survives.
 */
static int track_adv(const atlas_t *a, int prev_gi, int gi) {
    if (prev_gi < 0 || gi < 0) return 0;          /* string edge / no glyph */
    if (glyph_is_space(prev_gi)) return 0;        /* ...but not on the way out */
    return a->tracking;
}

/* Advance for a codepoint with no atlas glyph (blank or box). */
static int fallback_advance(const atlas_t *a, int cp) {
    return is_blank_cp(cp) ? a->glyphs[0].advance : notdef_advance(a);
}

/* ---------- Measurement / metrics ---------------------------------- */

int text_width(const char *s, const text_font_t *font) {
    if (!font || !s) return 0;
    const atlas_t *a = font->a;
    const unsigned char *p = (const unsigned char *)s;
    /*
     * Accumulate in 26.6 and round ONCE at the end, so a string's width is the
     * rounded true width rather than the sum of per-glyph roundings. Rounding
     * per glyph is what used to drift a title by 1-2px against its own
     * centring.
     */
    int w = 0, cp, prev_gi = -1;
    while ((cp = utf8_next(&p)) >= 0) {
        int gi = glyph_index(cp);
        if (gi < 0) {
            w += fallback_advance(a, cp);
            prev_gi = -1;              /* no glyph: nothing to kern against */
            continue;
        }
        w += track_adv(a, prev_gi, gi);
        w += kern_adv(a, prev_gi, gi);
        w += a->glyphs[gi].advance;
        prev_gi = gi;
    }
    return ATLAS_ADV_PX(w);
}

int text_line_height(const text_font_t *font) {
    if (!font) return 0;
    return font->a->line_height;
}

int text_descent(const text_font_t *font) {
    if (!font) return 0;
    return font->a->descent;
}

int text_ascent(const text_font_t *font) {
    if (!font) return 0;
    return font->a->ascent;
}

/* ---------- Rendering ---------------------------------------------- */

/* Paint the .notdef box for one unrenderable codepoint: a 1px hollow rectangle
 * sitting on the baseline, drawn in solid ink (no coverage data, so no atlas
 * read at all — this path cannot go out of bounds in the glyph data). Every
 * write is clamped to the clip window, which the caller has already clamped to
 * the framebuffer. */
static void draw_notdef(uint16_t *fb, int fb_w, int x, int y, const atlas_t *a,
                        uint16_t ink, int cx0, int cx1, int cy0, int cy1) {
    int bw = notdef_w(a), bh = notdef_h(a);
    int bx = x + 1, by = y - bh;              /* 1px left bearing, on baseline */

    int i0 = cx0 - bx; if (i0 < 0) i0 = 0;
    int i1 = cx1 - bx; if (i1 > bw) i1 = bw;
    int j0 = cy0 - by; if (j0 < 0) j0 = 0;
    int j1 = cy1 - by; if (j1 > bh) j1 = bh;

    for (int j = j0; j < j1; j++) {
        uint16_t *px = fb + (long)(by + j) * fb_w + (bx + i0);
        int edge_row = (j == 0 || j == bh - 1);
        for (int i = i0; i < i1; i++, px++) {
            if (edge_row || i == 0 || i == bw - 1) {
                *px = ink;
            }
        }
    }
}

static int text_draw_c(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                       const char *s, const text_font_t *font, uint16_t ink,
                       int cx0, int cx1, int cy0, int cy1) {
    if (!fb || !font || !s || fb_w <= 0 || fb_h <= 0) {
        return x;
    }
    /* Clamp the clip window to the framebuffer ONCE. Everything below derives
     * its loop bounds from these, so no per-pixel bounds test is needed. */
    if (cx0 < 0) cx0 = 0;
    if (cx1 > fb_w) cx1 = fb_w;
    if (cy0 < 0) cy0 = 0;
    if (cy1 > fb_h) cy1 = fb_h;
    const atlas_t *a = font->a;

    /* Ink in raw RGB565 bit-fields, pre-decoded to linear light. */
    uint8_t ir5 = (uint8_t)((ink >> 11) & 0x1F);
    uint8_t ig6 = (uint8_t)((ink >> 5)  & 0x3F);
    uint8_t ib5 = (uint8_t)( ink        & 0x1F);
    uint8_t ir_lin = srgb5_to_linear[ir5];
    uint8_t ig_lin = srgb6_to_linear[ig6];
    uint8_t ib_lin = srgb5_to_linear[ib5];

    const unsigned char *p = (const unsigned char *)s;
    /*
     * The pen runs in 26.6 (`pen`) relative to the caller's integer `x`, and
     * each glyph is placed at the pen ROUNDED to a whole pixel. Blitting is
     * still pixel-aligned — this is not subpixel positioning — but the
     * fractional part carries forward instead of being thrown away per glyph,
     * which is what made adjacent gaps in one word differ by ~1px.
     *
     * `x0` is kept so the return value is the caller's x plus the rounded
     * total, matching text_width() exactly. Callers chain on that.
     */
    const int x0 = x;
    int pen = 0;
    int cp, prev_gi = -1;
    while ((cp = utf8_next(&p)) >= 0) {
        int gi = glyph_index(cp);
        /* Kern BEFORE the pen is rounded into a position, so the pair's
         * adjustment can actually move this glyph. */
        if (gi >= 0) {
            pen += track_adv(a, prev_gi, gi);
            pen += kern_adv(a, prev_gi, gi);
        }
        prev_gi = gi;                  /* gi < 0 => nothing to kern against */
        x = x0 + ATLAS_ADV_PX(pen);
        if (gi < 0) {
            if (!is_blank_cp(cp)) {
                draw_notdef(fb, fb_w, x, y, a, ink, cx0, cx1, cy0, cy1);
            }
            pen += fallback_advance(a, cp);
            continue;
        }
        const atlas_glyph_t *gly = &a->glyphs[gi];

        /* offset_y is "px below the ascender line" (PIL bbox convention).
         * Convert to framebuffer y: from the baseline, walk up by ascent
         * to the ascender, then down by offset_y to this glyph's top. */
        int gw = gly->w, gh = gly->h;
        int gx = x + gly->offset_x;
        int gy = y - a->ascent + gly->offset_y;

        /* Whole-glyph reject against the clip window. The marquee tick runs
         * 30x/s over a title several times wider than its window, so most
         * glyphs of most ticks are entirely off one side; rasterizing them
         * was ~half the work per tick. Left of the window: advance and move
         * on. At or right of it: nothing further can land, so stop drawing —
         * but keep summing advances, since callers chain on the returned pen. */
        if (gx + gw <= cx0) {
            pen += gly->advance;
            continue;
        }
        if (gx >= cx1) {
            pen += gly->advance;
            /* Nothing further can land in the window, but the returned pen is
             * the caller's contract, so keep accumulating — kerning included,
             * or this tail would disagree with text_width(). */
            while ((cp = utf8_next(&p)) >= 0) {
                int gj = glyph_index(cp);
                if (gj < 0) {
                    pen += fallback_advance(a, cp);
                    prev_gi = -1;
                    continue;
                }
                pen += track_adv(a, prev_gi, gj);
                pen += kern_adv(a, prev_gi, gj);
                pen += a->glyphs[gj].advance;
                prev_gi = gj;
            }
            break;
        }

        /* Clamp the glyph's row/column ranges to the clip window once, so the
         * inner loop is a straight run with no per-pixel branch, and hoist both
         * row pointers out of it (-Os will not strength-reduce these). */
        int i0 = cx0 - gx; if (i0 < 0) i0 = 0;
        int i1 = cx1 - gx; if (i1 > gw) i1 = gw;
        int j0 = cy0 - gy; if (j0 < 0) j0 = 0;
        int j1 = cy1 - gy; if (j1 > gh) j1 = gh;
        if (i0 >= i1 || j0 >= j1) {
            pen += gly->advance;
            continue;
        }

        const uint8_t *srow = a->data + gly->data_offset + (long)j0 * gw + i0;
        uint16_t *drow = fb + (long)(gy + j0) * fb_w + (gx + i0);
        int run = i1 - i0;

        for (int j = j0; j < j1; j++, srow += gw, drow += fb_w) {
            for (int i = 0; i < run; i++) {
                uint8_t alpha = srow[i];
                if (alpha == 0) continue;

                uint16_t *dst = &drow[i];

                if (alpha == 255) {
                    *dst = ink;
                    continue;
                }

                uint16_t bg = *dst;
                uint8_t br_lin = srgb5_to_linear[(bg >> 11) & 0x1F];
                uint8_t bg_lin = srgb6_to_linear[(bg >> 5)  & 0x3F];
                uint8_t bb_lin = srgb5_to_linear[ bg        & 0x1F];

                /* Exact floor(x/255) for x in [0,65025] via add-shift — avoids
                 * three soft-divides (no HW divide on ARM7) per blended pixel,
                 * the hottest UI inner loop. Bit-identical to `/255`. */
                uint8_t inv = (uint8_t)(255 - alpha);
                unsigned xr = (unsigned)ir_lin * alpha + (unsigned)br_lin * inv;
                unsigned xg = (unsigned)ig_lin * alpha + (unsigned)bg_lin * inv;
                unsigned xb = (unsigned)ib_lin * alpha + (unsigned)bb_lin * inv;
                uint8_t r_lin = (uint8_t)((xr + 1 + (xr >> 8)) >> 8);
                uint8_t g_lin = (uint8_t)((xg + 1 + (xg >> 8)) >> 8);
                uint8_t b_lin = (uint8_t)((xb + 1 + (xb >> 8)) >> 8);

                uint8_t r5 = linear_to_srgb5[r_lin];
                uint8_t g6 = linear_to_srgb6[g_lin];
                uint8_t b5 = linear_to_srgb5[b_lin];
                *dst = (uint16_t)(((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | (uint16_t)b5);
            }
        }
        pen += gly->advance;
    }
    return x0 + ATLAS_ADV_PX(pen);
}

int text_draw(uint16_t *fb, int fb_w, int fb_h, int x, int y,
              const char *s, const text_font_t *font, uint16_t ink) {
    return text_draw_c(fb, fb_w, fb_h, x, y, s, font, ink, 0, fb_w, 0, fb_h);
}

/* Like text_draw but writes only pixels with clip_x0 <= px < clip_x1 — the
 * horizontal window a marquee scrolls its text through. */
int text_draw_clip(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                   const char *s, const text_font_t *font, uint16_t ink,
                   int clip_x0, int clip_x1) {
    if (clip_x0 < 0)     clip_x0 = 0;
    if (clip_x1 > fb_w)  clip_x1 = fb_w;
    return text_draw_c(fb, fb_w, fb_h, x, y, s, font, ink, clip_x0, clip_x1, 0, fb_h);
}

/* text_draw_clip with an additional VERTICAL window [clip_y0, clip_y1) — used by
 * the marquee to keep a scrolling title inside its list row (so tall glyph tops
 * can't paint above the selection bar into the row above). */
int text_draw_clip_v(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                     const char *s, const text_font_t *font, uint16_t ink,
                     int clip_x0, int clip_x1, int clip_y0, int clip_y1) {
    if (clip_x0 < 0)     clip_x0 = 0;
    if (clip_x1 > fb_w)  clip_x1 = fb_w;
    return text_draw_c(fb, fb_w, fb_h, x, y, s, font, ink,
                       clip_x0, clip_x1, clip_y0, clip_y1);
}
