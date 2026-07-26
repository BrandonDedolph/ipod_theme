/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kernel/clock.h — PP5022 clock / PLL / CPU-boost driver interface.
 *
 * The core boots on the 24 MHz crystal with the PLL bypassed
 * (CPUFREQ_DEFAULT). clock_init() moves it to the 30 MHz unboosted UI
 * operating point; codec / decode-heavy work brackets itself in
 * cpu_boost()/cpu_unboost() to run at 80 MHz, refcounted so nested
 * requests collapse to a single frequency change.
 *
 * Single-core: this build parks the COP, so the frequency-switch
 * sequence OMITS the scale_suspend_core steps the dual-core reference
 * uses to keep the other core coherent across a clock change.
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only.
 */

#ifndef CORE_KERNEL_CLOCK_H
#define CORE_KERNEL_CLOCK_H

#include <stdint.h>

#define CPUFREQ_DEFAULT 24000000u   /* crystal, PLL bypassed (boot state) */
#define CPUFREQ_NORMAL  30000000u   /* unboosted UI                       */
#define CPUFREQ_MAX     80000000u   /* boosted: codecs                    */

/* Move the core from its 24 MHz boot clock to CPUFREQ_NORMAL (30 MHz).
 * Idempotent: safe to run the full sequence from any current state.
 * Call once after uart_init, before the scheduler starts. */
void clock_init(void);

/* Boost refcount 0->1 edge: raise the core to CPUFREQ_MAX (80 MHz).
 * Nested calls only bump the counter. */
void cpu_boost(void);

/* Boost refcount 1->0 edge: drop the core back to CPUFREQ_NORMAL. */
void cpu_unboost(void);

/* Current core frequency in Hz (one of the CPUFREQ_* values). */
uint32_t cpu_frequency(void);

/*
 * Tell the clock driver whether the audio DMA is currently streaming PCM out
 * of SDRAM. While it is, cpu_boost/cpu_unboost/clock_init become no-ops: the
 * frequency switch reprograms DEV_TIMING1 (SDRAM/peripheral bus timing) and
 * detours CLOCK_SOURCE through the crystal while the PLL relocks, which is not
 * something a DMA master reading SDRAM can be exposed to. Called from
 * hal/hw/audio.c on start/stop; the refusal is silent and cpu_frequency()
 * keeps reporting the frequency actually in effect.
 */
void clock_set_audio_dma_active(int active);

#endif /* CORE_KERNEL_CLOCK_H */
