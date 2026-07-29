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

/* Host-side LCD port init (GPO32/GPIOC setup).
 *
 * This USED to rely on the chainloader having left the BCM powered, awake
 * and idle — ipodloader2 drew its own menu, so it necessarily had. We now
 * boot DIRECTLY from the firmware partition and nobody has touched the BCM
 * before us, so that guarantee is gone; bcm_init() exists to bring it up
 * cold. See core/docs/hw/02-lcd.md.
 *
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

/* ---- BCM bootstrap / recovery ---------------------------------------------
 *
 * NOT ON THE BOOT PATH. lcd_init() does not call bcm_init(): boot relies
 * entirely on the chainload handoff having left the BCM powered, bootstrapped
 * and idle, exactly as it always has. These exist for the case that handoff
 * cannot help with — a BCM that wedges at runtime, which today is
 * unrecoverable without a reboot, on a device whose only debug channel is the
 * screen the BCM drives.
 */

/* bcm_init() result codes. Negative values name the step that failed, so a
 * future debug screen can say WHERE the bootstrap died without a serial
 * cable. */
#define LCD_BCM_OK                 0
#define LCD_BCM_ERR_NO_BLOB      (-1)  /* no `vmcs` tag in the flash directory */
#define LCD_BCM_ERR_BAD_BLOB     (-2)  /* tag found, offset/length implausible */
#define LCD_BCM_ERR_ALT_BUSY     (-3)  /* stage 2: alt-control never went idle */
#define LCD_BCM_ERR_ALT_READY    (-4)  /* stage 2: alt-control never went ready*/
#define LCD_BCM_ERR_PORTS        (-5)  /* read ports never came ready          */
#define LCD_BCM_ERR_UPLOAD_BUSY  (-6)  /* stage 3 gate: never went idle        */
#define LCD_BCM_ERR_UPLOAD_READY (-7)  /* stage 3 gate: never went ready       */
#define LCD_BCM_ERR_SDRAM        (-8)  /* SDRAM mapping never completed        */
#define LCD_BCM_ERR_START        (-9)  /* firmware never started               */
#define LCD_BCM_ERR_BUSY        (-10)  /* lcd_recover() re-entered             */

/*
 * Full BCM bootstrap: power-cycle the coprocessor, run the strap + handshake
 * sequence, upload the `vmcs` firmware blob out of flash ROM, and start it
 * (core/docs/hw/02-lcd.md, "Bootstrap and firmware upload").
 *
 * Returns LCD_BCM_OK, or one of the codes above. The blob is located and
 * validated BEFORE any hardware is touched, and a blob that fails validation
 * is reported rather than uploaded — starting the BCM's processor on garbage
 * is worse than leaving it wedged.
 *
 * On success the BCM is running but its SDRAM framebuffer is undefined (the
 * power cycle discarded it) and the panel is mid-init: the caller must present
 * a frame, and that first frame can take the documented ~500 ms. Most callers
 * want lcd_recover() instead, which does all of that.
 *
 * UNVERIFIED ON SILICON — see the notes in lcd.c. Nothing in this sequence has
 * ever run on a real device; the chainloader has always done it for us.
 */
int bcm_init(void);

/*
 * Recover a wedged BCM: re-run the host-side port init, bcm_init(), and
 * re-establish framebuffer/panel state (one known full frame — lcd.c holds no
 * back buffer, so the caller must repaint afterwards). Returns LCD_BCM_OK or
 * the bcm_init() failure code; re-entrant calls return LCD_BCM_ERR_BUSY.
 *
 * This is the function a future caller uses when the BCM has wedged. It is
 * wired into exactly ONE place inside lcd.c — the post-wake absorb path, as a
 * last resort when the first commit after lcd_wake() exhausts its wall-clock
 * budget, i.e. precisely the case that currently latches the panel white until
 * a reboot — and that wiring is compiled out unless LCD_RECOVER_ON_WAKE is
 * defined to 1. It defaults to 0, so the shipping image behaves exactly as it
 * did before this existed.
 *
 * ENABLING IT IS A DEVICE-RISK CHANGE: it power-cycles the BCM on a sequence
 * no one has ever executed on this hardware. Turn it on only with a known-good
 * rollback image available.
 */
int lcd_recover(void);

/*
 * How many times lcd_recover() has been ENTERED since boot (attempts, not
 * successes — a recovery that ran and failed is the most interesting case).
 * Zero means the recovery path has never fired, which is what a healthy device
 * should report forever.
 */
uint32_t lcd_bcm_recoveries(void);

#endif /* CORE_HAL_HW_LCD_H */
