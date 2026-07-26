/*
 * core/hal/hw/lcd.h — BCM video coprocessor LCD driver (hw target only).
 *
 * Phase 1 surface: init + solid-color present. The full hal.h contract
 * (lcd_framebuffer / lcd_present over a host-side back buffer) sits on
 * top of this in a later PR; for bring-up we only need to prove the
 * BCM command path end to end.
 *
 * Not part of the portable HAL contract (hal.h) — sim has no BCM.
 */

#ifndef CORE_HAL_HW_LCD_H
#define CORE_HAL_HW_LCD_H

#include <stdint.h>

/* Host-side LCD port init (GPO32/GPIOC setup; no BCM bootstrap — the
 * chainload handoff guarantees the BCM is already powered, awake and
 * idle, see core/docs/hw/02-lcd.md "Chainload handoff state").
 * Returns nonzero if the BCM power-rail probe (GPO32_VAL bit 0x4000)
 * reads powered, 0 if not. Call once before lcd_fill(). */
int lcd_init(void);

/* Fill the entire 320x240 panel with one RGB565 color and present it
 * (full-frame BCMCMD_LCD_UPDATE, bootloader variant: returns without
 * waiting for completion). */
void lcd_fill(uint16_t rgb565);

/* Present a full host-side framebuffer to the panel: stream all
 * LCD_WIDTH*LCD_HEIGHT RGB565 pixels of `fb` (row-major, same layout
 * lcd_fill writes) via the full-frame BCMCMD_LCD_UPDATE fast path,
 * bootloader variant (returns without waiting for completion). `fb`
 * points to exactly LCD_WIDTH*LCD_HEIGHT uint16_t. Like lcd_fill, the
 * caller must have gated on lcd_init() reporting the BCM powered before
 * calling (the driver does no dead-BCM bootstrap).
 *
 * NOTE: this is the hw-only worker, deliberately NOT named lcd_present
 * — the portable HAL contract (hal.h) reserves `void lcd_present(void)`
 * for the back-buffer present that lands with lcd_framebuffer() in a
 * later PR; that entry point will simply forward
 * lcd_present_fb(lcd_framebuffer()). Naming it lcd_present here would
 * collide with hal.h (lcd.c includes both). */
void lcd_present_fb(const uint16_t *fb);

/* Present a sub-rectangle of a full-frame (LCD_WIDTH x LCD_HEIGHT,
 * row-major RGB565) buffer `fb`, streaming only w*h pixels to the panel
 * instead of the whole frame. (x,y) is the top-left corner and (w,h) the
 * size, all in pixels; the rect is clamped/validated to the panel bounds
 * and a fully out-of-bounds or zero-area rect is a safe no-op. Pixels are
 * read from `fb` at the full-frame stride: rect-local row r, column c is
 * fb[(y+r)*LCD_WIDTH + (x+c)].
 *
 * x and width are rounded to even (BCM bus alignment: pixels stream two
 * per 32-bit store) — x down, width up, so the rounded rect still covers
 * the requested region. Uses the same device-proven BCM handshake as
 * lcd_present_fb: overwrite only the changed pixels in the BCM's
 * persistent framebuffer, then the idle-wait-AFTER-stream + re-kick +
 * BCMCMD_LCD_UPDATE + 0x31 strobe commit (bootloader variant, returns
 * without waiting for completion). lcd_present_fb is exactly
 * lcd_present_rect(fb, 0, 0, LCD_WIDTH, LCD_HEIGHT). Same lcd_init()
 * powered gate applies. See core/docs/hw/02-lcd.md, "Partial present". */
void lcd_present_rect(const uint16_t *fb, int x, int y, int w, int h);

/*
 * Panel sleep/wake for suspend (EXPERIMENTAL — see lcd.c). lcd_sleep() blanks
 * the panel via the BCM LCD_SLEEP command without power-gating the BCM (so no
 * firmware re-upload is needed to wake). After lcd_wake(), present a frame to
 * re-light + repaint the panel.
 *
 * Both are idempotent. While slept, lcd_fill / lcd_present_fb /
 * lcd_present_rect are no-ops: streaming a frame into a BCM that is sleeping,
 * or into the internal panel init that follows a wake, latches it permanently
 * (the "screen wakes to solid white, needs a reboot" failure) and we have no
 * bcm_init() to recover with.
 *
 * SEQUENCING ON WAKE — the caller must: lcd_wake(); present ONE frame (which
 * absorbs up to ~500 ms of BCM panel init and returns when it has retired);
 * THEN raise the backlight. Lighting the panel before that first present
 * completes shows the panel init as a white flash (02-lcd.md).
 */
void lcd_sleep(void);
void lcd_wake(void);

/* Nonzero while the panel is slept (presents are being ignored). */
int lcd_is_slept(void);

/*
 * How many times a BCM handshake has exhausted its budget since boot. These
 * used to be silent — every wait proceeds with the access anyway and returned
 * void — so a wedged BCM produced corrupt frames with no indication. Nonzero
 * here means the display path is degraded; the driver also emits a
 * rate-limited UART line when it happens.
 */
uint32_t lcd_bcm_timeouts(void);

#endif /* CORE_HAL_HW_LCD_H */
