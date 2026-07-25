/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/i2s.h — PP502x I2S serializer / TX FIFO (polled path).
 *
 * See core/docs/hw/05-audio.md ("SoC I2S block" + "Polled TX-FIFO
 * write"). The WM8758 masters the bus clocks, so this driver only gates
 * the block's clock, configures the FIFO format, and pumps 16-bit stereo
 * frames into the TX FIFO by polling for free space. No DMA (that is the
 * follow-up continuous-playback driver). Asm-free for host trace tests.
 */
#ifndef CORE_HAL_HW_I2S_H
#define CORE_HAL_HW_I2S_H

#include <stdint.h>

/*
 * Bring up the I2S block: ungate its clock + the codec MCLK, select the
 * 24 MHz external reference, then reset + configure the FIFO for standard
 * I2S, 16-bit samples, LE16_2 FIFO format. Call after i2c/codec MCLK is
 * needed but BEFORE wm8758_init sets the codec to master (the codec's
 * MCLK reference must exist first).
 */
void i2s_init(void);

/* Enable the transmitter (start clocking the TX FIFO out to the codec). */
void i2s_tx_enable(void);

/* Stop the TX FIFO and gate the I2S + external (codec MCLK) clocks. Reversed by
 * the next i2s_init(). Call only after the codec is powered down. */
void i2s_disable(void);

/*
 * Write one packed stereo frame, blocking (bounded) until the TX FIFO
 * has a free slot. Returns 0 on success, -1 if no slot freed within the
 * spin budget (FIFO not draining — codec not clocking).
 */
int i2s_write_stereo(int16_t left, int16_t right);

#endif /* CORE_HAL_HW_I2S_H */
