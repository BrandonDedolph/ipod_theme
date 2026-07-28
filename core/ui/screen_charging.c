/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/screen_charging.c — full-screen "charging" battery view.
 *
 * Draws design_reference/system-screens.jsx ChargingScreen with the same
 * primitives the menu UI uses (console_fill_rect for shapes, the Nunito
 * text renderer for type). Pure integer math into the RGB565 console
 * framebuffer; no hardware access, no present (see screen_charging.h).
 *
 * Layout (320x240): a 150x68 battery outline centred horizontally in the
 * upper-middle with a nub on the right and a pct-proportional inner fill;
 * a lightning bolt cut into the fill (in the background colour) while
 * charging; a big percent number (bold 17) centred below with a smaller
 * muted "%"; then a one-line status in bold 11.
 */

#include "screen_charging.h"

#include "text.h"
#include "../kernel/console.h"
#include "../hal/hal.h"          /* LCD_WIDTH / LCD_HEIGHT */

/* ---------------------------------------------------------------------------
 * Palette (system-screens.jsx ChargingScreen tokens -> RGB565)
 * ------------------------------------------------------------------------- */
#define CHG_BG      0x0861u      /* #0e0d0c near-black background            */
#define CHG_OUTLINE 0x5A89u      /* #5a5048 battery outline + nub            */
#define CHG_FILL    0xEF3Bu      /* #e8e4dd light fill (normal, not low)     */
#define CHG_GREEN   0x3E4Du      /* charging fill (oklch(0.78 0.16 145))     */
#define CHG_RED     0xDA46u      /* low-battery fill (oklch(0.65 0.18 30))   */
#define CHG_TEXT    0xEF3Bu      /* #e8e4dd big percent digits               */
#define CHG_UNIT    0xACF2u      /* #a89e92 muted "%" unit                   */
/* At or above this, a stopped charger means "full", not "failed". */
#define CHG_FULL_PCT 90

#define CHG_MUTED   0x7B8Du      /* #7a736a muted status / "not charging"    */

/* Battery geometry. Centred horizontally: body 150 wide -> the panel centre
 * (160) is the battery centre, so the lightning bolt lands dead-centre. */
#define BATT_W   150             /* outline width                            */
#define BATT_H    68             /* outline height                           */
#define BATT_X    ((LCD_WIDTH - BATT_W) / 2)   /* 85                          */
#define BATT_Y    56             /* outline top (upper-middle)               */
#define BATT_T     3             /* outline stroke thickness                 */
#define BATT_INSET 8             /* gap from outline to the fill             */
#define NUB_W      6
#define NUB_H     24

#define PCT_BASE  168            /* big percent baseline                     */
#define STAT_BASE 196            /* status line baseline                     */

/* Clamp a raw charge reading to a drawable 0..100 percent. */
static int clamp_pct(int pct)
{
    if (pct < 0)   return 0;
    if (pct > 100) return 100;
    return pct;
}

/* Inner fill width for `pct` over an interior of `inner_w` pixels. Mirrors the
 * design's Math.max(8, ...): always show a small stub so an empty/near-empty
 * pack still reads as a battery rather than a bare outline. */
static int fill_width(int pct, int inner_w)
{
    int fw = (inner_w * clamp_pct(pct)) / 100;
    if (fw < 8) fw = 8;
    if (fw > inner_w) fw = inner_w;
    return fw;
}

/* Render a small unsigned value (0..999) as decimal into `dst`; returns the
 * digit count. `dst` needs >= 4 bytes. */
static int u_to_str(char *dst, int v)
{
    if (v < 0) v = 0;
    char tmp[4];
    int t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 3);
    int i = 0;
    while (t > 0) dst[i++] = tmp[--t];
    dst[i] = '\0';
    return i;
}

/* Draw a NUL-terminated string; thin wrapper over text_draw with the panel
 * dimensions baked in (mirrors kernel/main.c's ui_text). `y` is the baseline. */
static int chg_text(int x, int y, const char *s, const text_font_t *font,
                    uint16_t ink)
{
    return text_draw(console_fb(), LCD_WIDTH, LCD_HEIGHT, x, y, s, font, ink);
}

/* Rectangular battery outline (four strokes) with the outer corner pixels
 * knocked back to the background — a cheap hint of rounded corners. */
static void draw_battery_outline(int x, int y, int w, int h, int t, uint16_t c)
{
    console_fill_rect(x,             y,             w, t, c);   /* top    */
    console_fill_rect(x,             y + h - t,     w, t, c);   /* bottom */
    console_fill_rect(x,             y,             t, h, c);   /* left   */
    console_fill_rect(x + w - t,     y,             t, h, c);   /* right  */
    /* Soften the four outer corners. */
    console_fill_rect(x,             y,             1, 1, CHG_BG);
    console_fill_rect(x + w - 1,     y,             1, 1, CHG_BG);
    console_fill_rect(x,             y + h - 1,     1, 1, CHG_BG);
    console_fill_rect(x + w - 1,     y + h - 1,     1, 1, CHG_BG);
}

/* Lightning bolt cut into the fill (drawn in the background colour, as the
 * design does). It's the classic "flash" glyph: a solid angular polygon that
 * falls from a heavy top bar diagonally down-left, jogs right at the waist,
 * then tapers to a point at the bottom — an unmistakable "⚡".
 *
 * The outline is a fixed 7-vertex shape defined in a 10x20 design box (Material
 * flash proportions), scaled uniformly to `bh` tall and centred on `cx`, then
 * filled row by row with an even-odd scanline span test — integer edge-walking
 * only, since there's no polygon primitive to lean on. */
static void draw_bolt(int cx, int y0, int bh, uint16_t c)
{
    /* Bolt outline in the 10x20 design box, walked in order. */
    static const int px[7] = { 0, 0, 3, 3, 10, 6, 10 };
    static const int py[7] = { 0, 11, 11, 20, 8,  8, 0  };
    const int N = 7, box_w = 10, box_h = 20;

    /* Uniform scale off the height; keep the aspect ratio. */
    int bw = (box_w * bh) / box_h;
    int x0 = cx - bw / 2;

    /* Vertices in pixel space. */
    int sx[7], sy[7];
    for (int i = 0; i < N; i++) {
        sx[i] = x0 + (px[i] * bw) / box_w;
        sy[i] = y0 + (py[i] * bh) / box_h;
    }

    for (int y = y0; y < y0 + bh; y++) {
        /* Collect the x where each non-horizontal edge crosses this scanline,
         * using a half-open [ylo, yhi) rule so shared vertices count once. */
        int xs[8];
        int n = 0;
        for (int i = 0; i < N; i++) {
            int a = i, b = (i + 1) % N;
            int ya = sy[a], yb = sy[b], xa = sx[a], xb = sx[b];
            if (ya == yb) continue;                 /* horizontal edge */
            int ylo, yhi, xlo, xhi;
            if (ya < yb) { ylo = ya; yhi = yb; xlo = xa; xhi = xb; }
            else         { ylo = yb; yhi = ya; xlo = xb; xhi = xa; }
            if (y < ylo || y >= yhi) continue;
            xs[n++] = xlo + (xhi - xlo) * (y - ylo) / (yhi - ylo);
        }
        /* Insertion sort the (few) crossings, then fill between pairs. */
        for (int i = 1; i < n; i++) {
            int v = xs[i], j = i - 1;
            while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = v;
        }
        for (int k = 0; k + 1 < n; k += 2) {
            int L = xs[k], R = xs[k + 1];
            if (R > L) console_fill_rect(L, y, R - L, 1, c);
        }
    }
}

void screen_charging_render(int pct, int charging, int external)
{
    pct = clamp_pct(pct);

    console_clear(CHG_BG);

    /* Fill colour: green while charging, red when low (and not charging),
     * else the light "full" tone (system-screens.jsx). */
    uint16_t fill = charging ? CHG_GREEN : (pct < 20 ? CHG_RED : CHG_FILL);

    draw_battery_outline(BATT_X, BATT_Y, BATT_W, BATT_H, BATT_T, CHG_OUTLINE);

    /* Terminal nub on the right, vertically centred. */
    console_fill_rect(BATT_X + BATT_W, BATT_Y + (BATT_H - NUB_H) / 2,
                      NUB_W, NUB_H, CHG_OUTLINE);

    /* Proportional inner fill. */
    int ix = BATT_X + BATT_INSET;
    int iy = BATT_Y + BATT_INSET;
    int iw = BATT_W - 2 * BATT_INSET;
    int ih = BATT_H - 2 * BATT_INSET;
    int fw = fill_width(pct, iw);
    console_fill_rect(ix, iy, fw, ih, fill);

    /* Lightning bolt cut into the fill while charging. */
    if (charging) {
        draw_bolt(LCD_WIDTH / 2, iy + 4, ih - 8, CHG_BG);
    }

    /* Big percent number, centred, with a smaller muted "%". */
    char num[4];
    u_to_str(num, pct);
    const text_font_t *big  = text_font_bold_17();
    const text_font_t *unit = text_font_bold_13();
    int wn = text_width(num, big);
    int wu = text_width("%", unit);
    int total = wn + 2 + wu;
    int nx = (LCD_WIDTH - total) / 2;
    chg_text(nx, PCT_BASE, num, big, CHG_TEXT);
    chg_text(nx + wn + 2, PCT_BASE, "%", unit, CHG_UNIT);

    /* Status line: honest, no fake time estimate. */
    const char *status;
    uint16_t status_ink;
    if (charging) {
        status = "CHARGING";
        status_ink = CHG_GREEN;
    } else if (!external) {
        status = "CONNECT CABLE";
        status_ink = CHG_MUTED;
    } else if (pct >= CHG_FULL_PCT) {
        /*
         * Cable in, charger has STOPPED, battery high: that is the LTC4066
         * terminating a completed charge, not a fault. It manages CV taper and
         * end-of-charge itself (06-power.md, "Charge current control"), so this
         * is the normal, healthy end state — reporting it as "NOT CHARGING"
         * read like something had gone wrong.
         *
         * Threshold not a precise measurement: battery.h warns the percent
         * curve is uncalibrated and cosmetic until checked against a real (and
         * usually replacement) cell. It only has to separate "topped off" from
         * "plugged in but not charging", which is a wide gap.
         */
        status = "CHARGED";
        status_ink = CHG_GREEN;
    } else {
        status = "NOT CHARGING";
        status_ink = CHG_MUTED;
    }
    const text_font_t *sfont = text_font_bold_11();
    int ws = text_width(status, sfont);
    chg_text((LCD_WIDTH - ws) / 2, STAT_BASE, status, sfont, status_ink);
}
