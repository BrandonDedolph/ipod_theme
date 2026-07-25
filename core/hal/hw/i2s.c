/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/i2s.c — PP502x I2S serializer + TX FIFO, polled feed.
 *
 * Implements core/docs/hw/05-audio.md ("i2s_reset() config sequence",
 * "MCLK / clock-gating enable path", "Polled TX-FIFO write"). The
 * WM8758 is the I2S bus-clock master, so this side leaves IISCLK alone
 * and only: gates the block clock + codec MCLK, resets/configures the
 * FIFO, and pushes packed stereo frames when the TX FIFO has room.
 */

#include "pp5022.h"
#include "mmio.h"
#include "i2s.h"

/* Reset-hold spin, matching the other drivers' bounded holds. */
#define I2S_RESET_HOLD_SPIN (1u << 12)

/*
 * TX-FIFO free-slot poll budget. At 44.1 kHz a slot frees every ~22.7 us;
 * even at the 30 MHz boot-normal clock that is only a few thousand poll
 * trips. 1<<16 is far past a draining FIFO, so a timeout means the codec
 * is not clocking the FIFO out (bad clock/route) rather than momentary
 * backpressure — the caller can stop instead of hanging.
 */
#define I2S_TXFREE_SPIN_LIMIT (1u << 16)

void i2s_init(void)
{
    /* --- clock plumbing (05-audio.md, "MCLK / clock-gating enable
     * path"). Pulse the I2S block out of reset, ungate its clock, enable
     * the external device clocks that feed the codec MCLK, then select
     * the 24 MHz EXT reference by clearing bits 3:2 of 0x70000018. */
    mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) | DEV_I2S);
    mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) & ~DEV_I2S);
    mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) | DEV_I2S);
    mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) | DEV_EXTCLOCKS);
    mmio_write32(DEV_EXTCLK_SEL_ADDR,
                 mmio_read32(DEV_EXTCLK_SEL_ADDR) & ~DEV_EXTCLK_24MHZ_MASK);

    /* Route the I2S/CDI pads to their I2S alternate function: clear the
     * pad-group select fields in DEV_INIT2 (CDI+I2S) and DEV_INIT1
     * (05-audio.md, "MCLK / clock-gating enable path"). This is the
     * load-bearing "why is it silent" write on a chainloaded device — if
     * Apple's flash ROM left these pads as GPIO, the codec still ACKs on
     * the separate I2C bus but no audio clocks/data reach it. RMW to
     * preserve the other bits. */
    mmio_write32(DEV_INIT2_ADDR,
                 mmio_read32(DEV_INIT2_ADDR) & ~DEV_INIT2_I2S_PADS);
    mmio_write32(DEV_INIT1_ADDR,
                 mmio_read32(DEV_INIT1_ADDR) & ~DEV_INIT1_I2S_PADS);

    /* --- FIFO reset + format (05-audio.md, "i2s_reset() config"). */
    mmio_write32(IISCONFIG_ADDR, mmio_read32(IISCONFIG_ADDR) | IIS_RESET);
    for (volatile uint32_t i = 0; i < I2S_RESET_HOLD_SPIN; i++) {
        /* hold reset */
    }
    mmio_write32(IISCONFIG_ADDR, mmio_read32(IISCONFIG_ADDR) & ~IIS_RESET);

    /* Standard I2S, 16-bit samples, LE16_2 FIFO format. FORMAT_IIS and
     * SIZE_16BIT are 0, so those masks just clear their fields. */
    mmio_write32(IISCONFIG_ADDR,
                 (mmio_read32(IISCONFIG_ADDR) & ~IIS_FORMAT_MASK)
                     | IIS_FORMAT_IIS);
    mmio_write32(IISCONFIG_ADDR,
                 (mmio_read32(IISCONFIG_ADDR) & ~IIS_SIZE_MASK)
                     | IIS_SIZE_16BIT);
    mmio_write32(IISCONFIG_ADDR,
                 (mmio_read32(IISCONFIG_ADDR) & ~IIS_FIFO_FORMAT_MASK)
                     | IIS_FIFO_FORMAT_LE16_2);

    /* FIFO attention levels, then flush both FIFOs. */
    mmio_write32(IISFIFO_CFG_ADDR,
                 mmio_read32(IISFIFO_CFG_ADDR)
                     | IIS_RX_FULL_LVL_12 | IIS_TX_EMPTY_LVL_4);
    mmio_write32(IISFIFO_CFG_ADDR,
                 mmio_read32(IISFIFO_CFG_ADDR) | IIS_RXCLR | IIS_TXCLR);
}

void i2s_tx_enable(void)
{
    mmio_write32(IISCONFIG_ADDR, mmio_read32(IISCONFIG_ADDR) | IIS_TXFIFOEN);
}

void i2s_disable(void)
{
    /* Stop clocking the TX FIFO, then gate the I2S block clock AND the external
     * device clocks that feed the codec MCLK — both left running after playback
     * today. i2s_init() re-pulses reset and re-ungates both on the next track,
     * so this is safe to do whenever audio is fully stopped (the codec must
     * already be powered down: MCLK is what its power-down sequence rode on). */
    mmio_write32(IISCONFIG_ADDR, mmio_read32(IISCONFIG_ADDR) & ~IIS_TXFIFOEN);
    mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) & ~DEV_EXTCLOCKS);
    mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) & ~DEV_I2S);
}

int i2s_write_stereo(int16_t left, int16_t right)
{
    uint32_t spin = I2S_TXFREE_SPIN_LIMIT;
    while (((mmio_read32(IISFIFO_CFG_ADDR) >> IISFIFO_CFG_TXFREE_SHIFT)
            & IISFIFO_CFG_TXFREE_MASK) == 0
           && --spin != 0) {
        /* wait for a free TX slot */
    }
    if (spin == 0) {
        return -1;
    }

    /* Packed frame: left in the low 16 bits, right in the high 16 bits
     * (05-audio.md; word order verify-by-ear on device). */
    uint32_t word = ((uint32_t)(uint16_t)right << 16) | (uint16_t)left;
    mmio_write32(IISFIFO_WR_ADDR, word);
    return 0;
}
