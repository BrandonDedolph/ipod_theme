/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/audio.h — hw-specific audio glue.
 *
 * The portable playback contract (hal_audio_init / set_source / start /
 * stop / close) lives in hal/hal.h; core/hal/hw/audio.c implements it for
 * the device using i2c + wm8758 + i2s + dma. This header exposes only the
 * two hooks that are hw-specific: the DMA-completion ISR (called from the
 * kernel interrupt dispatcher) and a bring-up completion counter.
 */
#ifndef CORE_HAL_HW_AUDIO_H
#define CORE_HAL_HW_AUDIO_H

#include <stdint.h>

/* DMA-completion interrupt handler (DMA_IRQ / interrupt source 26).
 * Called from kernel/irq.c irq_dispatch when the channel-0 transfer
 * finishes: acks the IRQ, kicks the next buffer, refills the drained one. */
void audio_dma_isr(void);

/* Number of DMA chunks that have completed since the last
 * hal_audio_start() — a bring-up diagnostic (nonzero proves the DMA
 * completion IRQ path is live). */
uint32_t audio_dma_completions(void);

/*
 * Number of buffer refills that came up SHORT since hal_audio_init(), i.e.
 * how many times the source was starved and the HAL had to ramp the tail to
 * silence. Starvation used to be completely invisible — the zero-fill looked
 * exactly like normal operation — so this is the only handle on "is the disk
 * pump keeping up?". Monotonic within a stream; reset by hal_audio_init.
 */
uint32_t audio_underruns(void);

/*
 * Play out the PCM already sitting in the two ping-pong buffers, then return.
 *
 * The player advances on ring-empty, but at that instant up to two buffers
 * (~370 ms) of decoded audio have not been clocked to the DAC yet — and
 * hal_audio_stop() discards them, cutting the last fraction of a second off
 * every track. Call this BEFORE hal_audio_stop() at end-of-track to hear the
 * whole thing.
 *
 * Bounded by wall clock. Returns 0 when the in-flight buffers have retired,
 * -1 if `timeout_ms` elapsed first (a stalled DMA must not hang the caller).
 * Requires IRQs enabled at the core. A no-op returning 0 when not playing.
 */
int hal_audio_drain(uint32_t timeout_ms);

#endif /* CORE_HAL_HW_AUDIO_H */
