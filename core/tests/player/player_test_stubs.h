/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/player/player_test_stubs.h — control surface for the fake world the
 * player test runs core/player/player.c inside. See player_test_stubs.c.
 */
#ifndef CORE_TESTS_PLAYER_STUBS_H
#define CORE_TESTS_PLAYER_STUBS_H

#include <stdint.h>

/* Clear every counter and the broken-cluster list. Call before each case. */
void stub_reset(void);

/* Make the decoder refuse to open the file starting at `clus` — the "this
 * track is corrupt / missing" case the queue logic must skip past. */
void stub_break_cluster(uint32_t clus);

/* Frames the fake decoder yields before reporting end-of-stream. Keep it small
 * so a track finishes within a bounded number of player_pump() calls. */
void stub_set_track_frames(uint32_t frames);

/*
 * Act as the DAC: pull up to `frames` frames through the source callback the
 * player registered. On the device the DMA-completion ISR does this; without
 * it the PCM ring never empties and end-of-track auto-advance never fires, so
 * any test of the queue's advance behaviour has to drive it. Returns the
 * frames actually drained (0 when stopped/paused or the ring is empty).
 */
int stub_drain(int frames);

/* Cluster of the file most recently opened — i.e. which queue entry the player
 * actually chose, independent of what its index says. */
uint32_t stub_last_open_clus(void);

extern int stub_opens;          /* decoder opens that succeeded             */
extern int stub_open_attempts;  /* opens attempted, incl. the failures      */
extern int stub_closes;         /* decoder closes                           */
extern int stub_audio_starts;
extern int stub_audio_stops;
extern int stub_audio_running;  /* 1 while the DAC is running               */
extern int stub_ata_standbys;   /* drive spin-down requests                 */
extern int stub_meta_reads;     /* tag parses                               */

#endif /* CORE_TESTS_PLAYER_STUBS_H */
