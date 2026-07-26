/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/piezo.c — menu-click piezo via the PP5022 PWM0 unit.
 * See piezo.h for the register facts and rationale.
 */

#include "piezo.h"
#include "mmio.h"
#include "pp5022.h"          /* DEV_INIT1_ADDR, DEV_EN_ADDR, USEC_TIMER_ADDR */
#include "irqlock.h"         /* DEV_INIT1/DEV_EN RMW vs the timer ISR */

/* PWM facts not already in pp5022.h. DEV_PWM is DEV_EN bit 17, one above
 * DEV_OPTO (bit 16, the clickwheel). Addresses are hardware facts. */
#define DEV_INIT1_PWM_RT 0x0000000Cu   /* cleared to route PWM0 out to the piezo */
#define DEV_EN_PWM       0x00020000u   /* PWM peripheral clock enable            */
#define PWM0_CTRL_ADDR   0x7000A000u
#define PWM0_ON          0x80000000u   /* CTRL bit 31: run                       */

/* Click shape. inv_freq = 91225/hz sets the PWM period; `form` is the waveform
 * selector in the upper 16 bits. These match the iPod Video's own click; if the
 * feel needs tuning it's just these two constants + the duration. */
#define PIEZO_FORM       0x80u
#define PIEZO_CLICK_HZ   3000u          /* piezos only sound efficiently in the kHz
                                         * range — 40 Hz drove one sub-audible
                                         * deflection. ~3 kHz is near a typical
                                         * piezo resonance, so the burst is an
                                         * actual audible tick. */
#define PIEZO_CLICK_US   4000u          /* ~4 ms burst = ~12 cycles at 3 kHz: a
                                         * crisp click, still far shorter than any
                                         * load so it can't buzz, ~4 ms latency. */

void piezo_init(void)
{
    /* Route PWM0 to the piezo pin, then enable its peripheral clock. Both are
     * read-modify-writes of registers the timer ISR also touches (the wheel's
     * OPTO gate lives in DEV_EN, its button latch in DEV_INIT1), so the pair is
     * IRQ-masked — see irqlock.h. */
    uint32_t f = hw_irq_save();
    mmio_write32(DEV_INIT1_ADDR, mmio_read32(DEV_INIT1_ADDR) & ~DEV_INIT1_PWM_RT);
    mmio_write32(DEV_EN_ADDR,    mmio_read32(DEV_EN_ADDR) | DEV_EN_PWM);
    hw_irq_restore(f);
    mmio_write32(PWM0_CTRL_ADDR, 0);
}

/*
 * One self-contained click: drive PWM0 for a short burst, then stop — all in
 * this call. It deliberately does NOT depend on a later service pass to switch
 * off, because navigation often kicks off a BLOCKING operation right after
 * (e.g. loading the library index), during which the main loop can't run; a
 * deferred-stop scheme would leave the PWM buzzing for the whole load. The
 * burst is ~1.5 ms, so the busy-wait is imperceptible and can't underrun the
 * multi-second decode buffer even during rapid scrolling.
 */
void piezo_click_ex(uint32_t hz, uint32_t us)
{
    uint32_t inv = 91225u / (hz ? hz : 1u);
    mmio_write32(PWM0_CTRL_ADDR,
                 PWM0_ON | ((uint32_t)PIEZO_FORM << 16) | (inv & 0xFFFFu));
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < us) {
        /* spin */
    }
    mmio_write32(PWM0_CTRL_ADDR, 0);
}

void piezo_click(void)
{
    piezo_click_ex(PIEZO_CLICK_HZ, PIEZO_CLICK_US);
}
