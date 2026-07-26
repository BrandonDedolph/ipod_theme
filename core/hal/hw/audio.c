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
#include "volume.h"       /* hal_codec_restore(): re-apply user state per track */
#include "irqlock.h"      /* hw_irq_save/restore: set_source quiescence          */
#include "audio.h"
#include "../../kernel/cache.h"   /* cache_commit(): flush before DMA reads */
#include "../../kernel/clock.h"   /* lock out frequency switches while streaming */

/*
 * Ping-pong PCM buffers: 8192 frames each = 32 KB = ~186 ms at 44.1 kHz. The
 * DMA byte-count field is 16 bits written as bytes-4 (05-audio.md, "DMA
 * engine"), so 32768 bytes encodes as 0x7FFC and fits with room to spare; the
 * ceiling is 65536 bytes = 16384 frames.
 *
 * *** WHAT THE REAL DEADLINE IS. *** This comment used to claim "the ISR can
 * be delayed ~180 ms without the DAC underrunning". That was wrong, and every
 * downstream decision (notably the UI's 150 ms repaint throttle) was reasoned
 * against the wrong number.
 *
 * The channel is programmed SINGLE | WAIT_REQ and there is no second
 * descriptor armed: the next buffer is only kicked from INSIDE the completion
 * ISR. So at the instant the completion IRQ fires, the only audio still in
 * flight is whatever the I2S TX FIFO holds — 16 frames, i.e. about 363 us at
 * 44.1 kHz. THAT is the deadline for getting into audio_dma_isr and issuing
 * the next kick. Not 186 ms. Anything that masks IRQs for longer than ~360 us
 * risks an audible gap, and the LCD present's IRQ-masked pixel stream is well
 * past that.
 *
 * *** CAN THIS BE FIXED IN HARDWARE? NO. *** The engine documented in
 * 05-audio.md is a single-shot channel: one command word carries the source
 * address and byte count, START launches it, and completion is reported by a
 * status read. There is no descriptor chain, no linked list, no shadow/next
 * register, and no way to queue a second transfer while one is running — so
 * the next buffer genuinely cannot be pre-armed. (Feeding the same I2S request
 * line from a second DMA channel is not a documented arrangement and the
 * arbitration between them is unspecified, so it is not a fix either.) The
 * deadline stays a FIFO depth; buffer size cannot change that.
 *
 * What the buffer size DOES buy is frequency: at 8192 frames a completion IRQ
 * lands every ~186 ms instead of every ~93 ms, halving the number of ~360 us
 * windows that a long IRQ-masked section can collide with. That is the honest
 * argument for the size — it lowers the probability of a miss, it does not
 * widen the deadline.
 *
 * Layout: interleaved int16 [L,R,L,R,...]. The DMA reads 32-bit words, and on
 * little-endian ARM the pair [L,R] in memory IS (R<<16)|L, which is exactly
 * the I2S FIFO packing — so the buffer feeds the FIFO with no repack.
 */
#define AUDIO_FRAMES_PER_BUF 8192u
#define AUDIO_BUF_BYTES      (AUDIO_FRAMES_PER_BUF * 4u)   /* 4 bytes/frame */

/*
 * Length of the fade applied to the tail of a SHORT read. A starved source
 * used to be spliced straight to zero, which is a step discontinuity in the
 * waveform — an audible click, once per underrun. ~64 frames (1.5 ms at
 * 44.1 kHz) is long enough to kill the click and far too short to hear as a
 * fade.
 */
#define AUDIO_TAIL_RAMP_FRAMES 64u

static int16_t          audio_buf[2][AUDIO_FRAMES_PER_BUF * 2u];
static audio_source_fn  g_source;
static void            *g_source_ud;
static volatile int      g_active;       /* buffer DMA is currently draining */
static volatile int      g_running;
static volatile uint32_t g_completions;
static volatile uint32_t g_underruns;    /* short reads seen by fill_buffer   */
static uint16_t          g_channels = 2; /* 1 = expand mono to the stereo link */
static uint32_t          g_rate     = 44100u;

/*
 * Pause/resume bookkeeping (hal.h promises "the internal buffer is not
 * cleared — a subsequent hal_audio_start resumes from where we left off").
 *
 * g_primed says both ping-pong buffers hold PCM already pulled from the ring.
 * hal_audio_start used to ignore that and unconditionally re-prime both from
 * the ring, so every pause/resume silently threw away up to two buffers
 * (~370 ms) of already-decoded, never-heard audio.
 *
 * g_kick_us timestamps the last kick. The DMA is paced by the I2S FIFO, which
 * drains at exactly the sample rate, so elapsed microseconds convert directly
 * into frames already clocked out of the active buffer — accurate to the FIFO
 * depth (~16 frames, 0.4 ms). The engine exposes no residual byte count, so
 * this is the only way to resume mid-buffer instead of replaying it.
 */
static volatile int      g_primed;
static volatile uint32_t g_kick_us;
static volatile uint32_t g_kick_bytes;   /* byte count of the outstanding kick */

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

/* Kick the DMA at `phys` for `bytes`, recording when and how much so a later
 * pause can work out how far it got. */
static void audio_kick(uint32_t phys, uint32_t bytes)
{
    g_kick_us    = mmio_read32(USEC_TIMER_ADDR);
    g_kick_bytes = bytes;
    dma_playback_kick(phys, bytes);
}

/*
 * Refill buffer `i` from the source.
 *
 * A short read means the source is starved. The old behaviour was to zero-fill
 * from the short-read point, which splices the waveform to 0 instantly — a
 * step discontinuity, i.e. a click, on every underrun. Instead we fade the
 * last AUDIO_TAIL_RAMP_FRAMES of real audio down to zero and then pad, so a
 * starved decoder costs a soft drop-out rather than a pop. Underruns are also
 * COUNTED (audio_underruns()) so starvation is observable at all — it used to
 * be completely invisible.
 *
 * Mono sources are expanded to the stereo link here: the source writes
 * `frames` mono samples into the front of the buffer and we duplicate each
 * into an [L,R] pair, walking backwards so the expansion is safe in place.
 *
 * CACHE COHERENCY: this is a CPU write that the DMA then reads. The unified
 * cache is write-back, so we cache_commit() (flush dirty lines to SDRAM) after
 * filling — otherwise the DMA, reading the buffer's native SDRAM alias, would
 * fetch stale data. Called from both the priming path and the completion ISR.
 */
static void fill_buffer(int i)
{
    int16_t *buf = audio_buf[i];
    int got = 0;
    if (g_source != 0) {
        got = g_source(g_source_ud, buf, (int)AUDIO_FRAMES_PER_BUF);
    }
    if (got < 0) {
        got = 0;
    }
    unsigned n = (unsigned)got;
    if (n > AUDIO_FRAMES_PER_BUF) {
        n = AUDIO_FRAMES_PER_BUF;
    }

    /* Mono -> stereo, in place, back to front (dst index 2f >= src index f). */
    if (g_channels == 1u) {
        for (unsigned f = n; f-- != 0; ) {
            int16_t s = buf[f];
            buf[2u * f]      = s;
            buf[2u * f + 1u] = s;
        }
    }

    if (n < AUDIO_FRAMES_PER_BUF) {
        g_underruns++;

        /* Fade the tail of the real audio instead of cutting it. */
        unsigned ramp = n < AUDIO_TAIL_RAMP_FRAMES ? n : AUDIO_TAIL_RAMP_FRAMES;
        for (unsigned k = 0; k < ramp; k++) {
            unsigned f = n - ramp + k;
            /* Linear gain (ramp-1-k)/ramp, integer-only. */
            int32_t g = (int32_t)(ramp - 1u - k);
            buf[2u * f]      = (int16_t)(((int32_t)buf[2u * f]      * g) / (int32_t)ramp);
            buf[2u * f + 1u] = (int16_t)(((int32_t)buf[2u * f + 1u] * g) / (int32_t)ramp);
        }
        for (unsigned f = n; f < AUDIO_FRAMES_PER_BUF; f++) {
            buf[2u * f]      = 0;
            buf[2u * f + 1u] = 0;
        }
    }
    cache_commit();      /* flush so the DMA reads fresh PCM, not stale SDRAM */
}

int hal_audio_init(uint32_t sample_rate, uint16_t channels)
{
    /*
     * Rate: the codec's PLL preset table decides what is reachable (44.1 kHz
     * and its 22.05 kHz half from preset 0; 48 / 32 / 24 kHz from preset 1 —
     * see wm8758.c). Anything else is rejected rather than silently resampled,
     * per the hal.h contract. That is a small set on purpose: a 48 kHz album,
     * a 32 kHz file and a 24 kHz podcast used to fail to open outright.
     */
    if (wm8758_set_rate(sample_rate) != 0) {
        return -1;
    }
    /*
     * Channels: 1 or 2. The I2S link is always stereo — the serializer clocks
     * two 16-bit frames per word and the DMA feeds it 32-bit [R<<16|L] pairs —
     * so mono is handled HERE by duplicating each source sample into both
     * halves (see fill_buffer). The caller does not have to pre-expand.
     */
    if (channels != 1u && channels != 2u) {
        return -1;
    }
    g_channels = channels;

    i2c_init();
    i2s_init();
    /* Hand the codec its state-restore hook BEFORE bring-up: wm8758_init's
     * first act is a full WM_RESET, which wipes the user's volume/balance/
     * bass/treble, and this runs once per TRACK. */
    wm8758_set_restore(hal_codec_restore);
    int codec_bad = wm8758_init();
    dma_playback_init();

    g_rate        = sample_rate;
    g_source      = 0;
    g_source_ud   = 0;
    g_active      = 0;
    g_running     = 0;
    g_primed      = 0;
    g_completions = 0;
    g_underruns   = 0;

    /*
     * Propagate codec bring-up failure. Without this the UI shows a moving
     * progress bar over total silence with nothing anywhere reporting why.
     * The signal is coarse by necessity — the PP502x I2C controller has no
     * per-byte NAK status (09-i2c.md), so the only thing observable from the
     * host side is the BUSY-clear timeout — but a wedged control bus is
     * exactly the case worth surfacing.
     */
    return codec_bad != 0 ? -2 : 0;
}

void hal_audio_set_source(audio_source_fn fn, void *userdata)
{
    /*
     * hal.h's quiescence guarantee: "when this function returns the previously
     * registered fn/userdata are no longer in flight in any callback ... safe
     * to free userdata immediately". Two plain global stores delivered neither
     * half of that — the completion ISR could be inside the OLD callback while
     * this returned, and could observe a torn (new fn, old userdata) pair
     * between the two stores.
     *
     * Masking IRQs at the core fixes both on this single-core, single-priority
     * design: audio_dma_isr only ever runs at IRQ level, so if we are executing
     * here with IRQs masked then no ISR is mid-callback (it would have had to
     * complete before we got the core back), and the pair updates atomically
     * with respect to it.
     */
    uint32_t f = hw_irq_save();
    g_source    = fn;
    g_source_ud = userdata;
    hw_irq_restore(f);
}

void hal_audio_start(void)
{
    if (g_running) {
        return;
    }
    i2s_tx_enable();
    wm8758_mute(false);                        /* undo the mute from stop */
    clock_set_audio_dma_active(1);             /* freeze the CPU/SDRAM clocks */
    mmio_write32(CPU_INT_EN_ADDR, DMA_MASK);   /* enable IRQ 26 */

    if (g_primed) {
        /*
         * RESUME. hal.h: "the internal buffer is not cleared — a subsequent
         * hal_audio_start resumes from where we left off." This used to
         * re-prime BOTH buffers from the ring, discarding up to ~370 ms of
         * already-decoded audio the listener never heard — an audible skip on
         * every unpause.
         *
         * Pick up inside the active buffer at the point the DMA had reached
         * when we stopped. The I2S FIFO paces the DMA at exactly the sample
         * rate, so elapsed microseconds since the kick convert straight into
         * frames consumed (accurate to the ~16-frame FIFO depth). Round DOWN
         * to a whole frame so the L/R phase cannot invert.
         */
        uint32_t elapsed = mmio_read32(USEC_TIMER_ADDR) - g_kick_us;
        uint32_t frames  = (uint32_t)(((uint64_t)elapsed * g_rate) / 1000000u);
        uint32_t done    = frames * 4u;                  /* bytes, frame-aligned */
        if (done > g_kick_bytes) {
            done = g_kick_bytes;
        }
        uint32_t left = g_kick_bytes - done;
        g_running = 1;
        if (left >= 4u) {                 /* SIZE is bytes-4: 4 is the minimum */
            audio_kick(buf_phys(g_active) + done, left);
            return;
        }
        /* The active buffer had effectively finished; go straight to the other
         * one, which fill_buffer already loaded. */
        g_active = g_active ^ 1;
        audio_kick(buf_phys(g_active), AUDIO_BUF_BYTES);
        return;
    }

    /* COLD START: prime both buffers, then kick buffer 0. The core I-bit must
     * already be unmasked (arch_irq_enable) by the caller. */
    fill_buffer(0);
    fill_buffer(1);
    g_active      = 0;
    g_completions = 0;
    g_running     = 1;
    g_primed      = 1;
    audio_kick(buf_phys(0), AUDIO_BUF_BYTES);
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
     * refill the one that just drained. This ordering is what makes the
     * deadline a FIFO depth rather than zero — see the header comment. */
    audio_kick(buf_phys(next), AUDIO_BUF_BYTES);
    g_active = next;
    g_completions++;
    fill_buffer(just);
}

int hal_audio_drain(uint32_t timeout_ms)
{
    /*
     * The last ~186 ms of every track used to be thrown away: the player
     * advances on ring-empty, but at that moment the two ping-pong buffers
     * still hold un-clocked audio that hal_audio_stop -> dma_playback_stop
     * simply discards. Call this first and that tail actually reaches the DAC.
     *
     * Both in-flight buffers have retired once the completion count has
     * advanced by two. After the ring empties fill_buffer pads with silence
     * (ramped, not spliced), so waiting two buffers plays out the real tail
     * and then quiet — never more music.
     *
     * PRECONDITION: IRQs enabled at the core (the count only moves in the
     * completion ISR). Bounded by wall clock, so a stalled DMA cannot hang
     * the caller: returns -1 on timeout, 0 when drained.
     */
    if (!g_running) {
        return 0;
    }
    uint32_t target = g_completions + 2u;
    uint32_t t0     = mmio_read32(USEC_TIMER_ADDR);
    uint32_t limit  = timeout_ms * 1000u;
    while ((int32_t)(g_completions - target) < 0) {
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) > limit) {
            return -1;
        }
    }
    return 0;
}

void hal_audio_stop(void)
{
    /*
     * MUTE FIRST. Cutting the DMA with the DAC live leaves the serializer
     * repeating whatever the FIFO last held and drops the output rail
     * mid-waveform — the click on every stop and every skip. wm8758_mute()
     * has existed since bring-up and was called from nowhere.
     */
    wm8758_mute(true);
    g_running = 0;
    mmio_write32(CPU_INT_DIS_ADDR, DMA_MASK);   /* mask IRQ 26 */
    dma_playback_stop();
    clock_set_audio_dma_active(0);              /* clocks may move again */
    /* g_primed deliberately survives: the buffers still hold unplayed PCM and
     * hal_audio_start() resumes into them (see there). */
}

void hal_audio_close(void)
{
    hal_audio_stop();
    wm8758_powerdown();      /* codec cold (mute+VMID discharge) — MCLK still live */
    i2s_disable();           /* then gate the I2S + codec-MCLK clocks              */
    g_primed = 0;            /* buffers are no longer related to any live stream   */
}

uint32_t audio_dma_completions(void)
{
    return g_completions;
}

uint32_t audio_underruns(void)
{
    return g_underruns;
}
