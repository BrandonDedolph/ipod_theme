/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/power.c — PCF50605 deep-sleep standby.
 *
 * Cleanroom from core/docs/hw/06-power.md ("Standby / sleep"): standby is a
 * single write to the PMU's OOCC1 register over the shared I2C control bus
 * (same 7-bit device 0x08 the battery gauge reads). GOSTDBY latches the
 * transition; a wake source bit (EXTONWAK / CHGWAK) MUST accompany it or the
 * device can never be powered on again. On wake the SoC is fully re-initialised
 * (registers/GPIO/peripherals lost), so control returns via the boot path, not
 * from here.
 */

#include "power.h"
#include "i2c.h"
#include "pp5022.h"
#include "mmio.h"
#include "backlight.h"

#define PMU_ADDR         0x08            /* PCF50605 control port (7-bit)        */
#define PMU_OOCC1        0x08            /* on/off control & config 1            */
#define OOCC1_GOSTDBY    0x01            /* trigger standby (latching)           */
#define OOCC1_CHGWAK     0x20            /* wake on charger insertion            */
#define OOCC1_EXTONWAK   0x40            /* wake on external (button / dock)     */

/* How many times to re-issue the standby write before giving up. The only
 * failure i2c_send can report is a BUSY-clear timeout (09-i2c.md), i.e. a bus
 * that is momentarily or permanently wedged; a handful of retries covers the
 * momentary case without turning a permanent one into a hang. */
#define PMU_STANDBY_TRIES  4

/* Settle between retries, in microseconds. Bounded USEC_TIMER wait (the
 * free-running 1 MHz counter, 01-soc-pp5022.md "Timers") with a trip cap, so
 * a stopped counter cannot turn this into an unbounded loop either. */
#define PMU_RETRY_US       2000u
#define PMU_RETRY_GUARD    (1u << 22)

static void power_delay_us(uint32_t us)
{
    uint32_t t0    = mmio_read32(USEC_TIMER_ADDR);
    uint32_t guard = PMU_RETRY_GUARD;
    while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < us && --guard != 0) {
        /* wait */
    }
}

/*
 * Pre-sleep housekeeping step 2 (06-power.md, "Pre-sleep housekeeping"):
 * clear the upper IRAM region at 0x4000C000 (0x14000 bytes on PP5022). Apple's
 * OF reads this to detect a "boot from sleep" flag; leaving it dirty sends the
 * next boot down the resume path instead of a clean one. Nothing in the tree
 * did this.
 *
 * CAVEAT, and the reason for the hole: our own stacks live at the very top of
 * that range — linker.ld puts the IRQ stack at [0x40017C00, 0x40018000) and
 * grows the supervisor stack down from 0x40017C00 — and we are running on
 * them. Zeroing the frame we are executing inside is not survivable, so the
 * top PMU_IRAM_KEEP bytes are skipped. Everything below and above that window
 * is cleared. If a device test shows the OF still taking the sleep path, the
 * flag lives inside the skipped window and the fix is to relocate the stack
 * before this runs, not to widen the loop.
 */
#define PMU_IRAM_CLEAR_BASE  0x4000C000u
#define PMU_IRAM_CLEAR_END   0x40020000u    /* base + 0x14000 */
#define PMU_IRAM_KEEP_LO     0x40014000u    /* skip [KEEP_LO, KEEP_HI): live stacks */
#define PMU_IRAM_KEEP_HI     0x40018000u

static void power_clear_iram(void)
{
    for (volatile uint32_t *p = (volatile uint32_t *)PMU_IRAM_CLEAR_BASE;
         p < (volatile uint32_t *)PMU_IRAM_KEEP_LO; p++) {
        *p = 0;
    }
    for (volatile uint32_t *p = (volatile uint32_t *)PMU_IRAM_KEEP_HI;
         p < (volatile uint32_t *)PMU_IRAM_CLEAR_END; p++) {
        *p = 0;
    }
}

int power_standby(void)
{
    power_clear_iram();

    /* Write OOCC1 = GOSTDBY | wake-sources. EXTONWAK + CHGWAK are BOTH set so a
     * button OR a charger wakes it — never issue GOSTDBY without a wake bit. */
    const uint8_t cmd[2] = {
        PMU_OOCC1,
        (uint8_t)(OOCC1_GOSTDBY | OOCC1_CHGWAK | OOCC1_EXTONWAK),
    };

    /*
     * The return value used to be discarded and followed by an unconditional
     * for(;;). On a wedged I2C bus that is an unkillable spin with a blank
     * screen — reached by the user ASKING to power off, so it reads as "my
     * iPod is bricked". Retry a bounded number of times, and if the PMU never
     * takes the command, come BACK rather than hang.
     */
    for (int try = 0; try < PMU_STANDBY_TRIES; try++) {
        if (i2c_send(PMU_ADDR, cmd, 2) == 0) {
            /* Accepted. The PMU cuts power within a few milliseconds; hold
             * here so we never run on past the point of no return with a
             * half-torn-down system. Bounded: if power somehow does NOT drop,
             * fall through and report failure instead of spinning forever. */
            power_delay_us(500000u);        /* 500 ms >> the few ms it needs */
            break;
        }
        power_delay_us(PMU_RETRY_US);
    }

    /*
     * Still here: standby did not happen. Make that VISIBLE — the caller has
     * already blanked the panel, so relight the backlight; a lit screen that
     * comes back is unambiguous, where a dark unresponsive device is not. The
     * caller can then repaint and stay usable.
     */
    backlight_set(BACKLIGHT_MAX);
    return -1;
}
