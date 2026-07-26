/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_audio/audio_test_stubs.c — the one non-MMIO symbol hal/hw/audio.c
 * needs that cannot be host-compiled.
 *
 * cache_commit() lives in kernel/cache.c and pokes the PP5022 cache-control
 * coprocessor registers directly; there is nothing to model on the host, and
 * on the device it only affects VISIBILITY of bytes the test can already read
 * straight out of the buffer. Counting the calls still lets the test assert the
 * one thing that matters: the driver flushes after every refill, because a
 * missed flush means the DMA streams stale PCM.
 */

#include "cache.h"

static unsigned g_commits;

void cache_init(void)
{
    /* nothing to do host-side */
}

void cache_commit(void)
{
    g_commits++;
}

unsigned audio_test_cache_commits(void);
unsigned audio_test_cache_commits(void)
{
    return g_commits;
}

/*
 * Codec-state restore hook. audio.c registers the real one so wm8758_init()'s
 * reset can't leave the user's volume/tone wiped (bass and treble used to be
 * silently reset on every single track change). Host-side there is no codec, so
 * this just records that the driver asked for a restore.
 */
static unsigned g_restores;

void hal_codec_restore(void);
void hal_codec_restore(void)
{
    g_restores++;
}

unsigned audio_test_codec_restores(void);
unsigned audio_test_codec_restores(void)
{
    return g_restores;
}

/*
 * Audio-DMA activity flag. The real one makes set_cpu_frequency() refuse to
 * reprogram SDRAM timing while the DMA is streaming out of SDRAM; there is no
 * clock to reprogram here.
 */
void clock_set_audio_dma_active(int active);
void clock_set_audio_dma_active(int active)
{
    (void)active;
}
