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
