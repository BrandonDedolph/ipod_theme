/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/cache.c — PP5022 unified-cache driver (see cache.h).
 *
 * Register sequence follows docs/hw/01-soc-pp5022.md ("Cache"), which mirrors
 * Rockbox init_cache/commit_dcache for PP502x. Deliberately does NOT set
 * CACHE_CTL_VECT_REMAP, so the ARM exception vectors stay at 0x0 (our crt0's).
 */

#include "cache.h"

#include "hw/pp5022.h"
#include "hw/mmio.h"

#include <stdint.h>

void cache_init(void)
{
    /* 1. Into init mode: clear ENABLE|RUN, set INIT. */
    uint32_t ctl = mmio_read32(CACHE_CTL_ADDR);
    ctl &= ~(uint32_t)(CACHE_CTL_ENABLE | CACHE_CTL_RUN);
    ctl |= CACHE_CTL_INIT;
    mmio_write32(CACHE_CTL_ADDR, ctl);

    /* 2. Cache priority for this core (CPU). */
    mmio_write32(CACHE_PRIORITY_ADDR,
                 mmio_read32(CACHE_PRIORITY_ADDR) | CACHE_PRIORITY_CPU);

    /* 3. Cacheable-region mask + the init operation. */
    mmio_write32(CACHE_MASK_ADDR, CACHE_INIT_MASK);
    mmio_write32(CACHE_OP_ADDR, CACHE_INIT_OP);

    /* 4. Enable + run. */
    mmio_write32(CACHE_CTL_ADDR,
                 mmio_read32(CACHE_CTL_ADDR) |
                     CACHE_CTL_INIT | CACHE_CTL_ENABLE | CACHE_CTL_RUN);
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop");

    /* 5. Prime: touch one byte per line across a readable cached region so the
     * cache comes up populated rather than cold. */
    volatile const uint8_t *p = (const volatile uint8_t *)CACHE_PRIME_SRC;
    for (uint32_t off = 0; off < CACHE_SIZE_BYTES; off += CACHE_LINE_BYTES) {
        (void)p[off];
    }
}

/*
 * Upper bound on the write-back poll. This function runs INSIDE the audio DMA
 * completion ISR (hal/hw/audio.c fill_buffer), so an unbounded spin here is
 * the one place in the tree where a wedged peripheral takes the whole device
 * down with no serial cable and no debugger — every other wait in the HAL is
 * bounded (ATA_BSY_SPIN_LIMIT, I2C_BUSY_SPIN_LIMIT, BCM_SPIN_LIMIT,
 * DMA_STOP_SPIN_LIMIT, UART_TX_SPIN_LIMIT, PLL_LOCK_SPIN_LIMIT) and this now
 * matches that discipline.
 *
 * An 8 KB write-back retires in microseconds; 1<<20 trips is milliseconds even
 * at 80 MHz, i.e. orders of magnitude past working hardware. On timeout we
 * PROCEED rather than hang: the worst case is the DMA reading one stale PCM
 * buffer (a glitch), which is strictly better than a frozen player.
 */
#define CACHE_BUSY_SPIN_LIMIT (1u << 20)

void cache_commit(void)
{
    if ((mmio_read32(CACHE_CTL_ADDR) & CACHE_CTL_ENABLE) == 0) {
        return;                     /* cache off — nothing to flush */
    }
    mmio_write32(CACHE_OP_ADDR, mmio_read32(CACHE_OP_ADDR) | CACHE_OP_FLUSH);
    uint32_t spin = CACHE_BUSY_SPIN_LIMIT;
    while ((mmio_read32(CACHE_CTL_ADDR) & CACHE_CTL_BUSY) && --spin != 0) {
        /* spin until the write-back completes (bounded — see above) */
    }
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop");
}
