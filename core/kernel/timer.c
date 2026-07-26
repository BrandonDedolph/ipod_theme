/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kernel/timer.c — TIMER1 100 Hz system tick driver (PP5022).
 *
 * Implements the arm/ack sequence from core/docs/hw/01-soc-pp5022.md
 * ("Timers and the system tick"). The 1 MHz timer clock makes one count
 * one microsecond, so a HZ-hertz tick has a reload of (TIMER_FREQ/HZ)-1
 * microseconds; bit 30 makes the counter self-reload, so the ISR only
 * has to acknowledge (a read of TIMER1_VAL) and never reprograms CFG.
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only,
 * hardware reached solely through hal/hw/mmio.h so the register grammar
 * is trace-testable host-side under -DMMIO_MOCK.
 */

#include "timer.h"
#include "sched.h"
#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/clickwheel.h"

/* Backlight housekeeping hook, run from the tick. Weak: the host timer test
 * links this file without hal/hw/backlight.c, and the call is guarded. */
__attribute__((weak)) void backlight_service(void);

/* Monotonic tick counter. In .bss, zeroed by crt0. volatile: written by
 * the ISR, read by mainline code (current_tick / sleep_ms). */
static volatile uint32_t g_tick;

/*
 * TICK RECONCILIATION against the free-running 1 MHz USEC_TIMER.
 *
 * TIMER1 self-reloads and the ISR acknowledges with a single TIMER1_VAL read,
 * so N elapsed periods collapse into ONE g_tick++ — the timer has no
 * accumulating count for us to read back. That is not hypothetical here: the
 * LCD present masks IRQs for longer than the 10 ms period while it streams a
 * frame, so every full-frame repaint loses at least one tick. Two consequences,
 * both real:
 *
 *   - sleep_ms() systematically over-sleeps. The 60 ms disk-retry backoff
 *     stretches without bound under repaint load, because the clock it is
 *     counting stops advancing while the screen is busy.
 *   - clickwheel_service() is skipped for exactly as long, i.e. input is
 *     sampled least often precisely when the UI is busiest.
 *
 * USEC_TIMER never stops and is not affected by the IRQ mask, so the ISR can
 * work out how many whole periods actually elapsed and advance g_tick by that
 * many. g_last_us keeps the sub-period remainder so there is no drift.
 */
#define TICK_PERIOD_US   (TIMER_FREQ / HZ)      /* 10000 us at HZ = 100 */

/* Ceiling on a single catch-up. A jump this large means the reference is
 * meaningless (first ISR after arm, or a stopped counter), not that a second
 * of ticks was really missed — advancing by a huge number would make every
 * outstanding sleep_ms() return instantly. */
#define TICK_MAX_CATCHUP 100u                   /* 1 second's worth */

static uint32_t g_last_us;      /* USEC_TIMER value at the last reconcile */
static int      g_have_us;      /* g_last_us seeded yet? */

void timer_init(void)
{
    /* 1. Disarm: clear any stale enable/reload. */
    mmio_write32(TIMER1_CFG_ADDR, 0);
    /* 2. Clear a pending IRQ latched from a prior arm. */
    (void)mmio_read32(TIMER1_VAL_ADDR);
    /* 3. Arm periodic: enable + IRQ/reload + (TIMER_FREQ/HZ)-1 us period. */
    mmio_write32(TIMER1_CFG_ADDR,
                 TIMER_CFG_ENABLE | TIMER_CFG_IRQEN | ((TIMER_FREQ / HZ) - 1u));
    /* 4. Unmask TIMER1_IRQ (#0) in the CPU interrupt-enable register. */
    mmio_write32(CPU_INT_EN_ADDR, 1u << TIMER1_IRQ);
}

void timer_tick_isr(void)
{
    /* Advance by however many whole tick periods have really elapsed, not by
     * one — see the reconciliation note above. The seeding read is done here
     * rather than in timer_init so the arm sequence stays exactly the four
     * documented accesses. */
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    if (!g_have_us) {
        g_have_us = 1;
        g_last_us = now;
        g_tick++;
    } else {
        uint32_t elapsed = now - g_last_us;      /* wrap-safe */
        if (elapsed >= TICK_PERIOD_US) {
            uint32_t ticks = elapsed / TICK_PERIOD_US;
            if (ticks > TICK_MAX_CATCHUP) {
                ticks = TICK_MAX_CATCHUP;
                g_last_us = now;                 /* reference was stale: resync */
            } else {
                /* Keep the remainder so the tick clock does not drift. */
                g_last_us += ticks * TICK_PERIOD_US;
            }
            g_tick += ticks;
        } else {
            /* The IRQ fired slightly early against the microsecond counter
             * (jitter, or a counter that does not advance at all — the host
             * mock). It is still a real tick: count it and resync. */
            g_tick++;
            g_last_us = now;
        }
    }
    (void)mmio_read32(TIMER1_VAL_ADDR);   /* ack: clears the pending IRQ */

    /* Sample the click wheel every tick (10 ms). Running this from the tick
     * — not the main loop — is what keeps a quick face-button tap from being
     * lost when the main loop is blocked in a ~100 ms synchronous disk read:
     * the tick still fires, so clickwheel_service() latches the press for the
     * loop to drain later. Bounded work (one OPTO packet); a no-op until
     * clickwheel_init() arms it. */
    clickwheel_service();

    /* Backlight screen-off power policy (drop the charge pump once the panel
     * has been dark for a few seconds). At most one bus write per call.
     * Declared weak so the host timer test — which links kernel/timer.c
     * without the hal/hw backlight driver — still resolves. */
    if (backlight_service) {
        backlight_service();
    }
}

uint32_t current_tick(void)
{
    return g_tick;
}

/*
 * Halt this core until its countdown expires (or an interrupt arrives).
 * PROC_WAIT_CNT self-wakes on the countdown, so unlike PROC_SLEEP it is safe
 * even if no interrupt is pending (01-soc-pp5022.md, "Sleep / wake"). Three
 * NOPs after the write per the doc's pipeline rule. 1 ms keeps sleep_ms's
 * effective granularity well inside one 10 ms tick.
 */
#define SLEEP_HALT_MS  1u

static void tick_halt(void)
{
    mmio_write32(CPU_CTL_ADDR, PROC_WAIT_CNT | PROC_CNT_MSEC | SLEEP_HALT_MS);
#ifndef MMIO_MOCK
    __asm__ volatile("nop\n\tnop\n\tnop");
#endif
}

void sleep_ms(uint32_t ms)
{
    /* PRECONDITION: IRQs must be enabled at the core — the wait ends only
     * when the timer ISR advances the tick, so calling this inside an
     * arch_irq_disable() critical section spins forever. Cooperative:
     * other tasks run while it waits.
     *
     * Round up to whole ticks (ceil); the 64-bit intermediate keeps a
     * large ms from overflowing the multiply. */
    uint32_t ticks = (uint32_t)(((uint64_t)ms * HZ + 999u) / 1000u);
    uint32_t start = current_tick();

    /* Unsigned subtraction is wrap-safe across the UINT32_MAX boundary. */
    while ((current_tick() - start) < ticks) {
        /*
         * HALT, then yield. On the shipping path the scheduler never actually
         * runs — run_ui() never returns, so sched_start() is only reached on
         * the no-LCD / mount-failure fallback — which made sched_yield() an
         * immediate return and this loop a flat-out 80 MHz busy-spin for the
         * whole delay. Every sleep_ms burned full power for nothing.
         *
         * Halting first costs at most SLEEP_HALT_MS of scheduling latency when
         * a scheduler IS running (the only task there is an idle task that
         * halts anyway) and turns the shipping path into an actual sleep. We
         * cannot ask the scheduler whether it is live — kernel/sched.c is not
         * ours to change and its state is private — so doing both
         * unconditionally is the correct, non-invasive answer.
         */
        tick_halt();
        sched_yield();
    }
}

#ifdef MMIO_MOCK
/*
 * Host-test-only hook. Seeds the tick counter so a test can exercise
 * paths (e.g. the unsigned wrap in sleep_ms) that would otherwise
 * require billions of real ticks to reach. Compiled out of the
 * freestanding firmware image entirely — the arm-none-eabi build never
 * defines MMIO_MOCK, so g_tick stays a pure ISR-owned counter there.
 */
void timer_test_set_tick(uint32_t v)
{
    g_tick    = v;
    g_have_us = 0;      /* re-seed the USEC reference for the next ISR */
}
#endif
