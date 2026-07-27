/*
 * core/hal/hw/lcd.c — BCM video coprocessor LCD driver (PP5022,
 * full-frame solid fill).
 *
 * Implements the host-side port init and the full-frame LCD_UPDATE
 * sequence from core/docs/hw/02-lcd.md ("Host-side port init",
 * "Memory-mapped BCM interface", "LCD update protocol"), verified
 * against Rockbox lcd-video.c / ipodloader2 fb.c (2026-06-11).
 *
 * The BCM bootstrap is at the END of this file and is NOT on the boot
 * path. Boot relies on the Apple flash ROM having powered the BCM and
 * uploaded its firmware before any loader runs (02-lcd.md, "Chainload
 * handoff state") — that part holds whether we are chainloaded or
 * dispatched straight out of the firmware partition as OSOS. What does
 * NOT survive dropping ipodloader2 is its extra guarantee that the BCM
 * is handed over IDLE, so the first frame no longer assumes that; see
 * lcd_first_frame. bcm_init()/lcd_recover() exist for the case the
 * handoff cannot help with — a BCM that wedges at runtime.
 *
 * For frames we use the Rockbox-BOOTLOADER update variant: point the
 * write port at the framebuffer, stream params/data, THEN wait for the
 * previous update to retire, write the command, strobe, and return
 * without a completion wait. Waiting AFTER the
 * ~150 KB pixel stream — not before it — matches Rockbox's ordering
 * (the stream is the completion delay) and is what lets repeated
 * presents work: polling right after the previous strobe hits the BCM
 * at peak-busy and spins its whole budget (the "second present stalls"
 * bug). A re-kick re-issues the update if the BCM latches busy past a
 * bounded poll budget (analog of Rockbox's 50 ms BCM_UPDATE_TIMEOUT).
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only
 * (via pp5022.h); LCD_WIDTH/LCD_HEIGHT from the portable hal.h
 * contract.
 */

#include "pp5022.h"
#include "mmio.h"
#include "lcd.h"
#include "hal.h"
#ifndef MMIO_MOCK
#include "../../kernel/irq.h"   /* arch_irq_save/restore: present is a critical
                                 * section (real build only; the host trace
                                 * tests compile this file with -DMMIO_MOCK and
                                 * the ARM CPSR asm can't build there). */
#define LCD_IRQ_ENTER()  uint32_t lcd_irq_saved_ = arch_irq_save()
#define LCD_IRQ_EXIT()   arch_irq_restore(lcd_irq_saved_)
#else
#define LCD_IRQ_ENTER()  ((void)0)
#define LCD_IRQ_EXIT()   ((void)0)
#endif

/*
 * Upper bounds on the BCM handshake polls so a wedged or absent BCM
 * can't hang the kernel (same degrade-gracefully pattern as uart.c's
 * TX spin). The write/read handshakes normally complete in a few bus
 * cycles, so 1<<16 trips is orders of magnitude past "working
 * hardware". On timeout the access is attempted anyway: worst case
 * the frame is corrupt or dropped, the firmware keeps running and the
 * UART narration tells us what happened.
 */
#define BCM_SPIN_LIMIT       (1u << 13)   /* ~8k; a healthy handshake takes a few
                                          * cycles, so this is still orders past
                                          * working hw but bounds a wedged BCM to
                                          * sub-ms instead of the ~1ms of 1<<16. */

/*
 * Upper bound on the wait-for-idle poll before issuing a new frame.
 * Each trip is a full bcm_read32 handshake (several bus accesses), and
 * Rockbox's own re-kick timeout for a stalled update is 50 ms, so
 * 1<<16 read attempts is far beyond any healthy update. On timeout we
 * just proceed — issuing over a busy BCM beats hanging.
 */
#define BCM_IDLE_SPIN_LIMIT  (1u << 9)    /* Bound the wait-for-idle OUTER loop:
                                          * each trip is a bcm_read32 (itself
                                          * bounded), so this caps a wedged BCM to
                                          * ~tens of ms and issues the update
                                          * anyway, instead of the minutes-long
                                          * nested spin (65536 x 65536) that hard-
                                          * froze the device on a present after a
                                          * scroll wedged the BCM. Healthy path:
                                          * the pixel stream already retired the
                                          * previous update, so idle reads on the
                                          * first trip. */

/*
 * Poll-count budget before we re-kick a BCM that is still reading the
 * busy pattern. Rockbox re-issues LCD_UPDATE + strobe if an update
 * hasn't retired within HZ/20 (~50 ms) to unstick a latched-busy BCM;
 * lacking a tick clock here we approximate that timeout as a fraction
 * of the idle-spin budget. In the healthy case the ~150 KB pixel
 * stream has already given the previous update ample time to retire, so
 * the idle poll returns on its first read and this budget is never hit.
 */
#define BCM_REKICK_TRIPS     (BCM_IDLE_SPIN_LIMIT >> 4)

/*
 * First-frame flag. This used to mean "skip the wait-for-idle entirely,
 * because the chainload handoff guarantees an idle BCM". It no longer does.
 *
 * That guarantee was specifically an ipodloader2 property (02-lcd.md,
 * "Chainload handoff state": it finishes its final frame SYNCHRONOUSLY,
 * polling completion before jumping to the loaded image). Booting our image
 * directly out of the firmware partition as OSOS removes ipodloader2 from the
 * picture entirely, and nothing else establishes it — the Apple flash ROM
 * powers the BCM, uploads its firmware and inits the panel, but nowhere is it
 * documented to hand over with no update in flight, and its panel init alone
 * is documented to take up to 500 ms.
 *
 * So the first frame now WAITS, and the flag only selects WHICH wait. What we
 * genuinely do not know at frame one is whether the previous stage left an
 * update (or a panel init) still running, so it takes the same absorb the
 * post-wake path takes: wall-clock bounded, and NO re-kick — hammering fresh
 * commands at a BCM that is mid-panel-init is what latches it. Cost on a
 * healthy device is one bounded read handshake, once, at boot: whoever booted
 * us has long since gone idle and it returns on the first read.
 *
 * Steady-state frames keep the trip-counted re-kick path below, which is
 * tuned for the common case (the ~150 KB pixel stream already retired the
 * previous update) and is unchanged.
 */
static int lcd_first_frame = 1;

/* ---------------------------------------------------------------------------
 * Failure reporting.
 *
 * Every BCM handshake here exhausts its spin budget and then performs the
 * access anyway, and all of them return void — so a wedged BCM produced
 * corrupt frames with no signal of any kind, anywhere. The commit path now
 * returns a status, counts exhaustions, and narrates them over the debug UART.
 *
 * The warning is RATE-LIMITED: a stuck BCM fails on every frame, and a warning
 * per frame would both flood the console and, at 115200 baud, add milliseconds
 * of blocking UART time to each present — turning a display glitch into a
 * playback glitch. One line per second is enough to diagnose.
 *
 * uart_puts is declared weak because the host golden-trace tests link lcd.c on
 * its own; there it resolves to NULL and the call is skipped.
 * ------------------------------------------------------------------------- */
__attribute__((weak)) void uart_puts(const char *s);

#define LCD_WARN_INTERVAL_US  1000000u

static uint32_t lcd_timeouts;       /* total budget exhaustions since boot */
static uint32_t lcd_warn_last_us;
static int      lcd_warn_seen;

static void lcd_warn(const char *msg)
{
    if (!uart_puts) {
        return;
    }
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    if (lcd_warn_seen &&
        (uint32_t)(now - lcd_warn_last_us) < LCD_WARN_INTERVAL_US) {
        return;
    }
    lcd_warn_seen   = 1;
    lcd_warn_last_us = now;
    uart_puts(msg);
}

uint32_t lcd_bcm_timeouts(void)
{
    return lcd_timeouts;
}

/* ---------------------------------------------------------------------------
 * Wall-clock waits, for the panel sleep/wake paths only.
 *
 * The ordinary commit path counts POLL TRIPS, which is fine when the answer
 * arrives in microseconds. It is useless for the one case that takes hundreds
 * of milliseconds: 02-lcd.md ("LCD update protocol") records that after waking
 * from sleep the BCM's FIRST update can take up to 500 ms, because it is doing
 * internal LCD panel init. Poll trips cannot express that; the microsecond
 * counter can.
 * ------------------------------------------------------------------------- */
#define BCM_LCDINIT_TIMEOUT_US  600000u   /* > the documented 500 ms panel init */
#define BCM_SLEEP_DRAIN_US      200000u   /* in-flight update before LCD_SLEEP  */
#define BCM_PANEL_SETTLE_US      20000u   /* panel-enable bits to take effect   */

#ifdef MMIO_MOCK
#define BCM_WALL_GUARD_TRIPS  4u
#else
#define BCM_WALL_GUARD_TRIPS  (1u << 24)
#endif

/* Bounded busy-wait on the microsecond counter. */
static void bcm_wall_delay(uint32_t us)
{
    uint32_t t0    = mmio_read32(USEC_TIMER_ADDR);
    uint32_t guard = BCM_WALL_GUARD_TRIPS;
    while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < us && --guard != 0) {
        /* wait */
    }
}

/* Panel-slept flag: while set, NOTHING may stream pixels (see lcd_sleep). */
static int lcd_slept;

/* Set by lcd_wake: the next commit must absorb the long panel-init update. */
static int lcd_post_wake;

/*
 * BCM re-bootstrap on a failed post-wake absorb — OFF by default.
 *
 * lcd_recover() is a real recovery path but an UNVERIFIED one: no part of the
 * bootstrap it runs has ever executed on silicon (the chainloader has always
 * done it for us), it power-cycles the BCM on the way in, and if it is wrong
 * the panel does not come back — on a device whose only debug channel is that
 * panel. So the shipping image keeps today's behaviour exactly: the post-wake
 * commit absorbs, gives up, and reports. Set this to 1 only on a device with a
 * known-good rollback image to hand (see lcd_recover's header comment).
 */
#ifndef LCD_RECOVER_ON_WAKE
#define LCD_RECOVER_ON_WAKE 0
#endif

/* Re-entrancy guard: lcd_recover() ends by presenting a frame, which lands
 * back in bcm_frame_commit — which is one of lcd_recover's callers. */
static int lcd_in_recover;

/* How many times lcd_recover() has been entered (see lcd_bcm_recoveries). */
static uint32_t lcd_recoveries;

/* Set the BCM-internal write destination: 32-bit address store to the
 * write-address port, then poll write-ready (02-lcd.md, write
 * handshake; verified against Rockbox lcd-video.c, 2026-06-11). */
static void bcm_write_addr(uint32_t addr)
{
    uint32_t spin = BCM_SPIN_LIMIT;

    mmio_write32(BCM_WR_ADDR_ADDR, addr);
    while (!(mmio_read16(BCM_CONTROL_ADDR) & BCM_CONTROL_WR_READY) &&
           --spin != 0) {
        /* poll */
    }
}

/* Write one 32-bit word to a BCM-internal address. */
static void bcm_write32(uint32_t addr, uint32_t value)
{
    bcm_write_addr(addr);
    mmio_write32(BCM_DATA_ADDR, value);
}

/* Read one 32-bit word from a BCM-internal address. Handshake order
 * matters (02-lcd.md, read handshake; verified against Rockbox
 * lcd-video.c, 2026-06-11): poll the read port ready FIRST, then
 * write the address, then poll data-ready, then read the data port. */
static uint32_t bcm_read32(uint32_t addr)
{
    uint32_t spin = BCM_SPIN_LIMIT;

    while (!(mmio_read16(BCM_RD_ADDR_ADDR) & BCM_RD_ADDR_READY) &&
           --spin != 0) {
        /* poll */
    }
    mmio_write32(BCM_RD_ADDR_ADDR, addr);

    spin = BCM_SPIN_LIMIT;
    while (!(mmio_read16(BCM_CONTROL_ADDR) & BCM_CONTROL_RD_READY) &&
           --spin != 0) {
        /* poll */
    }
    return mmio_read32(BCM_DATA_ADDR);
}

/* Host-side port init (02-lcd.md, "Host-side port init"; from Rockbox
 * lcd_init_device, verified 2026-06-11 — runs even when the BCM is
 * already alive): BCM power rail + companion bit as GPO, GPIOC bit 6 =
 * BCM interrupt pin as a GPIO input, bit 7 released to its alternate
 * function, GPO32 bit 0 released.
 *
 * Factored out of lcd_init() verbatim so lcd_recover() can re-run it
 * before a bootstrap — the doc is explicit that this half "always runs",
 * separately from the BCM bringup. lcd_init()'s emitted grammar is
 * unchanged (the golden trace hw-lcd-trace pins it). */
static void lcd_port_init(void)
{
    mmio_write32(GPO32_ENABLE_ADDR, mmio_read32(GPO32_ENABLE_ADDR) | 0xC000);
    mmio_write32(GPIOC_ENABLE_ADDR, mmio_read32(GPIOC_ENABLE_ADDR) & ~0x80);
    mmio_write32(GPIOC_ENABLE_ADDR, mmio_read32(GPIOC_ENABLE_ADDR) | 0x40);
    mmio_write32(GPIOC_OUTPUT_EN_ADDR,
                 mmio_read32(GPIOC_OUTPUT_EN_ADDR) & ~0x40);
    mmio_write32(GPO32_ENABLE_ADDR, mmio_read32(GPO32_ENABLE_ADDR) & ~1u);
}

int lcd_init(void)
{
    lcd_port_init();

    /*
     * Probe: nonzero means the BCM is powered (and, post-chainload,
     * initialized). Boot still relies ENTIRELY on that handoff — this
     * deliberately does NOT call bcm_init() on a failed probe. The
     * bootstrap is unverifiable off-hardware and its failure mode is a
     * dead screen on a device whose only debug channel IS the screen, so
     * it ships as an explicit recovery entry point (lcd_recover) that
     * nothing on the normal path calls. See the bootstrap block below.
     */
    return (mmio_read32(GPO32_VAL_ADDR) & GPO32_BCM_POWER) != 0;
}

/* Number of 32-bit stores that carry one full 320x240 frame: two
 * RGB565 pixels are packed per store. */
#define BCM_FRAME_WORDS  ((LCD_WIDTH * LCD_HEIGHT) / 2u)

/*
 * Shared full-frame update preamble (step 2, identical for lcd_fill and
 * lcd_present): point the BCM write port at the framebuffer / command
 * parameter region so the caller can stream pixel data.
 *
 * The wait-for-idle deliberately does NOT live here. Polling right
 * after the previous frame's strobe — before any pixels stream — hits
 * the BCM at peak busy and never sees idle within budget (the "second
 * present stalls" hang). It runs in bcm_frame_commit instead, after the
 * full pixel stream has given the previous update time to retire.
 */
static void bcm_frame_begin(void)
{
    bcm_write_addr(BCMA_CMDPARAM);
}

/*
 * Shared full-frame update commit (steps 1, 4-5, identical for lcd_fill
 * and lcd_present):
 *
 *  (1) Wait for the previous update to retire, unless this is the first
 *      frame after the chainload handoff (guaranteed idle). The BCM is
 *      busy while BCMA_COMMAND still reads the in-flight LCD_UPDATE code
 *      (0xFFFF0000) or its half-consumed remnant 0xFFFF (02-lcd.md,
 *      "LCD update protocol"; verified against Rockbox lcd-video.c,
 *      2026-06-11). By the time we get here the caller has streamed the
 *      whole frame, so a healthy BCM has long since gone idle and the
 *      poll returns on its first read. If it is still busy past
 *      BCM_REKICK_TRIPS polls it may have latched — re-issue the command
 *      and re-strobe to unstick it (Rockbox's 50 ms re-kick analog). On
 *      exhausting the full spin budget, proceed anyway.
 *  (4) Queue the full-frame update command.
 *  (5) Strobe execute. Bootloader variant: return without a completion
 *      wait; the next frame's pre-stream wait catches any overrun.
 */
/*
 * Wait for an in-flight LCD_UPDATE to retire, bounded by WALL CLOCK and with
 * NO re-kick. Used for the two long-latency cases (the first update after a
 * panel wake, and draining before LCD_SLEEP). Returns 0 if it went idle.
 */
static int bcm_wait_idle_wall(uint32_t us)
{
    uint32_t t0    = mmio_read32(USEC_TIMER_ADDR);
    uint32_t guard = BCM_WALL_GUARD_TRIPS;
    uint32_t stat  = bcm_read32(BCMA_COMMAND);

    while ((stat == BCMCMD_LCD_UPDATE || stat == 0xFFFF) && --guard != 0) {
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) > us) {
            return -1;
        }
        stat = bcm_read32(BCMA_COMMAND);
    }
    return guard != 0 ? 0 : -1;
}

static int bcm_frame_commit(void)
{
    int rc = 0;
    const int post_wake = lcd_post_wake;

    if (post_wake || lcd_first_frame) {
        /*
         * THE TWO "WE DON'T KNOW WHAT THE BCM IS DOING" COMMITS: the first
         * one after a panel wake, and the very first one after boot.
         *
         * Post-wake, 02-lcd.md: the update can take up to 500 ms because the
         * BCM is running internal LCD panel init. At boot we inherit whatever
         * the previous stage left running — under a direct ROM boot (no
         * ipodloader2) that may include the Apple ROM's own panel init, which
         * the doc times at the same ~500 ms; see lcd_first_frame above.
         *
         * The ordinary path below budgets BCM_IDLE_SPIN_LIMIT (512 polls, on
         * the order of milliseconds) and RE-KICKS LCD_UPDATE up to
         * BCM_IDLE_SPIN_LIMIT/BCM_REKICK_TRIPS times inside that window — so
         * it hammers new update commands into a BCM that is mid-panel-init,
         * and later presents stream fresh pixels into the same in-progress
         * init. That is how the BCM ends up permanently latched: the screen
         * wakes to solid white and only a reboot recovers.
         *
         * So both of these commits ABSORB instead: a wall-clock window wider
         * than the documented panel-init time, and NO re-kick. Re-kicking is
         * the failure mechanism here, not the cure.
         */
        rc = bcm_wait_idle_wall(BCM_LCDINIT_TIMEOUT_US);
        lcd_post_wake = 0;
#if LCD_RECOVER_ON_WAKE
        /* Recovery is wired to the POST-WAKE absorb only — the boot-time
         * absorb deliberately does not trigger a re-bootstrap. At frame one
         * the panel has never been shown to work in the first place, so a
         * timeout there is far more likely to mean "this BCM was never alive"
         * (emulator, dead unit) than "a working BCM wedged", and power-cycling
         * it would trade a diagnosable boot for an unexplained dark screen. */
        if (post_wake && rc != 0 && !lcd_in_recover) {
            /*
             * LAST RESORT. The BCM was given a window wider than the documented
             * 500 ms panel init and still never retired the update — this is
             * precisely the state that latched the panel solid white until a
             * reboot, and from here every later frame is streamed into a BCM
             * that will never consume it. Re-bootstrap it from scratch.
             *
             * Return WITHOUT the command + strobe below: lcd_recover() has
             * already streamed and strobed a full frame at the freshly started
             * BCM, and issuing a second update on top of that would hammer a
             * BCM in mid-panel-init — the exact mechanism this branch exists to
             * avoid. The pixels this call streamed died with the power cycle
             * (the BCM's SDRAM framebuffer does not survive it), so the caller
             * must repaint; the -1 says so.
             */
            lcd_timeouts++;
            lcd_warn("lcd: post-wake BCM never retired — re-bootstrapping\n");
            (void)lcd_recover();
            return -1;
        }
#endif
    } else if (!lcd_first_frame) {
        uint32_t spin = BCM_IDLE_SPIN_LIMIT;
        uint32_t kick = BCM_REKICK_TRIPS;
        uint32_t stat = bcm_read32(BCMA_COMMAND);

        while ((stat == BCMCMD_LCD_UPDATE || stat == 0xFFFF) &&
               --spin != 0) {
            if (--kick == 0) {
                /* Still busy far too long: the update likely latched.
                 * Re-issue LCD_UPDATE + strobe to unstick the BCM. */
                bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);
                mmio_write16(BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);
                kick = BCM_REKICK_TRIPS;
            }
            stat = bcm_read32(BCMA_COMMAND);
        }
        if (spin == 0) {
            rc = -1;
        }
    }
    lcd_first_frame = 0;

    bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);
    mmio_write16(BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);

    if (rc != 0) {
        lcd_timeouts++;
        lcd_warn("lcd: BCM idle-wait timed out (frames may be corrupt)\n");
    }
    return rc;
}

/* Byte stride of one framebuffer row in BCM-internal memory: LCD_WIDTH
 * RGB565 pixels, 2 bytes each (02-lcd.md, "Internal BCM addresses":
 * pixel (x,y) at offset (LCD_WIDTH*2)*y + x*2 from BCMA_CMDPARAM). */
#define BCM_ROW_STRIDE_BYTES  ((uint32_t)LCD_WIDTH * 2u)

/*
 * Stream a contiguous run of `pixels` RGB565 values from `src` to the
 * BCM data port as packed 32-bit stores — two pixels per store, exactly
 * the packing lcd_present_fb proved on real 5.5G silicon: the earlier
 * pixel src[2k] (even, first 16-bit push) in the LOW half, the later
 * src[2k+1] (odd, second push) in the HIGH half (02-lcd.md,
 * "Memory-mapped BCM interface" — a 32-bit store is consumed as two
 * consecutive 16-bit pushes to ascending BCM addresses, low half first
 * on this little-endian bus). The caller must have already pointed the
 * write port at the run's start with bcm_write_addr, and `pixels` MUST
 * be even so the run is a whole number of 32-bit stores (guaranteed by
 * the even-width rounding in lcd_present_rect and by W*H being even for
 * a full frame). */
static void bcm_stream_pixels(const uint16_t *src, uint32_t pixels)
{
    uint32_t words = pixels >> 1;
    uint32_t k = 0;

    while (k < words) {
        const uint32_t pair = ((uint32_t)src[2u * k + 1u] << 16) |
                              src[2u * k];
        mmio_write32(BCM_DATA_ADDR, pair);
        k++;
    }
}

void lcd_fill(uint16_t rgb565)
{
    /* Solid color: both packed halves are the same pixel, so the
     * high/low ordering (see lcd_present) is irrelevant here. */
    const uint32_t pair = ((uint32_t)rgb565 << 16) | rgb565;
    uint32_t n = BCM_FRAME_WORDS;

    if (lcd_slept) {
        return;      /* panel is asleep — never stream into a powering-down BCM */
    }

    /* ONLY the pixel stream must be uninterrupted: an ISR stalling the push
     * mid-stream makes the BCM abort the frame. So mask just the stream — NOT
     * bcm_frame_commit()'s wait-for-idle spin, which polls the BCM and touches
     * no pixels. Masking the spin (as the code once did) let a long spin during
     * back-to-back presents starve the audio DMA ISR (glitches) and, worst case,
     * stall the tick + audio long enough to hard-freeze mid-present. The audio
     * ISR touches no BCM state, so it is safe to fire during the spin/commit. */
    LCD_IRQ_ENTER();
    bcm_frame_begin();

    /* (3) Stream the full 320x240 frame as 32-bit stores, two RGB565
     * pixels per store, no per-store handshake — the BCM's undecoded
     * low address bits consume each word as two consecutive 16-bit
     * pushes (02-lcd.md, "Memory-mapped BCM interface"; verified
     * against Rockbox lcd-video.c / ipodloader2 fb.c, 2026-06-11). */
    while (n-- != 0) {
        mmio_write32(BCM_DATA_ADDR, pair);
    }

    LCD_IRQ_EXIT();                       /* unmask before the idle-wait spin */
    bcm_frame_commit();
}

/*
 * Present a sub-rectangle of a full-frame (LCD_WIDTH x LCD_HEIGHT,
 * row-major RGB565) buffer `fb`, streaming only the rect's w*h pixels to
 * the panel instead of the whole ~150 KB frame — the win for our
 * row-based UI, which usually redraws one horizontal band.
 *
 * Mechanism (cleanroom from 02-lcd.md, "LCD update protocol" — the
 * documented Rockbox lcd_update_rect, NOT the ipodloader2
 * BCMCMD_LCD_UPDATERECT / BCM_CMD(5) header path): the BCM keeps a
 * persistent 320x240 framebuffer in its own SDRAM at BCMA_CMDPARAM. We
 * overwrite ONLY the changed pixels there — at their normal full-frame
 * stride offsets — then issue the ordinary full-frame BCMCMD_LCD_UPDATE.
 * The BCM scans out its whole framebuffer; the untouched regions keep
 * last frame's pixels. This deliberately reuses the device-proven
 * bcm_frame_commit() handshake verbatim (idle-wait AFTER the stream,
 * re-kick, LCD_UPDATE, 0x31 strobe) — the ONLY things that differ from
 * lcd_present_fb are the write-port start offset and, for a
 * narrower-than-full rect, a per-row re-address to skip the gap between
 * rows. We chose this over the UPDATERECT command because it (a) reuses
 * the crown-jewel commit unchanged and (b) makes a full-frame rect
 * byte-identical to the proven path, so lcd_present_fb can delegate.
 *
 * A fully out-of-bounds or zero-area rect is a safe no-op. x and width
 * are rounded to even (BCM bus alignment: pixels stream two-per-32-bit-
 * store, so a row must start on an even column and span an even count;
 * 02-lcd.md, "LCD update protocol / Constraints" — x down, width up to
 * still cover the requested region).
 */
void lcd_present_rect(const uint16_t *fb, int x, int y, int w, int h)
{
    /* Panel asleep: refuse. Streaming pixels while the BCM is sleeping — or
     * while it is running the panel init that follows a wake — is what latches
     * it permanently (see lcd_sleep / bcm_frame_commit). */
    if (lcd_slept) {
        return;
    }

    /* Validate + clamp to the panel. Trim a partially off-screen rect;
     * bail on anything with no on-screen area. */
    if (w <= 0 || h <= 0) {
        return;
    }
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || x + w <= 0 || y + h <= 0) {
        return;
    }
    if (x < 0) { w += x; x = 0; }           /* trim off-screen-left  */
    if (y < 0) { h += y; y = 0; }           /* trim off-screen-above */
    if (x + w > LCD_WIDTH)  { w = LCD_WIDTH  - x; }
    if (y + h > LCD_HEIGHT) { h = LCD_HEIGHT - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    /* Even alignment (BCM streams two pixels per 32-bit store): snap the
     * left edge down to an even column and the right edge up to an even
     * column, then take the width between them — so the aligned window
     * still fully covers the requested [x, x+w). Both endpoints even ->
     * width even. */
    int left  = x & ~1;                     /* floor x to even            */
    int right = (x + w + 1) & ~1;           /* ceil (x+w) to even         */
    x = left;
    w = right - left;
    if (x + w > LCD_WIDTH) { w = LCD_WIDTH - x; }
    if (w <= 0 || h <= 0) {
        return;
    }

    /* Mask ONLY the pixel stream (see lcd_fill's note); the wait-for-idle in
     * bcm_frame_commit runs unmasked so the audio DMA ISR can preempt it. */
    LCD_IRQ_ENTER();

    if (w == LCD_WIDTH) {
        /* Full-width band: the rect's rows are contiguous in BCM memory
         * (no inter-row gap), so a single write-addr + one contiguous
         * stream covers them — exactly like lcd_present_fb. For a full
         * frame (x=0,y=0,w=W,h=H) the offset is 0 and the pixel count is
         * W*H, so this reproduces the proven path byte-for-byte. */
        bcm_write_addr(BCMA_CMDPARAM + (uint32_t)y * BCM_ROW_STRIDE_BYTES);
        bcm_stream_pixels(fb + (uint32_t)y * LCD_WIDTH,
                          (uint32_t)w * (uint32_t)h);
    } else {
        /* Narrower rect: each destination row is separated by a
         * full-width gap in BCM memory, so re-point the write port at the
         * start of every row before streaming that row's w pixels. Source
         * stride is the full framebuffer width; dest stride is one BCM
         * row (02-lcd.md, "LCD update protocol", partial-width branch). */
        for (int r = 0; r < h; r++) {
            uint32_t row = (uint32_t)(y + r);
            bcm_write_addr(BCMA_CMDPARAM + row * BCM_ROW_STRIDE_BYTES +
                           (uint32_t)x * 2u);
            bcm_stream_pixels(fb + row * (uint32_t)LCD_WIDTH + (uint32_t)x,
                              (uint32_t)w);
        }
    }

    LCD_IRQ_EXIT();                       /* unmask before the idle-wait spin */
    bcm_frame_commit();
}

void lcd_present_fb(const uint16_t *fb)
{
    /* A full-frame present is just the whole-panel rectangle. This
     * delegation is byte-identical to the former hand-written full-frame
     * loop: lcd_present_rect's w==LCD_WIDTH branch with x=y=0 points the
     * write port at BCMA_CMDPARAM, streams exactly BCM_FRAME_WORDS packed
     * words in the same low=even/high=odd order, and commits through the
     * same bcm_frame_commit() — verified by the hw-lcd-present /
     * hw-lcd-trace golden traces, which are unchanged. */
    lcd_present_rect(fb, 0, 0, LCD_WIDTH, LCD_HEIGHT);
}

/* ---- Panel sleep/wake (suspend) -------------------------------------------
 *
 * EXPERIMENTAL. Rockbox's iPod-Video panel-off also POWER-GATES the BCM
 * (GPO32 &= ~0x4000) and re-bootstraps + re-uploads its firmware on wake — code
 * we don't have (we rely on the boot handoff for BCM bringup). So this is the
 * RECOVERABLE subset: clear the panel-enable bits and issue the LCD_SLEEP
 * command, but leave the BCM powered and bootstrapped. lcd_wake() restores the
 * bits; the caller then presents a frame to re-light + repaint. If the panel
 * doesn't come back, the BCM is still alive so a present (or worst case a
 * reset) recovers — nothing is permanently off.
 *
 * Register/command facts (Rockbox iPod-Video bcm_powerdown): panel-enable bits
 * 0xF0 in BCM reg 0x10001400; LCD_SLEEP is BCM command 8. */
#define BCM_PANEL_CTL_ADDR    0x10001400u
#define BCM_PANEL_CTL_ENABLE  0x000000F0u
#define BCMCMD_LCD_SLEEP      BCM_CMD(8)

void lcd_sleep(void)
{
    if (lcd_slept) {
        return;
    }

    /*
     * Drain first. Issuing LCD_SLEEP on top of an LCD_UPDATE that is still in
     * flight leaves the BCM holding two commands, and the sleep is what the
     * panel-init on the other side of the wake has to unwind. Bounded by wall
     * clock, unmasked (this polls the BCM and touches no pixels, so the audio
     * ISR is free to preempt it); if it does not retire we proceed anyway and
     * say so, because refusing to sleep is worse than sleeping untidily.
     */
    if (bcm_wait_idle_wall(BCM_SLEEP_DRAIN_US) != 0) {
        lcd_timeouts++;
        lcd_warn("lcd: BCM still busy at sleep\n");
    }

    LCD_IRQ_ENTER();
    bcm_write32(BCM_PANEL_CTL_ADDR,
                bcm_read32(BCM_PANEL_CTL_ADDR) & ~BCM_PANEL_CTL_ENABLE);
    bcm_write32(BCMA_COMMAND, BCMCMD_LCD_SLEEP);
    mmio_write16(BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);
    LCD_IRQ_EXIT();

    /*
     * Latch the slept state BEFORE anyone can present again. While it is set,
     * lcd_fill / lcd_present_rect refuse to stream: pushing a ~150 KB pixel
     * stream at a panel that is powering down (or, worse, into the panel-init
     * on the way back up) is exactly what wedges the BCM.
     */
    lcd_slept = 1;
    bcm_wall_delay(BCM_PANEL_SETTLE_US);
}

void lcd_wake(void)
{
    if (!lcd_slept) {
        return;
    }
    LCD_IRQ_ENTER();
    bcm_write32(BCM_PANEL_CTL_ADDR,
                bcm_read32(BCM_PANEL_CTL_ADDR) | BCM_PANEL_CTL_ENABLE);
    LCD_IRQ_EXIT();

    /* Let the panel-enable bits take effect before the caller presents. */
    bcm_wall_delay(BCM_PANEL_SETTLE_US);

    lcd_slept     = 0;
    /*
     * Arm the absorb window. The NEXT commit is the one the BCM answers
     * slowly (up to 500 ms of internal panel init, 02-lcd.md) and it must not
     * be re-kicked — see bcm_frame_commit. Note for callers: the backlight
     * should not be brought up until that first present has returned, or the
     * user sees the panel-init as a white flash (02-lcd.md: "If we wake the
     * backlight before the first update completes, the user sees a 500 ms
     * white flash").
     */
    lcd_post_wake = 1;
}

int lcd_is_slept(void)
{
    return lcd_slept;
}

/* ---- BCM bootstrap and firmware upload (RECOVERY PATH ONLY) ----------------
 *
 * Everything below implements 02-lcd.md, "Bootstrap and firmware upload"
 * (lines 114-181) — the sequence that takes a powered-off or wedged BCM,
 * uploads the `vmcs` firmware blob out of flash ROM, and starts it. Rockbox
 * runs it on cold boot and again inside lcd_awake() after its panel-off
 * power-gates the BCM (lines 450-495).
 *
 * WE DO NOT RUN IT ON BOOT. The chainload handoff (02-lcd.md, "Chainload
 * handoff state") already leaves the BCM powered, bootstrapped and idle, and
 * lcd_init() still relies on exactly that. This exists because the converse
 * has no answer today: if the BCM ever wedges there is no code anywhere that
 * can bring it back, and on a unit with no serial cable the framebuffer
 * console is the only debug channel — an unrecoverable LCD is an
 * unrecoverable device. So it lands as a deliberate, gated recovery entry
 * point (lcd_recover) rather than on the path every boot depends on.
 *
 * WHICH STATE WE RECOVER FROM — this matters, and it is the one place we
 * knowingly deviate from the doc's sequence. The doc's Stage 1 opens with
 * `GPO32_VAL |= 0x4000` because Rockbox always arrives here from its own
 * bcm_powerdown(), which cut that rail (line 460). OUR lcd_sleep()
 * deliberately does NOT power-gate — it is the recoverable subset — so on our
 * device the rail is still ASSERTED when the BCM wedges, and a bare OR would
 * be a no-op that left the BCM's internal state exactly as stuck as it was.
 * bcm_init() therefore power-CYCLES: drop the rail (the doc's own powerdown
 * step), hold it off, then raise it and take the documented 50 ms settle. That
 * makes the entry precondition true regardless of which state we came from,
 * which is the whole point of a recovery path.
 *
 * BOUNDED WAITS. Every `while (...)` in the doc's listing is unbounded, and
 * the core switches between 30 and 80 MHz, so iteration counts do not
 * translate into time here. All the new waits are wall-clock, on USEC_TIMER,
 * following bcm_wait_idle_wall/bcm_wall_delay above. The pre-existing
 * bcm_write_addr / bcm_write32 / bcm_read32 helpers keep their trip-count
 * bounds unchanged — they are already bounded, and re-basing them would
 * change the register grammar of the shipping present path, which the golden
 * traces pin deliberately.
 *
 * The timeout VALUES are ours: the doc records no timeouts for any of these
 * waits. They are sized as "far past any plausible healthy answer".
 */

/* BCM-INTERNAL addresses. 02-lcd.md files these under "Magic constants we
 * don't fully understand" (lines 173-179) — they came from iPodLinux
 * reverse-engineering with no semantics beyond "this is what works". Kept
 * here beside their use, next to the existing BCM_PANEL_CTL_ADDR, rather
 * than in pp5022.h which describes PP registers. */
#define BCM_SDRAM_MAP_ADDR      0x10000C00u  /* BCM SDRAM mapping control  */
#define BCM_SDRAM_MAP_ENABLE    0xC0000000u  /* line 156: map BCM SDRAM    */
#define BCM_SDRAM_MAP_DONE      0x00000001u  /* line 157: bit 0 = mapped   */
#define BCM_FW_START_ADDR       0x10000400u  /* firmware startup trigger   */
#define BCM_FW_START_MAGIC      0xA5A50002u  /* line 161                   */

/* Wall-clock budgets for the bootstrap waits. The doc gives none of these;
 * each is chosen well past any healthy answer so a bounded wait never turns a
 * slow BCM into a failed recovery. */
#define BCM_POWER_OFF_US        50000u   /* rail low before re-asserting; the
                                          * >= 50 ms "since last power-off"
                                          * guard from lcd_awake (line 471) */
#define BCM_POWER_SETTLE_US     50000u   /* the doc's sleep(HZ/20), line 122 */
#define BCM_HANDSHAKE_US       100000u   /* alt-control busy/ready waits     */
#define BCM_SDRAM_MAP_US       100000u   /* SDRAM mapping (line 157)         */
#define BCM_FW_START_US        500000u   /* firmware boot (line 162); scaled
                                          * to the panel-init order of
                                          * magnitude, the only BCM latency
                                          * the doc quantifies at all      */

/* Smallest byte count we will believe is a real firmware blob. Arbitrary —
 * the doc records no size for the vmcs section. Its only job is to reject a
 * zeroed or garbage directory entry, so it is set low enough that no
 * plausible real blob trips it. */
#define BCM_VMCS_MIN_BYTES      64u

/*
 * The 8-byte bootstrap handshake (02-lcd.md lines 132-134). Sent whole to
 * BCM_CONTROL, then bytes 3..7 again to BCM_ALT_CONTROL (lines 135-136).
 * The doc types it u8 while both ports are the 16-bit views, so these go out
 * as 16-bit writes of byte values — matching how the driver already writes
 * the 0x31 strobe to BCM_CONTROL.
 */
static const uint8_t bcm_boot_seq[8] = {
    0xA1, 0x81, 0x91, 0x02, 0x12, 0x22, 0x72, 0x62
};
#define BCM_BOOT_SEQ_ALT_FIRST  3   /* line 136: the alt port replays 3..7 */

/* Wall-clock bounded poll of a 16-bit BCM status port until `mask` reads set
 * (want_set) or clear (!want_set). Returns 0 if the condition was met. */
static int bcm_wait16(uintptr_t port, uint16_t mask, int want_set, uint32_t us)
{
    uint32_t t0    = mmio_read32(USEC_TIMER_ADDR);
    uint32_t guard = BCM_WALL_GUARD_TRIPS;

    while (((mmio_read16(port) & mask) != 0) != (want_set != 0)) {
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) > us) {
            return -1;
        }
        if (--guard == 0) {
            return -1;
        }
    }
    return 0;
}

/* Wall-clock bounded poll of a BCM-INTERNAL word until any of `mask` is set
 * (via the full bcm_read32 handshake). Returns 0 if it came up. */
static int bcm_wait_internal(uint32_t bcm_addr, uint32_t mask, uint32_t us)
{
    uint32_t t0    = mmio_read32(USEC_TIMER_ADDR);
    uint32_t guard = BCM_WALL_GUARD_TRIPS;

    while ((bcm_read32(bcm_addr) & mask) == 0) {
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) > us) {
            return -1;
        }
        if (--guard == 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * Locate and validate the BCM firmware blob in flash ROM.
 *
 * 02-lcd.md lines 165-171 say the `vmcs` section is found "via the flash
 * directory at 0x200FFE00" using flash_get_section(ROM_ID('v','m','c','s')),
 * and that its offset and length end up in flash_vmcs_offset /
 * flash_vmcs_length.
 *
 * >>> UNVERIFIED: THE DIRECTORY ENTRY LAYOUT IS NOT IN THE DOC. <<<
 * The doc names the directory's address and the lookup key and stops there —
 * it gives no record size, no field order, and no tag byte order. So this
 * scans the directory region for the four-character tag at 4-byte alignment
 * and assumes the two words that FOLLOW it are {offset, length}. That
 * assumption is the single most likely thing in this file to be wrong, and
 * it is why nothing is uploaded until it has been validated:
 *
 *   - both tag byte orders are accepted (the doc pins neither);
 *   - the offset is taken as ROM-relative if it is below the ROM base and
 *     absolute otherwise (flash_get_section could plausibly return either);
 *   - the result must land wholly inside the 1 MB ROM window, be at least
 *     BCM_VMCS_MIN_BYTES long, and be 4-byte aligned (the upload streams
 *     32-bit loads; an unaligned blob would fault on ARM);
 *   - a tag whose following words fail any of that is treated as a false
 *     positive and the scan CONTINUES, rather than trusting it.
 *
 * A blob we cannot validate is reported, never uploaded. Pushing garbage into
 * the BCM's SRAM and then starting its processor on it is the one outcome
 * strictly worse than leaving the panel wedged.
 *
 * ROM words are read through mmio_read32 rather than a pointer deref for two
 * reasons: it is the same volatile load on the firmware target, and it makes
 * the locator observable to the golden-trace test (a plain deref would be a
 * wild host pointer).
 */
static int bcm_find_vmcs(uint32_t *out_addr, uint32_t *out_len)
{
    const uint32_t rom_end = FLASH_ROM_BASE + FLASH_ROM_SIZE;
    const uint32_t words   = FLASH_DIR_BYTES / 4u;
    int saw_tag = 0;

    /* Stop 2 words early: a match needs its two following words to exist. */
    for (uint32_t i = 0; i + 2u < words; i++) {
        uint32_t tag = mmio_read32(FLASH_DIR_BASE + 4u * i);
        if (tag != FLASH_ID_VMCS_BE && tag != FLASH_ID_VMCS_LE) {
            continue;
        }
        saw_tag = 1;

        uint32_t off  = mmio_read32(FLASH_DIR_BASE + 4u * (i + 1u));
        uint32_t len  = mmio_read32(FLASH_DIR_BASE + 4u * (i + 2u));
        uint32_t addr = (off < FLASH_ROM_BASE) ? (FLASH_ROM_BASE + off) : off;

        if (addr < FLASH_ROM_BASE || addr >= rom_end) {
            continue;                       /* not a ROM address    */
        }
        if (len < BCM_VMCS_MIN_BYTES || len > rom_end - addr) {
            continue;                       /* implausible / overruns ROM */
        }
        if ((addr & 3u) != 0u) {
            continue;                       /* 32-bit loads must be aligned */
        }

        *out_addr = addr;
        *out_len  = len;
        return LCD_BCM_OK;
    }
    return saw_tag ? LCD_BCM_ERR_BAD_BLOB : LCD_BCM_ERR_NO_BLOB;
}

int bcm_init(void)
{
    uint32_t blob_addr = 0;
    uint32_t blob_len  = 0;
    int rc;

    /* Validate BEFORE touching any hardware: a bootstrap we cannot finish
     * leaves the BCM worse off than not starting it (the power cycle below
     * discards whatever state it still had). */
    rc = bcm_find_vmcs(&blob_addr, &blob_len);
    if (rc != LCD_BCM_OK) {
        lcd_warn("lcd: no usable BCM firmware (vmcs) in flash ROM\n");
        return rc;
    }

    /* ---- Stage 1 — power (02-lcd.md lines 120-125) --------------------
     * The power CYCLE, not the doc's bare OR — see the deviation note in
     * this section's header comment. Then the strap config: clear
     * STRAP_OPT_A bits 0xF00 and drive the boot strap pins. */
    mmio_write32(GPO32_VAL_ADDR,
                 mmio_read32(GPO32_VAL_ADDR) & ~(uint32_t)GPO32_BCM_POWER);
    bcm_wall_delay(BCM_POWER_OFF_US);
    mmio_write32(GPO32_VAL_ADDR,
                 mmio_read32(GPO32_VAL_ADDR) | (uint32_t)GPO32_BCM_POWER);
    bcm_wall_delay(BCM_POWER_SETTLE_US);

    mmio_write32(STRAP_OPT_A_ADDR,
                 mmio_read32(STRAP_OPT_A_ADDR) & ~(uint32_t)STRAP_OPT_A_BCM_MASK);
    mmio_write32(STRAP_BOOT_PINS_ADDR, STRAP_BOOT_PINS_BCM);

    /* ---- Stage 2 — wait for the BCM, then handshake (lines 127-143) ---
     * Busy must clear before ready may set; both gate the 8-byte sequence. */
    if (bcm_wait16(BCM_ALT_CONTROL_ADDR, BCM_ALT_CONTROL_BUSY, 0,
                   BCM_HANDSHAKE_US) != 0) {
        lcd_warn("lcd: BCM bootstrap stalled (alt busy, stage 2)\n");
        return LCD_BCM_ERR_ALT_BUSY;
    }
    if (bcm_wait16(BCM_ALT_CONTROL_ADDR, BCM_ALT_CONTROL_READY, 1,
                   BCM_HANDSHAKE_US) != 0) {
        lcd_warn("lcd: BCM bootstrap stalled (alt not ready, stage 2)\n");
        return LCD_BCM_ERR_ALT_READY;
    }

    for (uint32_t i = 0; i < sizeof bcm_boot_seq; i++) {
        mmio_write16(BCM_CONTROL_ADDR, bcm_boot_seq[i]);
    }
    for (uint32_t i = BCM_BOOT_SEQ_ALT_FIRST; i < sizeof bcm_boot_seq; i++) {
        mmio_write16(BCM_ALT_CONTROL_ADDR, bcm_boot_seq[i]);
    }

    /* Post-handshake sync (lines 138-143): wait until BOTH read ports report
     * ready, then dummy-read both write-address ports to flush them. The
     * short-circuit && is the doc's own — the alt port is not sampled until
     * the main one is ready. */
    {
        uint32_t t0    = mmio_read32(USEC_TIMER_ADDR);
        uint32_t guard = BCM_WALL_GUARD_TRIPS;

        while (!((mmio_read16(BCM_RD_ADDR_ADDR)     & BCM_RD_ADDR_READY) &&
                 (mmio_read16(BCM_ALT_RD_ADDR_ADDR) & BCM_RD_ADDR_READY))) {
            if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) >
                BCM_HANDSHAKE_US || --guard == 0) {
                lcd_warn("lcd: BCM read ports never came ready\n");
                return LCD_BCM_ERR_PORTS;
            }
        }
    }
    (void)mmio_read16(BCM_WR_ADDR_ADDR);
    (void)mmio_read16(BCM_ALT_WR_ADDR_ADDR);

    /* ---- Stage 3 — upload the firmware blob (lines 145-152) -----------
     * The same busy/ready gate again, then point the BCM write port at its
     * SRAM base and stream the blob. */
    if (bcm_wait16(BCM_ALT_CONTROL_ADDR, BCM_ALT_CONTROL_BUSY, 0,
                   BCM_HANDSHAKE_US) != 0) {
        lcd_warn("lcd: BCM bootstrap stalled (alt busy, upload)\n");
        return LCD_BCM_ERR_UPLOAD_BUSY;
    }
    if (bcm_wait16(BCM_ALT_CONTROL_ADDR, BCM_ALT_CONTROL_READY, 1,
                   BCM_HANDSHAKE_US) != 0) {
        lcd_warn("lcd: BCM bootstrap stalled (alt not ready, upload)\n");
        return LCD_BCM_ERR_UPLOAD_READY;
    }

    bcm_write_addr(BCMA_SRAM_BASE);
    {
        /* Line 151: the upload length rounds to an EVEN number of 16-bit
         * units, ((len + 3) >> 1) & ~1 — i.e. a whole number of the 32-bit
         * stores this bus actually takes, so halve it for the word count.
         * The "& ~1" is redundant once halved (the shift discards bit 0
         * regardless); it is kept so this reads as the doc's formula rather
         * than as a simplification a reader would have to re-derive. */
        const uint32_t units16 = ((blob_len + 3u) >> 1) & ~1u;
        const uint32_t words32 = units16 >> 1;

        for (uint32_t k = 0; k < words32; k++) {
            mmio_write32(BCM_DATA_ADDR, mmio_read32(blob_addr + 4u * k));
        }
    }

    /* ---- Initialize the BCM processor (lines 154-158) ------------------ */
    bcm_write32(BCMA_COMMAND, 0);
    bcm_write32(BCM_SDRAM_MAP_ADDR, BCM_SDRAM_MAP_ENABLE);
    if (bcm_wait_internal(BCM_SDRAM_MAP_ADDR, BCM_SDRAM_MAP_DONE,
                          BCM_SDRAM_MAP_US) != 0) {
        lcd_warn("lcd: BCM SDRAM mapping never completed\n");
        return LCD_BCM_ERR_SDRAM;
    }
    bcm_write32(BCM_SDRAM_MAP_ADDR, 0);

    /* ---- Start BCM firmware execution (lines 160-162) ------------------
     * The firmware announces itself by making BCMA_COMMAND read nonzero. */
    bcm_write32(BCM_FW_START_ADDR, BCM_FW_START_MAGIC);
    if (bcm_wait_internal(BCMA_COMMAND, 0xFFFFFFFFu, BCM_FW_START_US) != 0) {
        lcd_warn("lcd: BCM firmware never started\n");
        return LCD_BCM_ERR_START;
    }

    return LCD_BCM_OK;
}

uint32_t lcd_bcm_recoveries(void)
{
    return lcd_recoveries;
}

int lcd_recover(void)
{
    int rc;

    /* lcd_fill() below re-enters bcm_frame_commit, which is one of our
     * callers. One recovery at a time. */
    if (lcd_in_recover) {
        return LCD_BCM_ERR_BUSY;
    }
    lcd_in_recover = 1;

    /* Count ATTEMPTS, not successes: a recovery that ran and failed is the
     * single most useful thing a debug screen could show. */
    lcd_recoveries++;

    /* The host-side port setup "always runs", separately from the BCM
     * bringup (02-lcd.md, "Host-side port init") — and we are about to
     * re-drive the very GPO bit it configures, so re-run it first. */
    lcd_port_init();

    rc = bcm_init();
    if (rc == LCD_BCM_OK) {
        /*
         * Re-establish panel + driver state. The BCM's SDRAM framebuffer did
         * NOT survive the power cycle, so whatever it is scanning out now is
         * undefined; drive one known full frame at it rather than leaving
         * garbage on screen. lcd.c holds no back buffer, so black is the only
         * frame we can produce here — the caller repaints.
         *
         * lcd_first_frame is re-armed so that fill's commit takes the plain
         * post-handoff path (nothing is in flight on a BCM that just started),
         * and lcd_post_wake is cleared across it so the fill does not try to
         * absorb a panel init that has not been kicked off yet. It is set
         * again afterwards because that fill IS what kicks the panel init
         * off — so the NEXT commit is the slow one, exactly as after a wake.
         */
        lcd_slept      = 0;
        lcd_first_frame = 1;
        lcd_post_wake   = 0;
        lcd_fill(0x0000);
        lcd_post_wake   = 1;
    }

    lcd_in_recover = 0;
    return rc;
}
