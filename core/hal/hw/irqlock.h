/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/irqlock.h — tiny nestable critical section for the hw drivers.
 *
 * WHY THIS EXISTS
 * ---------------
 * Four SoC registers are shared read-modify-write state between the 100 Hz
 * timer ISR and mainline code:
 *
 *   DEV_EN    (0x6000600C)  peripheral clock gates   — i2c, i2s, opto, pwm
 *   DEV_RS    (0x60006004)  peripheral resets        — i2c, i2s, opto
 *   DEV_INIT1 (0x70000020)  pad-function select      — i2s pads, pwm route,
 *                                                      wheel button latch
 *   DEV_INIT2 (0x70000024)  pad-function select      — i2s/cdi pads, pll power
 *
 * clickwheel_service() drives DEV_EN / DEV_RS / DEV_INIT1 from the timer ISR
 * (it gates the OPTO block on every Hold edge), while i2s_init/i2s_disable,
 * i2c_init and piezo_init drive the same registers from thread context. Every
 * one of those is a non-atomic read-modify-write, so an ISR landing between
 * the read and the write silently discards the mainline update:
 *
 *   - lose the DEV_INIT2/DEV_INIT1 I2S pad routing and the codec still ACKs on
 *     I2C but no clocks/data reach it — silent playback until reboot
 *     (i2s.c calls that write "the load-bearing why-is-it-silent write");
 *   - lose the DEV_EN OPTO gate and the wheel is dead until reboot.
 *
 * The realistic trigger is mundane: flip Hold while a track auto-advances.
 *
 * The mask is the ARMv4T (ARM7TDMI) mrs/orr|bic/msr read-modify-write —
 * mirroring kernel/irq.h's arch_irq_save/restore, but kept here so hal/hw
 * stays include-clean (it is not on the kernel include path) and so the whole
 * thing compiles out under -DMMIO_MOCK, where the host golden-trace tests have
 * no CPSR to mask. Because it emits NO bus traffic it is invisible to those
 * traces.
 */
#ifndef CORE_HAL_HW_IRQLOCK_H
#define CORE_HAL_HW_IRQLOCK_H

#include <stdint.h>

#include "pp5022.h"   /* CPSR_I_BIT */

/* Mask IRQs at the core; returns the prior I-bit for a nestable section. */
static inline uint32_t hw_irq_save(void)
{
#ifndef MMIO_MOCK
    uint32_t cpsr, tmp;
    __asm__ volatile(
        "mrs %0, cpsr\n\t"
        "orr %1, %0, %2\n\t"
        "msr cpsr_c, %1\n\t"
        : "=&r"(cpsr), "=&r"(tmp)
        : "i"(CPSR_I_BIT)
        : "cc", "memory");
    return cpsr & CPSR_I_BIT;   /* prior I-bit: 0 == IRQs were enabled */
#else
    return 0;
#endif
}

/* Restore the I-bit captured by hw_irq_save(). */
static inline void hw_irq_restore(uint32_t saved)
{
#ifndef MMIO_MOCK
    uint32_t cpsr, tmp;
    __asm__ volatile(
        "mrs %0, cpsr\n\t"
        "bic %1, %0, %3\n\t"
        "orr %1, %1, %2\n\t"
        "msr cpsr_c, %1\n\t"
        : "=&r"(cpsr), "=&r"(tmp)
        : "r"(saved), "i"(CPSR_I_BIT)
        : "cc", "memory");
#else
    (void)saved;
#endif
}

#endif /* CORE_HAL_HW_IRQLOCK_H */
