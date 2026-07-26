/*
 * core/kernel/console.h — minimal on-screen text/hex console.
 *
 * Portable, freestanding logic only: renders an 8x8 bitmap font into an
 * RGB565 back buffer. No hardware access — hand the framebuffer to
 * lcd_present() (a separate module) to actually light the panel.
 *
 * The panel is LCD_WIDTH x LCD_HEIGHT RGB565 (see hal/hal.h). With an
 * 8x8-pixel font that is a 40x30 character grid.
 */

#ifndef CORE_KERNEL_CONSOLE_H
#define CORE_KERNEL_CONSOLE_H

#include <stdint.h>

/* RGB565 color helpers. */
#define CON_BLACK  0x0000u
#define CON_WHITE  0xFFFFu
#define CON_RED    0xF800u
#define CON_GREEN  0x07E0u
#define CON_BLUE   0x001Fu
#define CON_YELLOW 0xFFE0u
#define CON_CYAN   0x07FFu

/* The back buffer (LCD_WIDTH*LCD_HEIGHT RGB565), for handing to lcd_present(). */
const uint16_t *console_framebuffer(void);

/* Fill the whole framebuffer with one color. */
void console_clear(uint16_t rgb565);

/* Blit a w*h block of RGB565 pixels (row-major) into the framebuffer at pixel
 * (x,y), clipped to the panel. Used for pre-scaled album art. */
void console_blit565(int x, int y, int w, int h, const uint16_t *src);

/* Writable framebuffer pointer (LCD_WIDTH*LCD_HEIGHT RGB565) for drawing that
 * doesn't go through the console glyph path — e.g. the Nunito text renderer. */
uint16_t *console_fb(void);

/* Fill a w*h rectangle with one RGB565 colour, clipped to the panel. */
void console_fill_rect(int x, int y, int w, int h, uint16_t rgb565);

/* Draw one character at character-cell (col,row) [col 0..39, row 0..29]
 * with fg/bg colors. Supported glyphs: '0'-'9', 'A'-'F' (hex), space,
 * and the uppercase letters used for labels: at minimum
 * F R E Q P L S T C U N G I O W M = - (any unsupported char draws as a
 * blank/space cell). Out-of-range
 * col/row is a no-op (bounds-checked). */
void console_char(int col, int row, char ch, uint16_t fg, uint16_t bg);

/* Draw a NUL-terminated string starting at (col,row), advancing right;
 * does not wrap (stops at col 40). */
void console_str(int col, int row, const char *s, uint16_t fg, uint16_t bg);

/* Draw `value` as 8 uppercase hex digits at (col,row). */
void console_hex32(int col, int row, uint32_t value, uint16_t fg, uint16_t bg);

/* ---------------------------------------------------------------------------
 * Damage tracking
 *
 * Presenting the framebuffer streams every pixel to the panel with IRQs masked,
 * which is ruinously expensive for a repaint that only moved a selection bar one
 * row. So the console keeps a running DAMAGE RECT: the union of every region
 * written since the last console_damage_reset(). A caller can then present only
 * that region (lcd_present_rect) instead of the whole frame.
 *
 * console_clear() damages the whole panel, so a full repaint still presents in
 * full — the optimisation only pays off where the caller redraws selectively.
 *
 * Drawing that bypasses this module (anything writing through console_fb(), e.g.
 * the Nunito text renderer) is INVISIBLE to the tracker and must report itself
 * with console_damage_add(); otherwise a damage-only present drops those pixels.
 * ------------------------------------------------------------------------- */

/* Forget all accumulated damage (the rect becomes empty). */
void console_damage_reset(void);

/* Union a w*h rectangle at (x,y) into the damage rect, clipped to the panel.
 * Non-positive w/h is a no-op. */
void console_damage_add(int x, int y, int w, int h);

/* Read the damage rect through the x/y/w/h out-pointers (any may be NULL;
 * they're written only when the rect is non-empty). Returns 1 when
 * something is damaged, 0 when the rect is empty (nothing to present). */
int console_damage_get(int *x, int *y, int *w, int *h);

#endif /* CORE_KERNEL_CONSOLE_H */
