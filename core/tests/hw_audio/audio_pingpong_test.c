/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_audio/audio_pingpong_test.c — the ping-pong PCM buffer state
 * machine in hal/hw/audio.c, driven host-side under -DMMIO_MOCK.
 *
 * The existing audio_trace_test asserts the *register grammar* of the driver
 * chain (i2c/wm8758/i2s/dma) but never calls hal_audio_start, audio_dma_isr or
 * hal_audio_stop — so the part of audio.c that actually decides WHICH bytes
 * reach the DAC, and in what order, had no test at all. That state machine is
 * where a glitch lives: a buffer kicked twice repeats ~100 ms of audio, a
 * buffer never kicked drops it, and a short source read that doesn't zero its
 * tail plays back whatever the previous track left behind.
 *
 * WRITTEN AGAINST THE CONTRACT, NOT THE IMPLEMENTATION. Everything here is
 * derived from hal/hal.h's hal_audio_* documentation:
 *   - "Fill buf with up to frames frames ... If you write fewer than frames,
 *      the HAL pads the remainder with silence."
 *   - "hal_audio_stop pauses output. The internal buffer is not cleared — a
 *      subsequent hal_audio_start resumes from where we left off."
 *   - "Pass fn = NULL to clear (silence on next pull)."
 * Buffer SIZE is deliberately never asserted: it is a tuning knob (currently
 * 4096 frames) and the test discovers it from the `frames` the driver asks
 * for. The test should survive that knob moving.
 *
 * The source records the buffer pointer it was handed, so the test can inspect
 * exactly the memory the driver then hands to the DMA — and cross-check that
 * the physical address kicked into the DMA engine really is that buffer's
 * (phys == SDRAM_NATIVE_BASE + (uint32_t)buffer, the dual-mapping audio.c
 * relies on).
 */

#include <stdio.h>
#include <string.h>

#include "pp5022.h"
#include "hal.h"
#include "audio.h"
#include "dma.h"
#include "mmio_mock.h"
#include "../xfail.h"

/* Count of cache_commit() calls, from audio_test_stubs.c. */
unsigned audio_test_cache_commits(void);

/* ---- counting source ------------------------------------------------
 * Every frame it produces carries a globally unique, monotonically increasing
 * value in BOTH channels. That turns "did the driver repeat, drop or reorder
 * audio?" into a statement about integers we can check exactly. */

static uint32_t g_next_val;     /* next value the source will emit           */
static int      g_short_after;  /* after this many calls, produce a short read */
static int      g_short_frames; /* how many frames the short read produces    */
static int      g_calls;
static int      g_return_neg;   /* source reports an error instead of frames  */

#define MAX_CALLS 32
static int16_t *g_call_buf[MAX_CALLS];    /* buffer handed to each call      */
static int      g_call_frames[MAX_CALLS]; /* frames requested                */
static int      g_call_got[MAX_CALLS];    /* frames produced                 */
static uint32_t g_call_first[MAX_CALLS];  /* first value written             */

static int counting_source(void *ud, int16_t *buf, int frames)
{
    (void)ud;
    int want = frames;
    if (g_return_neg) {
        want = -1;
    } else if (g_short_after >= 0 && g_calls == g_short_after) {
        want = g_short_frames;
    }

    if (g_calls < MAX_CALLS) {
        g_call_buf[g_calls]    = buf;
        g_call_frames[g_calls] = frames;
        g_call_got[g_calls]    = want;
        g_call_first[g_calls]  = g_next_val;
    }
    g_calls++;

    if (want < 0) {
        return want;
    }
    for (int f = 0; f < want; f++) {
        /* Values start at 1 so "0" unambiguously means "silence padding". */
        int16_t v = (int16_t)((g_next_val++ & 0x7FFFu) + 1u);
        buf[2 * f]     = v;
        buf[2 * f + 1] = v;
    }
    return want;
}

static void source_reset(void)
{
    g_next_val    = 0;
    g_calls       = 0;
    g_short_after = -1;
    g_short_frames = 0;
    g_return_neg  = 0;
    memset(g_call_buf, 0, sizeof g_call_buf);
    memset(g_call_frames, 0, sizeof g_call_frames);
    memset(g_call_got, 0, sizeof g_call_got);
    memset(g_call_first, 0, sizeof g_call_first);
}

/* ---- DMA kick observation -------------------------------------------
 * dma_playback_kick writes the source address and the byte count into the
 * channel-0 registers; the mock bus records them. Reading the kicks back out
 * of the log is how the test sees which buffer the DAC was pointed at. */

#define MAX_KICKS 64
static uint32_t g_kick_addr[MAX_KICKS];
static uint32_t g_kick_bytes[MAX_KICKS];
static int      g_kicks;

/* Rescan the whole mock log and rebuild the kick list. A kick is identified by
 * the write to the channel-0 source-address register; the byte count is the
 * next write to the size register. */
static void collect_kicks(void)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    g_kicks = 0;
    for (size_t i = 0; i < len && g_kicks < MAX_KICKS; i++) {
        if (log[i].op != MMIO_OP_WRITE || log[i].addr != DMA0_RAM_ADDR_ADDR) {
            continue;
        }
        g_kick_addr[g_kicks] = log[i].value;
        g_kick_bytes[g_kicks] = 0;
        for (size_t j = i + 1; j < len; j++) {
            if (log[j].op == MMIO_OP_WRITE && log[j].addr == DMA0_CMD_ADDR) {
                g_kick_bytes[g_kicks] = log[j].value;
                break;
            }
        }
        g_kicks++;
    }
}

/* Physical address audio.c hands the DMA for a given host buffer. */
static uint32_t phys_of(const int16_t *p)
{
    return SDRAM_NATIVE_BASE + (uint32_t)(uintptr_t)p;
}

/* Program every status register the chain polls so no bounded spin runs long.
 * Values are "ready"/"idle" for each poll; the grammar itself is asserted by
 * audio_trace_test, not here. */
static void bus_ready(void)
{
    mmio_mock_reset();
    /* Every register the playback path polls (I2C BUSY, the DMA channel's
     * BUSY/INTR) reads 0 by default on the mock bus, which is exactly the
     * "idle / ready" state — so no bounded spin here runs to its limit. */
}

/* The int16 sample the counting source writes for the n-th frame it ever
 * produced. Values start at 1 so 0 unambiguously means "silence padding". */
static int16_t sample_for(uint32_t n)
{
    return (int16_t)((n & 0x7FFFu) + 1u);
}

/* Host buffer behind a DMA physical address, or NULL if the driver kicked
 * something the source was never handed. */
static const int16_t *buf_for_phys(uint32_t phys)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        if (g_call_buf[i] != 0 && phys_of(g_call_buf[i]) == phys) {
            return g_call_buf[i];
        }
    }
    return 0;
}

/*
 * First PCM sample in the buffer the driver LAST pointed the DMA at, i.e. the
 * next audio the DAC will actually play. Valid immediately after a start() or
 * an ISR: audio.c kicks the already-filled buffer and only then refills the
 * OTHER one, so the kicked buffer's contents are still the ones being handed
 * out. Returns 0x7FFF_FFFF if nothing was kicked.
 */
#define NO_KICK 0x7FFFFFFF
static int32_t last_kick_first_sample(void)
{
    collect_kicks();
    if (g_kicks == 0) {
        return NO_KICK;
    }
    const int16_t *b = buf_for_phys(g_kick_addr[g_kicks - 1]);
    return b ? (int32_t)b[0] : NO_KICK;
}

/* Bring the driver to a known state: stopped, source re-armed, counters and
 * the mock log cleared, then started. hal_audio_start() is a no-op while the
 * driver is already running, so the stop is what makes this deterministic. */
/*
 * A genuinely COLD start.
 *
 * hal_audio_stop() no longer discards the primed buffers — that is the whole
 * point of the pause/resume fix (hal.h promises "the internal buffer is not
 * cleared ... a subsequent hal_audio_start resumes from where we left off",
 * and start() used to re-prime both buffers unconditionally, dropping up to a
 * full buffer of music on every un-pause). Only hal_audio_close() severs the
 * buffers from the stream. So a test that wants to observe priming has to
 * close and re-init, not stop and re-start.
 */
static void fresh_start(void)
{
    hal_audio_close();
    bus_ready();
    hal_audio_init(44100u, 2u);
    source_reset();
    bus_ready();
    hal_audio_set_source(counting_source, 0);
    hal_audio_start();
}

int main(void)
{
    xfail_ctx c = { "audio-pingpong", 0, 0, 0 };

    /* --- 1. init accepts the formats it advertises, and only those ------
     * 44.1 kHz stereo used to be the ONLY accepted format, which meant a
     * 48 kHz album or a mono podcast failed to open and the player skipped it
     * with no explanation. The codec now reconfigures its PLL per rate, so the
     * supported set is a real set — but it is still a set, and a rate the DAC
     * cannot clock must be refused rather than played at the wrong speed. */
    bus_ready();
    xpect(&c, "init accepts 44.1 kHz stereo", hal_audio_init(44100u, 2u) == 0);
    bus_ready();
    xpect(&c, "init accepts 48 kHz stereo",   hal_audio_init(48000u, 2u) == 0);
    bus_ready();
    xpect(&c, "init accepts mono",            hal_audio_init(44100u, 1u) == 0);
    bus_ready();
    xpect(&c, "init rejects a rate the PLL has no preset for",
          hal_audio_init(96000u, 2u) != 0);
    bus_ready();
    xpect(&c, "init rejects a channel count that is neither mono nor stereo",
          hal_audio_init(44100u, 6u) != 0);
    bus_ready();
    xpect(&c, "init re-accepts 44.1 kHz stereo", hal_audio_init(44100u, 2u) == 0);

    /* --- 2. start primes from the source and kicks a real buffer ------- */
    hal_audio_set_source(counting_source, 0);
    fresh_start();
    collect_kicks();

    xpect(&c, "start pulls from the source", g_calls > 0);
    xpect(&c, "start kicks the DMA exactly once", g_kicks == 1);
    if (g_calls == 0 || g_kicks == 0) {
        fprintf(stderr, "audio-pingpong: driver never started; "
                        "the rest of this suite cannot run\n");
        return 1;
    }

    const int frames = g_call_frames[0];
    xpect(&c, "source is asked for a positive frame count", frames > 0);
    xpect(&c, "start primes both ping-pong buffers before kicking",
          g_calls == 2 && g_call_buf[0] != g_call_buf[1]);
    xpect(&c, "every pull asks for the same frame count",
          g_call_frames[1] == frames);
    /* The command word carries the length as (bytes - DMA_SIZE_BIAS) in
     * DMA_CMD_SIZE_MASK, with START set. 4 bytes per interleaved stereo s16
     * frame is the packing audio.c documents. */
    xpect(&c, "kick length == frames * 4 bytes",
          (g_kick_bytes[0] & DMA_CMD_SIZE_MASK) + DMA_SIZE_BIAS ==
              (uint32_t)frames * 4u);
    xpect(&c, "kick sets the START bit",
          (g_kick_bytes[0] & DMA_CMD_START) != 0);
    xpect(&c, "kick address is a primed buffer, in the SDRAM alias",
          g_kick_addr[0] == phys_of(g_call_buf[0]));
    xpect(&c, "the buffer kicked first is the one filled first",
          last_kick_first_sample() == sample_for(0));

    /* --- 3. buffers ping-pong: exactly two, strictly alternating ------- */
    fresh_start();
    for (int i = 0; i < 6; i++) {
        audio_dma_isr();
    }
    collect_kicks();

    xpect(&c, "one kick per completion (1 prime + 6 ISRs)", g_kicks == 7);
    int distinct_ok = 1, alternating_ok = 1;
    for (int i = 0; i < g_kicks; i++) {
        if (g_kick_addr[i] != g_kick_addr[0] && g_kick_addr[i] != g_kick_addr[1]) {
            distinct_ok = 0;
        }
        if (i >= 1 && g_kick_addr[i] == g_kick_addr[i - 1]) {
            alternating_ok = 0;
        }
    }
    xpect(&c, "only two distinct PCM buffers are ever kicked", distinct_ok);
    xpect(&c, "consecutive kicks never reuse the same buffer", alternating_ok);
    xpect(&c, "buffers alternate A,B,A,B,...",
          g_kicks >= 4 && g_kick_addr[0] != g_kick_addr[1] &&
          g_kick_addr[2] == g_kick_addr[0] && g_kick_addr[3] == g_kick_addr[1]);
    xpect(&c, "the drained buffer is refilled after the other is kicked",
          g_calls == 8);   /* 2 priming pulls + one per completion */

    /* Continuity: step through completions and check, at each one, that the
     * audio the DMA was just pointed at continues exactly where the previous
     * chunk ended. A repeated chunk (same first sample twice) or a skipped one
     * (a jump of more than `frames`) is an audible glitch. */
    fresh_start();
    int continuity_ok = (last_kick_first_sample() == sample_for(0));
    for (uint32_t n = 1; n <= 5; n++) {
        audio_dma_isr();
        if (last_kick_first_sample() != sample_for(n * (uint32_t)frames)) {
            continuity_ok = 0;
        }
    }
    xpect(&c, "no audio repeated or dropped across completions", continuity_ok);

    /* --- 4. a short source read zero-pads EXACTLY the tail ------------- */
    hal_audio_close();                /* cold, so start() really primes       */
    bus_ready();
    hal_audio_init(44100u, 2u);
    source_reset();
    bus_ready();
    hal_audio_set_source(counting_source, 0);
    g_short_after  = 0;               /* short-change the first priming pull */
    g_short_frames = 17;              /* deliberately odd and tiny            */
    hal_audio_start();
    xpect(&c, "short pull was taken", g_calls >= 1 && g_call_got[0] == 17);
    {
        const int16_t *b = g_call_buf[0];
        int head_ok = 1, tail_ok = 1;
        /*
         * The source's frames are NOT left bit-identical any more: an underrun
         * used to zero-splice mid-waveform, which is an instantaneous jump to
         * silence — a click, not a graceful dropout. The driver now fades the
         * last AUDIO_TAIL_RAMP_FRAMES of real audio to zero before padding.
         * With only 17 frames delivered the ramp covers all of them, so what we
         * can assert is that each frame is a monotonically shrinking, correctly
         * signed attenuation of what the source wrote — and that the last one
         * has reached silence, so the pad it runs into is continuous.
         */
        for (int f = 0; f < 17; f++) {
            int32_t want = sample_for((uint32_t)f);
            int32_t got  = b[2 * f];
            if (b[2 * f + 1] != got) {
                head_ok = 0;                     /* both channels ramp alike */
            }
            if (want >= 0 ? (got < 0 || got > want) : (got > 0 || got < want)) {
                head_ok = 0;                     /* attenuated, never amplified
                                                  * and never sign-flipped     */
            }
        }
        if (b[2 * 16] != 0 || b[2 * 16 + 1] != 0) {
            head_ok = 0;                         /* ramp reaches true silence */
        }
        for (int f = 17; f < frames; f++) {
            if (b[2 * f] != 0 || b[2 * f + 1] != 0) {
                tail_ok = 0;
            }
        }
        xpect(&c, "short read: the source's frames are ramped down, not spliced",
              head_ok);
        xpect(&c, "short read: every frame past the tail is silence", tail_ok);
        xpect(&c, "short read: the chunk length handed to the DMA is unchanged",
              (g_kick_bytes[0] & DMA_CMD_SIZE_MASK) + DMA_SIZE_BIAS ==
                  (uint32_t)frames * 4u);
    }

    /* A source that reports an error must yield a fully silent buffer, not
     * whatever the previous track left in it. */
    hal_audio_close();                /* cold, so start() really primes       */
    bus_ready();
    hal_audio_init(44100u, 2u);
    source_reset();
    bus_ready();
    hal_audio_set_source(counting_source, 0);
    g_return_neg = 1;
    hal_audio_start();
    {
        const int16_t *b = g_call_buf[0];
        int all_zero = (g_calls >= 1);   /* a stop/start pair primes nothing:
                                          * without a pull there is no buffer
                                          * to inspect, and reading g_call_buf[0]
                                          * would be a wild pointer. */
        for (int i = 0; i < frames * 2; i++) {
            if (b[i] != 0) {
                all_zero = 0;
            }
        }
        xpect(&c, "failed pull yields silence, not stale PCM", all_zero);
    }

    /* --- 5. stop() then start() must not skip audio -------------------- *
     * hal.h: "hal_audio_stop pauses output. The internal buffer is not
     * cleared — a subsequent hal_audio_start resumes from where we left off."
     * The player's pause/resume is exactly this pair, so anything pulled from
     * the ring and never played is audio dropped on every un-pause. */
    fresh_start();
    audio_dma_isr();
    audio_dma_isr();
    /*
     * Three chunks have been handed out, so the DMA is part-way through the one
     * starting at 2*frames — NOT finished with it. "Resumes where we left off"
     * therefore means picking up INSIDE that chunk, not moving on to the next
     * one: jumping to 3*frames is precisely the bug, because the unplayed
     * remainder of chunk 2 was pulled from the ring and never heard.
     *
     * The mock's USEC_TIMER is a constant, so no time passes across the
     * stop/start and the resume point is the chunk's own start.
     */
    int32_t before = last_kick_first_sample();
    xpect(&c, "pre-stop position is where we think it is",
          before == sample_for(2u * (uint32_t)frames));

    hal_audio_stop();
    bus_ready();
    hal_audio_start();
    collect_kicks();
    xpect(&c, "stop() then start() resumes kicking the DMA", g_kicks >= 1);
    xpect(&c, "stop/start resumes where it left off (drops no audio)",
          last_kick_first_sample() == sample_for(2u * (uint32_t)frames));
    xpect(&c, "stop/start does not skip to the next chunk",
          last_kick_first_sample() != sample_for(3u * (uint32_t)frames));

    /* --- 6. a cleared source is silence, not a crash ------------------- */
    hal_audio_stop();
    hal_audio_set_source(0, 0);
    source_reset();
    bus_ready();
    hal_audio_start();
    audio_dma_isr();
    collect_kicks();
    xpect(&c, "NULL source still clocks buffers (silence, not a stall)",
          g_kicks == 2);
    xpect(&c, "NULL source is never called", g_calls == 0);

    /* --- 7. after stop, a stray completion must not kick the DMA ------- */
    hal_audio_set_source(counting_source, 0);
    fresh_start();
    hal_audio_stop();
    bus_ready();
    audio_dma_isr();
    collect_kicks();
    xpect(&c, "an ISR arriving after stop() does not restart playback",
          g_kicks == 0);
    xpect(&c, "an ISR arriving after stop() does not pull the source",
          g_calls == 2);   /* only the two priming pulls from fresh_start */

    /* --- 8. every refill is followed by a cache flush ------------------ *
     * The DMA reads the buffer's native SDRAM alias while the CPU writes it
     * through a write-back cache; a missed flush streams stale PCM. */
    unsigned commits_before = audio_test_cache_commits();
    fresh_start();
    audio_dma_isr();
    xpect(&c, "each buffer fill is followed by a cache flush",
          audio_test_cache_commits() - commits_before == (unsigned)g_calls);

    return xfail_done(&c);
}
