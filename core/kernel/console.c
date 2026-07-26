/*
 * core/kernel/console.c — minimal on-screen text/hex console.
 *
 * Portable freestanding logic: an 8x8 bitmap font rasterized into a static
 * RGB565 back buffer. No hardware access. See console.h for the contract.
 *
 * Font layout: each glyph is 8 bytes, one byte per pixel row (top to
 * bottom). Within a byte the MSB (bit 7) is the leftmost pixel. A set bit
 * paints the foreground color; a clear bit paints the background color.
 */

#include "console.h"
#include "hal.h"        /* LCD_WIDTH, LCD_HEIGHT */

#include <stdint.h>

/* 320*240 RGB565 = 153,600 bytes, lives in .bss. 4-byte aligned so the fill
 * paths can write PAIRS of pixels as one 32-bit store (see fill_words). */
static uint16_t g_fb[LCD_WIDTH * LCD_HEIGHT] __attribute__((aligned(4)));

/* ---------------------------------------------------------------------------
 * Damage rect: the union of everything drawn since the last reset, as the
 * half-open box [x0,x1) x [y0,y1). Empty when x0 >= x1. See console.h.
 * ------------------------------------------------------------------------- */
static int g_dmg_x0 = LCD_WIDTH, g_dmg_y0 = LCD_HEIGHT, g_dmg_x1, g_dmg_y1;

/* Union an ALREADY-CLAMPED box into the damage rect. */
static void damage_box(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    if (x0 < g_dmg_x0) g_dmg_x0 = x0;
    if (y0 < g_dmg_y0) g_dmg_y0 = y0;
    if (x1 > g_dmg_x1) g_dmg_x1 = x1;
    if (y1 > g_dmg_y1) g_dmg_y1 = y1;
}

void console_damage_reset(void)
{
    g_dmg_x0 = LCD_WIDTH;
    g_dmg_y0 = LCD_HEIGHT;
    g_dmg_x1 = 0;
    g_dmg_y1 = 0;
}

void console_damage_add(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > LCD_WIDTH)  x1 = LCD_WIDTH;
    if (y1 > LCD_HEIGHT) y1 = LCD_HEIGHT;
    damage_box(x0, y0, x1, y1);
}

int console_damage_get(int *x, int *y, int *w, int *h)
{
    if (g_dmg_x1 <= g_dmg_x0 || g_dmg_y1 <= g_dmg_y0) {
        return 0;
    }
    if (x) *x = g_dmg_x0;
    if (y) *y = g_dmg_y0;
    if (w) *w = g_dmg_x1 - g_dmg_x0;
    if (h) *h = g_dmg_y1 - g_dmg_y0;
    return 1;
}

/* Fill `n` pixels at `dst` with `rgb565`, writing 32-bit word pairs over the
 * aligned run (the panel bus + the CPU both prefer words; lcd_fill already does
 * this on its side). Handles an odd leading/trailing pixel. */
static void fill_words(uint16_t *dst, int n, uint16_t rgb565)
{
    if (n <= 0) {
        return;
    }
    if (((uintptr_t)dst & 2u) != 0) {          /* odd start: one 16-bit store */
        *dst++ = rgb565;
        n--;
    }
    uint32_t  pair = ((uint32_t)rgb565 << 16) | rgb565;
    uint32_t *w    = (uint32_t *)(void *)dst;
    int       nw   = n >> 1;
    for (int i = 0; i < nw; i++) {
        w[i] = pair;
    }
    if (n & 1) {                                /* odd tail */
        dst[n - 1] = rgb565;
    }
}

#define GLYPH_W 8
#define GLYPH_H 8

/* Character grid dimensions derived from the panel + font size. */
#define CON_COLS (LCD_WIDTH  / GLYPH_W)   /* 40 */
#define CON_ROWS (LCD_HEIGHT / GLYPH_H)   /* 30 */

/* One 8x8 glyph. */
typedef struct {
    char          ch;
    uint8_t       rows[GLYPH_H];
} glyph_t;

/* Hand-authored 5x7-in-8x8 font. Bit 7 = leftmost pixel. */
static const glyph_t g_font[] = {
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },

    { '0', { 0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70, 0x00 } },
    { '1', { 0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00 } },
    { '2', { 0x70, 0x88, 0x10, 0x20, 0x40, 0x80, 0xF8, 0x00 } },
    { '3', { 0x70, 0x88, 0x08, 0x30, 0x08, 0x88, 0x70, 0x00 } },
    { '4', { 0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10, 0x00 } },
    { '5', { 0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70, 0x00 } },
    { '6', { 0x70, 0x80, 0x80, 0xF0, 0x88, 0x88, 0x70, 0x00 } },
    { '7', { 0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40, 0x00 } },
    { '8', { 0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70, 0x00 } },
    { '9', { 0x70, 0x88, 0x88, 0x78, 0x08, 0x08, 0x70, 0x00 } },

    { 'A', { 0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00 } },
    { 'B', { 0xF0, 0x88, 0x88, 0xF0, 0x88, 0x88, 0xF0, 0x00 } },
    { 'C', { 0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70, 0x00 } },
    { 'D', { 0xF0, 0x88, 0x88, 0x88, 0x88, 0x88, 0xF0, 0x00 } },
    { 'E', { 0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0xF8, 0x00 } },
    { 'F', { 0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0x80, 0x00 } },

    /* Label letters. */
    { 'G', { 0x70, 0x88, 0x80, 0xB8, 0x88, 0x88, 0x70, 0x00 } },
    { 'H', { 0x88, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00 } },
    { 'I', { 0x70, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00 } },
    { 'J', { 0x38, 0x10, 0x10, 0x10, 0x10, 0x90, 0x60, 0x00 } },
    { 'K', { 0x88, 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x88, 0x00 } },
    { 'L', { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8, 0x00 } },
    { 'M', { 0x88, 0xD8, 0xA8, 0xA8, 0x88, 0x88, 0x88, 0x00 } },
    { 'N', { 0x88, 0xC8, 0xC8, 0xA8, 0x98, 0x98, 0x88, 0x00 } },
    { 'O', { 0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00 } },
    { 'P', { 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x80, 0x00 } },
    { 'Q', { 0x70, 0x88, 0x88, 0x88, 0xA8, 0x90, 0x68, 0x00 } },
    { 'R', { 0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x88, 0x00 } },
    { 'S', { 0x70, 0x88, 0x80, 0x70, 0x08, 0x88, 0x70, 0x00 } },
    { 'T', { 0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00 } },
    { 'U', { 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00 } },
    { 'V', { 0x88, 0x88, 0x88, 0x88, 0x50, 0x50, 0x20, 0x00 } },
    { 'W', { 0x88, 0x88, 0x88, 0xA8, 0xA8, 0xA8, 0x50, 0x00 } },
    { 'X', { 0x88, 0x88, 0x50, 0x20, 0x50, 0x88, 0x88, 0x00 } },
    { 'Y', { 0x88, 0x88, 0x50, 0x20, 0x20, 0x20, 0x20, 0x00 } },
    { 'Z', { 0xF8, 0x08, 0x10, 0x20, 0x40, 0x80, 0xF8, 0x00 } },

    { '=', { 0x00, 0x00, 0xF8, 0x00, 0xF8, 0x00, 0x00, 0x00 } },
    { '-', { 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00 } },

    /* Filename punctuation (for the file browser's name column) + the
     * MM:SS clock separator on the now-playing screen. */
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x00 } },
    { '_', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8 } },
    { ':', { 0x00, 0x60, 0x60, 0x00, 0x60, 0x60, 0x00, 0x00 } },
    { '>', { 0x80, 0x40, 0x20, 0x10, 0x20, 0x40, 0x80, 0x00 } },
};

#define FONT_COUNT ((int)(sizeof g_font / sizeof g_font[0]))

/* Return the 8-byte glyph rows for ch, or the blank glyph (index 0, space)
 * if unsupported. Never NULL: g_font[0] is guaranteed to be the blank. */
static const uint8_t *glyph_for(char ch)
{
    int i;
    for (i = 0; i < FONT_COUNT; i++) {
        if (g_font[i].ch == ch) {
            return g_font[i].rows;
        }
    }
    return g_font[0].rows;   /* blank cell */
}

const uint16_t *console_framebuffer(void)
{
    return g_fb;
}

void console_clear(uint16_t rgb565)
{
    fill_words(g_fb, LCD_WIDTH * LCD_HEIGHT, rgb565);
    damage_box(0, 0, LCD_WIDTH, LCD_HEIGHT);   /* a clear damages everything */
}

uint16_t *console_fb(void)
{
    return g_fb;
}

/* Clamp the rect to the panel ONCE, then run tight inner loops (no per-pixel
 * bounds branch). Identical output for the in-bounds pixels. */
void console_fill_rect(int x, int y, int w, int h, uint16_t rgb565)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > LCD_WIDTH)  x1 = LCD_WIDTH;
    if (y1 > LCD_HEIGHT) y1 = LCD_HEIGHT;
    for (int py = y0; py < y1; py++) {
        fill_words(&g_fb[py * LCD_WIDTH + x0], x1 - x0, rgb565);
    }
    damage_box(x0, y0, x1, y1);
}

void console_blit565(int x, int y, int w, int h, const uint16_t *src)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > LCD_WIDTH)  x1 = LCD_WIDTH;
    if (y1 > LCD_HEIGHT) y1 = LCD_HEIGHT;
    for (int py = y0; py < y1; py++) {
        const uint16_t *srow = &src[(py - y) * w];
        uint16_t       *drow = &g_fb[py * LCD_WIDTH];
        for (int px = x0; px < x1; px++) {
            drow[px] = srow[px - x];
        }
    }
    damage_box(x0, y0, x1, y1);
}

void console_char(int col, int row, char ch, uint16_t fg, uint16_t bg)
{
    const uint8_t *glyph;
    int base_x, base_y, ry, cx;

    if (col < 0 || col >= CON_COLS || row < 0 || row >= CON_ROWS) {
        return;   /* out of range: no-op */
    }

    glyph  = glyph_for(ch);
    base_x = col * GLYPH_W;
    base_y = row * GLYPH_H;

    for (ry = 0; ry < GLYPH_H; ry++) {
        uint8_t bits = glyph[ry];
        for (cx = 0; cx < GLYPH_W; cx++) {
            int set = (bits >> (7 - cx)) & 1;
            g_fb[(base_y + ry) * LCD_WIDTH + (base_x + cx)] = set ? fg : bg;
        }
    }
    damage_box(base_x, base_y, base_x + GLYPH_W, base_y + GLYPH_H);
}

void console_str(int col, int row, const char *s, uint16_t fg, uint16_t bg)
{
    if (s == 0) {
        return;
    }
    for (; *s != '\0' && col < CON_COLS; s++, col++) {
        console_char(col, row, *s, fg, bg);
    }
}

void console_hex32(int col, int row, uint32_t value, uint16_t fg, uint16_t bg)
{
    static const char hexdigits[] = "0123456789ABCDEF";
    int i;

    for (i = 0; i < 8; i++) {
        unsigned nibble = (value >> (28 - 4 * i)) & 0xFu;
        console_char(col + i, row, hexdigits[nibble], fg, bg);
    }
}
