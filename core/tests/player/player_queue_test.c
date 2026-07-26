/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/player/player_queue_test.c — the queue / auto-advance / repeat /
 * shuffle logic of core/player/player.c, host-side.
 *
 * player.c is 753 lines and had zero tests. The decode path needs real audio
 * and real hardware, but the part that decides WHICH TRACK PLAYS NEXT is pure
 * bookkeeping over an array, and it is the part users notice when it is wrong:
 * a queue that stops one track early, a Repeat All that doesn't wrap, a
 * Shuffle that replays the song you just heard, a Prev that restarts instead
 * of going back, a broken file that stalls playback instead of being skipped.
 *
 * The real player.c is compiled unmodified; its disk, codec and DAC are
 * replaced by the controllable fakes in player_test_stubs.c. Which entry the
 * player picked is read back from the CLUSTER the fake decoder was opened on,
 * not from an index accessor — so the assertions are about the file that would
 * actually play.
 *
 * Note on what is NOT covered here: player_advance(), queue_playable_count()
 * and queue_random_playable() are `static` in player.c and therefore not
 * directly reachable. They are exercised INDIRECTLY, through player_pump()
 * running a track to end-of-stream (which is how auto-advance happens in the
 * firmware) and through player_next/player_prev. The decode/ring path,
 * load_folder_art() and player_probe_meta() need real file bytes and are out
 * of scope for this test.
 */

#include <stdio.h>
#include <string.h>

#include "player.h"
#include "pp5022.h"
#include "mmio_mock.h"
#include "player_test_stubs.h"
#include "../xfail.h"

/* Clusters are the identity of a queue entry throughout this test. */
#define CLUS(i) (100u + (uint32_t)(i))

static fat32_t g_fs;

/* Build an n-entry queue of playable files; `dir_mask` bit i marks entry i as
 * a subdirectory (which the player must skip when choosing what to play). */
static void make_entries(browse_entry_t *e, int n, unsigned dir_mask)
{
    memset(e, 0, sizeof(browse_entry_t) * (size_t)n);
    for (int i = 0; i < n; i++) {
        snprintf(e[i].name, sizeof e[i].name, "TRACK%02d", i);
        e[i].clus   = CLUS(i);
        e[i].size   = 1024u * 1024u;
        e[i].fmt    = 0;                       /* FLAC */
        e[i].is_dir = (dir_mask >> i) & 1u;
    }
}

/* Advance the virtual microsecond clock the player reads for elapsed time. */
static void set_usec(uint32_t us)
{
    mmio_mock_set_read(USEC_TIMER_ADDR, us);
}

/* Run the player until the current track ends and auto-advance has happened,
 * or until we give up. Returns the number of pumps it took. player_pump() is
 * the firmware's main-loop call, so this is exactly how a track ends on the
 * device. */
static int pump_to_track_end(int max_pumps)
{
    int start_opens = stub_opens;
    int start_stops = stub_audio_stops;
    for (int i = 0; i < max_pumps; i++) {
        player_pump();
        stub_drain(4096);          /* stand in for the DMA-completion ISR */
        if (stub_opens != start_opens ||
            (!player_active() && stub_audio_stops != start_stops)) {
            return i + 1;
        }
    }
    return max_pumps;
}

int main(void)
{
    xfail_ctx c = { "player-queue", 0, 0, 0 };
    browse_entry_t ents[8];

    mmio_mock_reset();
    set_usec(0);
    player_init(&g_fs);

    /* ---- 1. play_queue starts at the requested entry ------------------ */
    stub_reset();
    make_entries(ents, 4, 0);
    player_play_queue(ents, 4, 2, 0, 0);
    xpect(&c, "play_queue opens the entry it was asked to start at",
          stub_last_open_clus() == CLUS(2));
    xpect(&c, "play_queue reports the queue length", player_queue_len() == 4);
    xpect(&c, "play_queue reports the current index", player_queue_current() == 2);
    xpect(&c, "play_queue leaves the player active", player_active() == 1);
    xpect(&c, "play_queue starts the DAC", stub_audio_running == 1);
    xpect(&c, "play_queue reads the track's tags once", stub_meta_reads == 1);
    xpect(&c, "the queue exposes the entry names",
          strcmp(player_queue_name(2), "TRACK02") == 0);
    xpect(&c, "out-of-range queue names are empty, not out-of-bounds reads",
          player_queue_name(-1)[0] == '\0' &&
          player_queue_name(99)[0] == '\0');

    /* ---- 2. auto-advance walks the queue in order --------------------- */
    stub_reset();
    stub_set_track_frames(4096);
    make_entries(ents, 4, 0);
    player_play_queue(ents, 4, 0, 0, 0);
    xpect(&c, "advance: starts on entry 0", stub_last_open_clus() == CLUS(0));
    pump_to_track_end(2000);
    xpect(&c, "advance: end of track 0 moves to track 1",
          stub_last_open_clus() == CLUS(1) && player_queue_current() == 1);
    pump_to_track_end(2000);
    xpect(&c, "advance: end of track 1 moves to track 2",
          stub_last_open_clus() == CLUS(2) && player_queue_current() == 2);

    /* ---- 3. Repeat OFF stops at the end of the queue ------------------ */
    stub_reset();
    player_set_repeat(0);
    player_set_shuffle(0);
    make_entries(ents, 2, 0);
    player_play_queue(ents, 2, 1, 0, 0);      /* start on the LAST entry */
    pump_to_track_end(2000);
    xpect(&c, "repeat off: the queue ends idle, it does not wrap",
          player_active() == 0);
    xpect(&c, "repeat off: the DAC is stopped at the end of the queue",
          stub_audio_running == 0);

    /* ---- 4. Repeat ALL wraps to the first playable entry -------------- */
    stub_reset();
    player_set_repeat(1);
    make_entries(ents, 3, 0);
    player_play_queue(ents, 3, 2, 0, 0);      /* last entry */
    pump_to_track_end(2000);
    xpect(&c, "repeat all: wraps from the last entry to the first",
          player_active() == 1 && stub_last_open_clus() == CLUS(0) &&
          player_queue_current() == 0);

    /* ---- 5. Repeat ONE replays the same track ------------------------- */
    stub_reset();
    player_set_repeat(2);
    make_entries(ents, 3, 0);
    player_play_queue(ents, 3, 1, 0, 0);
    int opens_before = stub_opens;
    pump_to_track_end(2000);
    xpect(&c, "repeat one: replays the SAME entry",
          player_queue_current() == 1 && stub_last_open_clus() == CLUS(1));
    xpect(&c, "repeat one: really re-opened the file (not a no-op)",
          stub_opens == opens_before + 1);

    /* Repeat One must not spin forever on a track that stops opening: the
     * player's own comment says the replay path returns early precisely so a
     * lone broken track cannot loop. Break the track, then end it. */
    stub_break_cluster(CLUS(1));
    int pumps = pump_to_track_end(2000);
    xpect(&c, "repeat one: a track that stops opening does not spin forever",
          pumps < 2000);
    player_set_repeat(0);

    /* ---- 6. folders in the queue are never played --------------------- */
    stub_reset();
    player_set_repeat(0);
    make_entries(ents, 4, 0x2u);              /* entry 1 is a directory */
    player_play_queue(ents, 4, 0, 0, 0);
    pump_to_track_end(2000);
    xpect(&c, "a subdirectory entry is skipped, not opened",
          stub_last_open_clus() == CLUS(2) && player_queue_current() == 2);
    xpect(&c, "the queue still reports the folder as a folder",
          player_queue_is_dir(1) == 1 && player_queue_is_dir(0) == 0);

    /* ---- 7. a broken file is skipped, and a fully broken queue idles --- */
    stub_reset();
    make_entries(ents, 4, 0);
    stub_break_cluster(CLUS(0));
    stub_break_cluster(CLUS(1));
    player_play_queue(ents, 4, 0, 0, 0);
    xpect(&c, "a broken first track is skipped to the first that opens",
          player_active() == 1 && stub_last_open_clus() == CLUS(2));
    xpect(&c, "skipping tried each broken track exactly once",
          stub_open_attempts == 3);

    stub_reset();
    make_entries(ents, 3, 0);
    stub_break_cluster(CLUS(0));
    stub_break_cluster(CLUS(1));
    stub_break_cluster(CLUS(2));
    player_play_queue(ents, 3, 0, 0, 0);
    xpect(&c, "a queue where nothing opens ends idle rather than looping",
          player_active() == 0);
    xpect(&c, "a queue where nothing opens is bounded by the queue length",
          stub_open_attempts <= 3 + 1);

    /* ---- 8. player_jump ----------------------------------------------- */
    stub_reset();
    make_entries(ents, 4, 0x4u);              /* entry 2 is a directory */
    player_play_queue(ents, 4, 0, 0, 0);
    player_jump(3);
    xpect(&c, "jump plays the requested entry",
          player_queue_current() == 3 && stub_last_open_clus() == CLUS(3));
    player_jump(2);
    xpect(&c, "jump to a folder is a no-op", player_queue_current() == 3);
    player_jump(-1);
    player_jump(99);
    xpect(&c, "jump out of range is a no-op", player_queue_current() == 3);

    /* ---- 9. next / prev ------------------------------------------------ */
    stub_reset();
    player_set_repeat(0);
    player_set_shuffle(0);
    make_entries(ents, 4, 0);
    player_play_queue(ents, 4, 1, 0, 0);
    player_next();
    xpect(&c, "next moves forward one entry",
          player_queue_current() == 2 && stub_last_open_clus() == CLUS(2));

    /* Prev within the first ~3 s of a track goes BACK; past it, restarts. */
    set_usec(0);
    player_play_queue(ents, 4, 2, 0, 0);
    set_usec(1000000u);                        /* 1 s in */
    player_prev();
    xpect(&c, "prev early in a track goes to the previous entry",
          player_queue_current() == 1 && stub_last_open_clus() == CLUS(1));

    set_usec(0);
    player_play_queue(ents, 4, 2, 0, 0);
    set_usec(9000000u);                        /* 9 s in */
    player_prev();
    xpect(&c, "prev late in a track restarts the current one",
          player_queue_current() == 2 && stub_last_open_clus() == CLUS(2));

    /* Prev wraps at the start of the queue even with Repeat off — the
     * familiar iPod behaviour player.h documents. */
    set_usec(0);
    player_play_queue(ents, 4, 0, 0, 0);
    set_usec(500000u);
    player_prev();
    xpect(&c, "prev at the head of the queue wraps to the tail",
          player_queue_current() == 3);

    /* next at the tail with Repeat OFF must stop, not wrap. */
    stub_reset();
    player_set_repeat(0);
    make_entries(ents, 3, 0);
    player_play_queue(ents, 3, 2, 0, 0);
    player_next();
    xpect(&c, "next past the last entry with repeat off stops playback",
          player_active() == 0);

    /* ...and with Repeat ALL it wraps. */
    stub_reset();
    player_set_repeat(1);
    player_play_queue(ents, 3, 2, 0, 0);
    player_next();
    xpect(&c, "next past the last entry with repeat all wraps to the first",
          player_active() == 1 && player_queue_current() == 0);
    player_set_repeat(0);

    /* ---- 10. shuffle --------------------------------------------------- */
    stub_reset();
    player_set_shuffle(1);
    make_entries(ents, 6, 0);
    player_play_queue(ents, 6, 0, 0, 0);
    {
        /* Every shuffle pick must be a real, playable entry, and over many
         * picks it must actually move around rather than sticking. The RNG is
         * stirred with USEC_TIMER, so advance the clock between picks. */
        int seen[6] = { 0 };
        int immediate_repeats = 0;
        int prev = player_queue_current();
        int in_range = 1;
        for (int i = 0; i < 60; i++) {
            set_usec((uint32_t)(i * 7919));
            player_next();
            int cur = player_queue_current();
            if (cur < 0 || cur >= 6) {
                in_range = 0;
                break;
            }
            seen[cur] = 1;
            if (cur == prev) {
                immediate_repeats++;
            }
            prev = cur;
        }
        xpect(&c, "shuffle only ever selects entries inside the queue", in_range);
        int distinct = 0;
        for (int i = 0; i < 6; i++) {
            distinct += seen[i];
        }
        xpect(&c, "shuffle reaches most of the queue over 60 picks",
              distinct >= 4);
        xpect(&c, "shuffle avoids replaying the track it just played",
              immediate_repeats == 0);
    }

    /* Shuffle must not pick a folder. */
    stub_reset();
    player_set_shuffle(1);
    make_entries(ents, 5, 0x0Au);             /* entries 1 and 3 are folders */
    player_play_queue(ents, 5, 0, 0, 0);
    {
        int picked_folder = 0;
        for (int i = 0; i < 40; i++) {
            set_usec((uint32_t)(i * 104729));
            player_next();
            if (player_queue_is_dir(player_queue_current())) {
                picked_folder = 1;
            }
        }
        xpect(&c, "shuffle never selects a subdirectory", !picked_folder);
    }

    /* A shuffled queue with exactly one playable entry must terminate. */
    stub_reset();
    player_set_shuffle(1);
    make_entries(ents, 3, 0x6u);              /* only entry 0 is playable */
    player_play_queue(ents, 3, 0, 0, 0);
    player_next();
    xpect(&c, "shuffle with a single playable entry replays it, bounded",
          player_queue_current() == 0 && player_active() == 1);
    player_set_shuffle(0);

    /* ---- 11. pause / resume -------------------------------------------- */
    stub_reset();
    make_entries(ents, 3, 0);
    set_usec(0);
    player_play_queue(ents, 3, 0, 0, 0);
    xpect(&c, "a fresh track is not paused", player_paused() == 0);

    set_usec(5000000u);                       /* 5 s in */
    xpect(&c, "elapsed time tracks the microsecond timer",
          player_elapsed_s() == 5u);
    player_pause();
    xpect(&c, "pause stops the DAC", stub_audio_running == 0);
    xpect(&c, "pause keeps the track loaded", player_active() == 1);
    xpect(&c, "pause reports itself", player_paused() == 1);

    set_usec(12000000u);                      /* 7 s of paused wall time  */
    xpect(&c, "the elapsed clock freezes while paused",
          player_elapsed_s() == 5u);
    int opens_at_pause = stub_opens;
    player_pump();
    xpect(&c, "pump does nothing while paused", stub_opens == opens_at_pause);

    player_resume();
    xpect(&c, "resume restarts the DAC", stub_audio_running == 1);
    xpect(&c, "resume does not re-open the track", stub_opens == opens_at_pause);
    xpect(&c, "resume clears the paused flag", player_paused() == 0);
    xpect(&c, "resume does not count the paused time as elapsed",
          player_elapsed_s() == 5u);
    set_usec(14000000u);
    xpect(&c, "the clock runs again after resume", player_elapsed_s() == 7u);

    player_toggle_pause();
    xpect(&c, "toggle pauses a playing track", player_paused() == 1);
    player_toggle_pause();
    xpect(&c, "toggle resumes a paused track", player_paused() == 0);

    /* ---- 12. stop ------------------------------------------------------ */
    player_stop();
    xpect(&c, "stop leaves the player inactive", player_active() == 0);
    xpect(&c, "stop stops the DAC", stub_audio_running == 0);
    xpect(&c, "stop clears the paused flag", player_paused() == 0);
    /* Device-proven invariant from player.h: stop must NOT close the decoder
     * mid-decode — doing so hard-freezes the core. */
    xpect(&c, "stop does not close the decoder (device-proven invariant)",
          stub_closes == 0);
    player_stop();
    xpect(&c, "stop is idempotent", player_active() == 0);

    /* ---- 13. the incremental queue builder ----------------------------- */
    stub_reset();
    player_queue_begin();
    xpect(&c, "queue_begin empties the queue", player_queue_len() == 0);
    make_entries(ents, 5, 0);
    for (int i = 0; i < 5; i++) {
        player_queue_add(&ents[i]);
    }
    xpect(&c, "queue_add accumulates entries", player_queue_len() == 5);
    player_queue_commit(3);
    xpect(&c, "queue_commit starts at the requested index",
          player_queue_current() == 3 && stub_last_open_clus() == CLUS(3));

    player_queue_begin();
    for (int i = 0; i < 5; i++) {
        player_queue_add(&ents[i]);
    }
    player_queue_commit(-1);
    xpect(&c, "queue_commit clamps an out-of-range start to 0",
          player_queue_current() == 0);
    player_queue_begin();
    for (int i = 0; i < 5; i++) {
        player_queue_add(&ents[i]);
    }
    player_queue_commit(999);
    xpect(&c, "queue_commit clamps a too-large start to 0",
          player_queue_current() == 0);

    stub_reset();
    player_queue_begin();
    player_queue_commit(0);
    xpect(&c, "committing an empty queue plays nothing",
          stub_open_attempts == 0 && player_active() == 0);

    /* The builder must not run off the end of its fixed array. */
    stub_reset();
    player_queue_begin();
    {
        browse_entry_t one;
        make_entries(&one, 1, 0);
        for (int i = 0; i < QUEUE_MAX + 50; i++) {
            player_queue_add(&one);
        }
    }
    xpect(&c, "queue_add stops at QUEUE_MAX instead of overflowing",
          player_queue_len() == QUEUE_MAX);

    /* play_queue must clamp too: it copies a caller array of any length. */
    stub_reset();
    make_entries(ents, 4, 0);
    player_play_queue(ents, 4, 0, 0, 0);
    xpect(&c, "play_queue takes the whole (short) queue", player_queue_len() == 4);

    /* ---- 14. calls with nothing loaded are safe ------------------------ */
    stub_reset();
    player_queue_begin();          /* stops playback, empties the queue */
    player_next();
    player_prev();
    player_pump();
    player_pause();
    player_resume();
    player_toggle_pause();
    xpect(&c, "next/prev/pump/pause on an empty queue do nothing",
          player_active() == 0 && stub_open_attempts == 0 &&
          player_queue_len() == 0);
    xpect(&c, "elapsed/total read as zero when nothing is loaded",
          player_elapsed_s() == 0u && player_total_s() == 0u);

    return xfail_done(&c);
}
