/* SPDX-License-Identifier: Apache-2.0 */
/*
 * kernel/clock.c — PP5022 clock / PLL / CPU-boost driver.
 *
 * Implements the frequency-switch sequence from
 * core/docs/hw/01-soc-pp5022.md ("Clock tree" -> "Frequency switch
 * sequence"). The PLL is driven off the 24 MHz crystal; a switch always
 * detours the bus through the crystal first (step 3 below) so the PLL
 * can be reprogrammed while the core keeps running, which also makes
 * every switch self-contained and idempotent.
 *
 * SINGLE-CORE: the COP is parked in this build, so the two
 * scale_suspend_core() steps the dual-core reference uses (to stall the
 * other core while clocks move) are OMITTED — there is no other core
 * observing the change.
 *
 * PP5022 vs PP5020: the 11-step list in an earlier doc revision was the
 * PP5020 sequence. On PP5022 (confirmed by the cleanroom dig) there is
 * NO pre-stage PLL_CONTROL write — the target value is programmed in a
 * single write — and DEV_TIMING1 is target-specific and set to its
 * operating value BEFORE routing CLOCK_SOURCE to the PLL.
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only,
 * hardware reached solely through hal/hw/mmio.h so the register grammar
 * is trace-testable host-side under -DMMIO_MOCK.
 */

#include "clock.h"
#include "hw/pp5022.h"
#include "hw/mmio.h"

/*
 * Upper bound on the PLL-lock poll so a dead PLL — or the clicky
 * emulator, which never sets the lock bit — can't hang the kernel. The
 * PLL locks in well under a millisecond of real hardware time; 1<<20
 * iterations is multiple milliseconds even at 80 MHz, orders of
 * magnitude past "working hardware". On timeout we proceed anyway: a
 * missing lock must degrade (run on a possibly-unlocked PLL) rather than
 * hang, matching uart.c's UART_TX_SPIN_LIMIT discipline.
 *
 * Under -DMMIO_MOCK the recording bus logs every access into a
 * fixed-capacity array; a full 1<<20-iteration "never locks" spin would
 * record ~1M reads and overrun it. The host build therefore uses a much
 * smaller (but still real) bound so the never-locks path can be exercised
 * within the log capacity. The freestanding firmware always gets 1<<20.
 */
#ifdef MMIO_MOCK
#define PLL_LOCK_SPIN_LIMIT  1024u
#else
#define PLL_LOCK_SPIN_LIMIT  (1u << 20)
#endif

/* Current core frequency; starts at the crystal boot clock. */
static uint32_t g_freq = CPUFREQ_DEFAULT;
/* Boost refcount: >0 means at least one outstanding cpu_boost(). */
static int g_boost;

/*
 * Set while the audio DMA is streaming PCM out of SDRAM (hal/hw/audio.c calls
 * clock_set_audio_dma_active on start/stop).
 *
 * set_cpu_frequency reprograms DEV_TIMING1 — the SDRAM/peripheral bus timing —
 * and routes CLOCK_SOURCE through the crystal and back while the PLL is
 * relocked. A DMA master reading SDRAM across that window is reading through
 * timing that is being rewritten underneath it. Today that never happens, but
 * only INCIDENTALLY: the one caller happens to check !player_active() first,
 * and it does so for power reasons, not correctness ones. Anyone adding a
 * boost anywhere else — the ATA driver now brackets transfers in one — would
 * silently inherit the hazard.
 *
 * So the guard belongs here, at the register sequence that is actually unsafe,
 * not in the callers. Refusing is the right answer rather than quiescing: the
 * DMA cannot be paused without a gap in the audio, and a frequency change is
 * always optional.
 */
static volatile int g_dma_active;

void clock_set_audio_dma_active(int active)
{
    g_dma_active = active ? 1 : 0;
}

/*
 * Run the PP5022 frequency switch to the given PLL_CONTROL target and
 * record the resulting core frequency. `operating_timing` is the
 * DEV_TIMING1 value for the target point (SLOW for 30 MHz, FAST for
 * 80 MHz) applied once the PLL is locked and before routing to it. The
 * scale_suspend_core steps are omitted (single core). Step 3 forces
 * CLOCK_SOURCE back to the crystal first, so this is safe to call from
 * any current clock state.
 */
static void set_cpu_frequency(uint32_t pll_value, uint32_t operating_timing,
                              uint32_t new_freq_hz)
{
    uint32_t spin;

    /* 0. Refuse outright while audio DMA is streaming from SDRAM — see
     *    g_dma_active. Nothing is written, and g_freq keeps reporting the
     *    frequency we are actually running at. */
    if (g_dma_active) {
        return;
    }

    /* 1. Power up the PLL (preserve the other DEV_INIT2 bits). */
    mmio_write32(DEV_INIT2_ADDR,
                 mmio_read32(DEV_INIT2_ADDR) | DEV_INIT2_PLL_POWER);
    /* 2. Enable the PLL (preserve the other PLL_CONTROL bits). */
    mmio_write32(PLL_CONTROL_ADDR,
                 mmio_read32(PLL_CONTROL_ADDR) | PLL_CONTROL_ENABLE);
    /* 3. Detour the bus onto the 24 MHz crystal (no PLL dependency)
     *    while the PLL is reprogrammed. */
    mmio_write32(CLOCK_SOURCE_ADDR, CLOCK_SOURCE_XTAL);
    /* 4. Relax memory/peripheral timing before reprogramming the PLL. */
    mmio_write32(DEV_TIMING1_ADDR, DEV_TIMING1_SLOW);
    /* 5. Program the PLL for the target operating point in a single
     *    write (PP5022 has no pre-stage write). */
    mmio_write32(PLL_CONTROL_ADDR, pll_value);
    /* 6. Bounded spin until the PLL reports lock. */
    spin = PLL_LOCK_SPIN_LIMIT;
    while (!(mmio_read32(PLL_STATUS_ADDR) & PLL_STATUS_LOCK) && --spin != 0) {
        /* poll */
    }
    if (spin == 0) {
        /* PLL never locked (a dead PLL on silicon, or an emulator that
         * doesn't model lock). DEGRADE to the stable crystal rather than
         * routing the core + memory bus onto an unlocked PLL: step 3
         * already left us on the 24 MHz crystal with relaxed timing, so
         * we simply stay there and report the true frequency. */
        g_freq = CPUFREQ_DEFAULT;
        return;
    }
    /* 7. Apply the target's operating timing before routing to the PLL. */
    mmio_write32(DEV_TIMING1_ADDR, operating_timing);
    /* 8. Route all domains to the PLL output. */
    mmio_write32(CLOCK_SOURCE_ADDR, CLOCK_SOURCE_PLL);

    g_freq = new_freq_hz;
}

void clock_init(void)
{
    set_cpu_frequency(PLL_CONTROL_30MHZ, DEV_TIMING1_SLOW, CPUFREQ_NORMAL);
}

void cpu_boost(void)
{
    if (g_boost++ == 0) {
        set_cpu_frequency(PLL_CONTROL_80MHZ, DEV_TIMING1_FAST, CPUFREQ_MAX);
    }
}

void cpu_unboost(void)
{
    if (g_boost > 0 && --g_boost == 0) {
        set_cpu_frequency(PLL_CONTROL_30MHZ, DEV_TIMING1_SLOW, CPUFREQ_NORMAL);
    }
}

uint32_t cpu_frequency(void)
{
    return g_freq;
}

#ifdef MMIO_MOCK
/*
 * Host-test-only hook. Restores the driver's static state to its
 * power-on values (24 MHz, no outstanding boost) so each test case
 * starts from a known point. Compiled out of the freestanding image —
 * the arm-none-eabi build never defines MMIO_MOCK.
 */
void clock_test_reset(void)
{
    g_freq       = CPUFREQ_DEFAULT;
    g_boost      = 0;
    g_dma_active = 0;
}
#endif
