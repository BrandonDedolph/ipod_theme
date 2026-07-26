/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/backlight.h — LCD backlight brightness/on-off contract.
 *
 * The iPod Video 5G/5.5G backlight is a 32-level charge-pump LED driver
 * bit-banged over three GPIO lines (NOT a hardware PWM peripheral, and
 * NOT on the BCM video coprocessor). See backlight.c and
 * core/docs/hw/02-lcd.md ("Backlight") for the mechanism and the
 * cleanroom facts it was built from.
 *
 * Level convention (matches the UI's dim/fade + inactivity-off needs):
 *   backlight_set(0)             -> backlight OFF (LED disabled)
 *   backlight_set(BACKLIGHT_MAX) -> full brightness
 *   1 .. BACKLIGHT_MAX-1         -> intermediate dim levels
 */

#ifndef CORE_HAL_HW_BACKLIGHT_H
#define CORE_HAL_HW_BACKLIGHT_H

/*
 * Number of discrete brightness steps in the external LED-driver IC.
 * The dimmer counts 1..32 (verified against Rockbox
 * backlight-nano_video.c, which tracks `current_dim` over 1..32,
 * 2026-07-21). Level 0 is our software convention for "LED off", handled
 * by the separate enable line rather than a 33rd dimmer step.
 */
#define BACKLIGHT_MAX  32

/* Power the backlight circuit, configure the GPIO lines, and bring the
 * panel to full brightness. Call once after lcd_init(). */
void backlight_init(void);

/* Set brightness. level<=0 turns the LED off (circuit stays powered so a
 * later non-zero level relights instantly); 1..BACKLIGHT_MAX steps the
 * dimmer to that level. Values above BACKLIGHT_MAX are clamped. */
void backlight_set(int level);

/*
 * Periodic housekeeping, called from the 100 Hz system tick. Its only job is
 * the screen-off power policy: backlight_set(0) drops the LED-enable line but
 * leaves the boost converter running so a quick relight is instant, and this
 * shuts the converter down once the off state has lasted a few seconds. The
 * next backlight_set(level > 0) powers it back up and re-seeds the tracked
 * level from the driver IC's power-up reference.
 *
 * Bounded, allocation-free, and at most ONE bus write per call — safe from
 * interrupt context.
 */
void backlight_service(void);

#endif /* CORE_HAL_HW_BACKLIGHT_H */
