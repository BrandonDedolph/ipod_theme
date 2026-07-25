/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/audio.c — hw backend for the hal_audio playback contract.
 *
 * Ties together the audio subsystem for interrupt-fed continuous
 * playback: I2C (i2c.c) -> WM8758 codec (wm8758.c) -> I2S serializer
 * (i2s.c), fed by the DMA engine (dma.c) instead of polled writes. A
 * ping-pong pair of PCM buffers is drained by DMA channel 0; each
 * DMA-completion IRQ kicks the other buffer and refills the drained one
 * from the registered source callback (the audio engine's fill function
 * on a real stream; a tone generator during bring-up).
 *
 * Freestanding-clean and asm-free (host-trace-testable). The source
 * callback runs in IRQ context, per the hal_audio contract in hal/hal.h.
 */

#include "hal.h"          /* hal_audio_* contract + audio_source_fn */
#include "pp5022.h"
#include "mmio.h"
#include "i2c.h"
#include "wm8758.h"
#include "i2s.h"
#include "dma.h"
#include "audio.h"
#include "../../kernel/cache.h"   /* cache_commit(): flush before DMA reads */

/*
 * Ping-pong PCM buffers. 8192 frames = ~186 ms at 44.1 kHz, 32 KB each — under
 * the DMA byte-count limit (16-bit field, max 65536 bytes = 16384 frames), and
 * long enough that the ISR can be delayed ~180 ms without the DAC underrunning.
 * That headroom matters: the LCD present runs in an IRQ-masked critical section
 * (an ISR mid-pixel-stream aborts the BCM frame), and its wait-for-idle spin can
 * hold IRQs long enough to starve the audio ISR — worst during FAST scrolling,
 * where back-to-back full-frame presents keep the BCM busy so each idle-wait
 * runs near its limit. 186 ms absorbs a run of those (the UI already caps
 * repaints to ~14 fps while playing). Interleaved int16 [L,R,L,R,...]:
 * read by the DMA as 32-bit words, and on little-endian ARM the pair [L,R] in
 * memory IS (R<<16)|L, which is exactly the I2S FIFO packing — so the buffer
 * feeds the FIFO directly with no repack.
 */
#define AUDIO_FRAMES_PER_BUF 4096u
#define AUDIO_BUF_BYTES      (AUDIO_FRAMES_PER_BUF * 4u)   /* 4 bytes/frame */

static int16_t          audio_buf[2][AUDIO_FRAMES_PER_BUF * 2u];
static audio_source_fn  g_source;
static void            *g_source_ud;
static volatile int      g_active;       /* buffer DMA is currently draining */
static volatile int      g_running;
static volatile uint32_t g_completions;

/*
 * DMA-visible physical address of a buffer. SDRAM is dual-mapped: our .bss
 * lives at the post-MMAP0-remap logical base (0x00000000-based), and the
 * same bytes are reachable at the native SDRAM base (0x10000000 + offset).
 * The DMA engine may not honor the CPU's MMAP0 remap, so we hand it the
 * native alias, which addresses the same bytes either way.
 *
 * *** This is the #1 on-device risk for DMA playback. *** If the tone is
 * silent or garbled with the completion IRQ firing, the alias is wrong for
 * this SoC — try the raw logical address ((uint32_t)(uintptr_t)audio_buf[i]).
 */
static uint32_t buf_phys(int i)
{
    return SDRAM_NATIVE_BASE + (uint32_t)(uintptr_t)audio_buf[i];
}

/*
 * Refill buffer `i` from the source, zero-padding any frames it doesn't
 * produce so the DMA chunk stays a constant size (silence, not a click).
 *
 * CACHE COHERENCY: this is a CPU write that the DMA then reads. The unified
 * cache is write-back, so we cache_commit() (flush dirty lines to SDRAM) after
 * filling — otherwise the DMA, reading the buffer's native SDRAM alias, would
 * fetch stale data. Called from both the priming path and the completion ISR.
 */
static void fill_buffer(int i)
{
    int got = 0;
    if (g_source != 0) {
        got = g_source(g_source_ud, audio_buf[i], (int)AUDIO_FRAMES_PER_BUF);
    }
    if (got < 0) {
        got = 0;
    }
    for (unsigned f = (unsigned)got; f < AUDIO_FRAMES_PER_BUF; f++) {
        audio_buf[i][2u * f]      = 0;
        audio_buf[i][2u * f + 1u] = 0;
    }
    cache_commit();      /* flush so the DMA reads fresh PCM, not stale SDRAM */
}

int hal_audio_init(uint32_t sample_rate, uint16_t channels)
{
    if (sample_rate != 44100u || channels != 2u) {
        return -1;   /* only 44.1 kHz stereo is wired for now */
    }
    i2c_init();
    i2s_init();
    wm8758_init();
    dma_playback_init();

    g_source      = 0;
    g_source_ud   = 0;
    g_active      = 0;
    g_running     = 0;
    g_completions = 0;
    return 0;
}

void hal_audio_set_source(audio_source_fn fn, void *userdata)
{
    g_source    = fn;
    g_source_ud = userdata;
}

void hal_audio_start(void)
{
    if (g_running) {
        return;
    }
    /* Prime both buffers, enable transmit, unmask the DMA completion IRQ,
     * then kick buffer 0. The core I-bit must already be unmasked
     * (arch_irq_enable) by the caller. */
    fill_buffer(0);
    fill_buffer(1);
    g_active      = 0;
    g_completions = 0;
    g_running     = 1;

    i2s_tx_enable();
    mmio_write32(CPU_INT_EN_ADDR, DMA_MASK);   /* enable IRQ 26 */
    dma_playback_kick(buf_phys(0), AUDIO_BUF_BYTES);
}

void audio_dma_isr(void)
{
    dma_playback_ack();            /* read STATUS -> clear the pending IRQ */
    if (!g_running) {
        return;
    }
    int just = g_active;
    int next = just ^ 1;

    /* Keep the FIFO fed with the already-filled other buffer FIRST, then
     * refill the one that just drained. */
    dma_playback_kick(buf_phys(next), AUDIO_BUF_BYTES);
    g_active = next;
    g_completions++;
    fill_buffer(just);
}

void hal_audio_stop(void)
{
    g_running = 0;
    mmio_write32(CPU_INT_DIS_ADDR, DMA_MASK);   /* mask IRQ 26 */
    dma_playback_stop();
}

void hal_audio_close(void)
{
    hal_audio_stop();
    wm8758_powerdown();      /* codec cold (mute+VMID discharge) — MCLK still live */
    i2s_disable();           /* then gate the I2S + codec-MCLK clocks              */
}

uint32_t audio_dma_completions(void)
{
    return g_completions;
}
