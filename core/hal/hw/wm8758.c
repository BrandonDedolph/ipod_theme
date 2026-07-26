/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/wm8758.c — Wolfson WM8758B codec bring-up for 44.1 kHz
 * I2S playback.
 *
 * Reaches the codec over the SoC I2C controller (i2c.c). Register/bit
 * numbers are the WM8758B datasheet's own register map (see wm8758.h).
 * The init order follows the datasheet's power-up requirements — the
 * "Recommended Power Up Sequence" (bias/VMID and POBCTRL pop-suppression
 * before the output rails, VMID 10K fast-charge then 500K hold), I2S
 * interface + PLL for 44.1 kHz, DAC routed to the mixers, then outputs
 * un-muted last with the zero-cross + volume-update latches. It is
 * expressed here as a data table so the exact bus grammar is easy to
 * assert in the trace tests.
 *
 * Asm-free so it host-compiles for the mock-bus tests. It is NO LONGER
 * delay-free: the datasheet's VMID settle between the 10k fast-charge and the
 * 500k hold is supplied here (a bounded USEC_TIMER wait), because the one
 * caller never did and this now runs once per track — i.e. the missing settle
 * was a pop at every track boundary, not just at boot.
 */

#include "wm8758.h"
#include "i2c.h"
#include "pp5022.h"
#include "mmio.h"

/*
 * WM8758 control word: 7-bit register in bits 15:9, 9-bit data in 8:0,
 * packed into two I2C payload bytes — byte0 = (reg<<1) | data bit 8,
 * byte1 = data low 8 bits (05-audio.md, "DAC I²C interface").
 *
 * Returns i2c_send's status. NOTE what that status can and cannot tell you:
 * the PP502x controller exposes NO per-byte ACK/NAK bit (09-i2c.md lists only
 * BUSY in I2C_STATUS), so a codec that is absent, unpowered or holding SDA
 * cannot be distinguished from a healthy one by looking at the bus. The one
 * real signal is the BUSY-clear timeout: if the controller never finishes a
 * transaction the bus is wedged. That is the only failure this can report, and
 * it is worth reporting — the alternative failure mode is a UI with a moving
 * progress bar and total silence.
 */
static int wm8758_write(uint8_t reg, uint16_t data)
{
    uint8_t frame[2];
    frame[0] = (uint8_t)((reg << 1) | ((data >> 8) & 0x1));
    frame[1] = (uint8_t)(data & 0xFF);
    return i2c_send(WM8758_I2C_ADDR, frame, 2);
}

struct wm_write {
    uint8_t  reg;
    uint16_t data;
};

/*
 * Moderate output level for first sound. Headphone amp gain 0x39 = 0 dB
 * (the codec reset default); the DAC digital volume is full-scale. The
 * source tone is generated at ~-8.7 dBFS, which lands at a reasonable
 * listening level. Tune down here if it is too loud on sensitive IEMs.
 */
#define WM_HP_GAIN_0DB 0x39

/*
 * VMID settle between the 10k fast-charge and the 500k hold (05-audio.md, the
 * datasheet pop-suppression sequence). The file header used to declare itself
 * "delay-free ... any VMID settle delay is the caller's responsibility" — and
 * the only caller runs i2c_init(); i2s_init(); wm8758_init(); dma_playback_init()
 * back to back, so the delay never existed. Now that wm8758_init runs once per
 * TRACK, that omission is a pop at every track boundary, so the driver supplies
 * it here where the sequence actually needs it.
 *
 * DEVICE-GATED: 40 ms is a compromise, not a measured value — long enough for
 * a meaningful fraction of the 10k charge, short enough to hide inside a track
 * change (which already spins the disk and refills the decode buffer). If a
 * pop is still audible on hardware, raise it; this constant is the knob.
 */
#define WM_VMID_SETTLE_US   40000u

/*
 * Guard on the settle loop. USEC_TIMER is a free-running 1 MHz counter so on
 * silicon the elapsed test always terminates; the trip cap is the same
 * "no unbounded loops, ever" discipline the rest of the HAL follows, and it is
 * what lets this run under the host mock bus (whose fake counter never
 * advances). Same MMIO_MOCK split kernel/clock.c uses for its PLL-lock spin,
 * and for the same reason: a full-size never-advances spin would flood the
 * recording bus's fixed-capacity event log.
 */
#ifdef MMIO_MOCK
#define WM_SETTLE_GUARD_TRIPS  4u
#else
#define WM_SETTLE_GUARD_TRIPS  (1u << 24)
#endif

static void wm8758_settle_us(uint32_t us)
{
    uint32_t t0    = mmio_read32(USEC_TIMER_ADDR);
    uint32_t guard = WM_SETTLE_GUARD_TRIPS;
    while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < us && --guard != 0) {
        /* wait */
    }
}

/*
 * Per-rate PLL + divider program (05-audio.md, "DAC sample-rate setup" and the
 * resolved 44.1 kHz appendix). Six writes, always in this order. SYSCLK =
 * fPLLOUT / MCLKDIV and must equal exactly 256 x fs:
 *
 *   44100 : preset 0 (22.5792 MHz) / 2 = 11.2896 MHz = 256 x 44100
 *   22050 : preset 0 (22.5792 MHz) / 4 =  5.6448 MHz = 256 x 22050
 *   48000 : preset 1 (24.576  MHz) / 2 = 12.288  MHz = 256 x 48000
 *   32000 : preset 1 (24.576  MHz) / 3 =  8.192  MHz = 256 x 32000
 *   24000 : preset 1 (24.576  MHz) / 4 =  6.144  MHz = 256 x 24000
 *
 * The ADDCTRL SR field is only a filter-class hint, so each rate carries its
 * nearest class rather than an exact match (44.1 kHz uses the 48 kHz class —
 * that is the documented, intended behaviour, not an oversight).
 */
struct wm_rate {
    uint32_t rate;
    uint16_t plln, pllk1, pllk2, pllk3;
    uint16_t clkctrl;
    uint16_t addctrl;
};

#define WM_CLKCTRL_BASE  (CLKCTRL_CLKSEL | CLKCTRL_BCLKDIV_2 | CLKCTRL_MS)

static const struct wm_rate rate_table[] = {
    { 44100u, WM_PLLN_44, WM_PLLK1_44, WM_PLLK2_44, WM_PLLK3_44,
      WM_CLKCTRL_BASE | CLKCTRL_MCLKDIV_2, ADDCTRL_SR_48kHz | ADDCTRL_SLOWCLKEN },
    { 48000u, WM_PLLN_48, WM_PLLK1_48, WM_PLLK2_48, WM_PLLK3_48,
      WM_CLKCTRL_BASE | CLKCTRL_MCLKDIV_2, ADDCTRL_SR_48kHz | ADDCTRL_SLOWCLKEN },
    { 32000u, WM_PLLN_48, WM_PLLK1_48, WM_PLLK2_48, WM_PLLK3_48,
      WM_CLKCTRL_BASE | CLKCTRL_MCLKDIV_3, ADDCTRL_SR_32kHz | ADDCTRL_SLOWCLKEN },
    { 24000u, WM_PLLN_48, WM_PLLK1_48, WM_PLLK2_48, WM_PLLK3_48,
      WM_CLKCTRL_BASE | CLKCTRL_MCLKDIV_4, ADDCTRL_SR_24kHz | ADDCTRL_SLOWCLKEN },
    { 22050u, WM_PLLN_44, WM_PLLK1_44, WM_PLLK2_44, WM_PLLK3_44,
      WM_CLKCTRL_BASE | CLKCTRL_MCLKDIV_4, ADDCTRL_SR_24kHz | ADDCTRL_SLOWCLKEN },
};

/* Index into rate_table; 0 (44.1 kHz) until wm8758_set_rate says otherwise. */
static unsigned g_rate_idx;

int wm8758_set_rate(uint32_t sample_rate)
{
    for (unsigned i = 0; i < sizeof rate_table / sizeof rate_table[0]; i++) {
        if (rate_table[i].rate == sample_rate) {
            g_rate_idx = i;
            return 0;
        }
    }
    return -1;      /* unsupported: leave the previous selection in place */
}

/* Callback run at the tail of wm8758_init (see wm8758.h). */
static void (*g_restore)(void);

void wm8758_set_restore(void (*fn)(void))
{
    g_restore = fn;
}

/*
 * Bring-up, part A: soft reset through the VMID 10k fast-charge, then the
 * fixed interface config. Everything before the rate program.
 */
static const struct wm_write init_seq_a[] = {
    /* --- soft reset to known power-on defaults ---------------------- *
     * We chainload after Apple's flash ROM (and possibly disk mode),
     * which may have left codec state in registers this sequence does
     * not touch (ADC path, EQ, limiter, ALC, input mux). A reset (write
     * any value to reg 0x00) guarantees we start from datasheet defaults
     * rather than inherited state. The I2C control port works with or
     * without MCLK, so this is safe as step 0. */
    { WM_RESET,     0 },

    /* --- preinit: bias + protection, everything muted --------------- */
    { WM_BIASCTRL,  BIASCTRL_BIASCUT },
    { WM_OUTCTRL,   OUTCTRL_HP_COM | OUTCTRL_LINE_COM | OUTCTRL_TSOPCTRL
                    | OUTCTRL_TSDEN | OUTCTRL_VROI },
    { WM_LOUT1VOL,  OUTVOL_VU | OUTVOL_MUTE },   /* 0x140 */
    { WM_ROUT1VOL,  OUTVOL_VU | OUTVOL_MUTE },
    { WM_LOUT2VOL,  OUTVOL_VU | OUTVOL_MUTE },
    { WM_ROUT2VOL,  OUTVOL_VU | OUTVOL_MUTE },
    { WM_OUT3MIX,   OUTMIX_MUTE },               /* 0x40 */
    { WM_OUT4MIX,   OUTMIX_MUTE },

    /* --- power rails up in order ------------------------------------ */
    { WM_PWRMGMT2,  PWRMGMT2_LOUT1EN | PWRMGMT2_ROUT1EN },
    { WM_OUT4TOADC, OUT4TOADC_POBCTRL },         /* VMID-independent bias */
    { WM_PWRMGMT3,  PWRMGMT3_DACENL | PWRMGMT3_DACENR
                    | PWRMGMT3_LMIXEN | PWRMGMT3_RMIXEN },
    { WM_PWRMGMT1,  PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                    | PWRMGMT1_BUFIOEN | PWRMGMT1_VMIDSEL_10K },

    /* --- interface + clocking: I2S 16-bit, codec is master ---------- */
    { WM_AINTFCE,   AINTFCE_FORMAT_I2S | AINTFCE_IWL_16BIT },
    { WM_CLKCTRL,   CLKCTRL_MS },
};

/*
 * Bring-up, part B: the DAC route, issued after the rate program. Ends just
 * before the VMID settle.
 */
static const struct wm_write init_seq_b[] = {
    /* --- route DAC to the output mixers (without this: silence) ----- */
    { WM_LOUTMIX,   LOUTMIX_DACL2LMIX },
    { WM_ROUTMIX,   ROUTMIX_DACR2RMIX },
    { WM_OUT4TOADC, 0 },                         /* drop the bias toggle */
};

/*
 * Bring-up, part C: everything AFTER the VMID settle — hand VMID over to the
 * low-power 500k hold, drop the low-bias, then volume and unmute last.
 */
static const struct wm_write init_seq_c[] = {
    /* --- postinit: low-power VMID hold, clear low-bias -------------- */
    { WM_PWRMGMT1,  PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                    | PWRMGMT1_BUFIOEN | PWRMGMT1_VMIDSEL_500K },
    { WM_BIASCTRL,  0 },

    /* --- volume + unmute -------------------------------------------- */
    { WM_LDACVOL,   DACVOL_MASK },               /* full-scale, no VU yet */
    { WM_RDACVOL,   DACVOL_MASK | DACVOL_DACVU }, /* VU latches L+R */
    { WM_LOUT1VOL,  WM_HP_GAIN_0DB | OUTVOL_ZC },
    { WM_ROUT1VOL,  WM_HP_GAIN_0DB | OUTVOL_ZC | OUTVOL_VU },
    { WM_DACCTRL,   DACCTRL_DACOSR128 },         /* unmute (128x OSR) */
};

/* Run one table, accumulating the failed-write count. */
static int wm8758_run(const struct wm_write *seq, unsigned n)
{
    int bad = 0;
    for (unsigned i = 0; i < n; i++) {
        if (wm8758_write(seq[i].reg, seq[i].data) != 0) {
            bad++;
        }
    }
    return bad;
}

#define WM_RUN(tbl) wm8758_run((tbl), sizeof (tbl) / sizeof (tbl)[0])

int wm8758_init(void)
{
    const struct wm_rate *r = &rate_table[g_rate_idx];
    int bad = 0;

    bad += WM_RUN(init_seq_a);

    /* Rate program: six writes, fixed order (05-audio.md's resolved
     * sequence), values from the per-rate table. */
    bad += (wm8758_write(WM_PLLN,    r->plln)    != 0);
    bad += (wm8758_write(WM_PLLK1,   r->pllk1)   != 0);
    bad += (wm8758_write(WM_PLLK2,   r->pllk2)   != 0);
    bad += (wm8758_write(WM_PLLK3,   r->pllk3)   != 0);
    bad += (wm8758_write(WM_CLKCTRL, r->clkctrl) != 0);
    bad += (wm8758_write(WM_ADDCTRL, r->addctrl) != 0);

    bad += WM_RUN(init_seq_b);

    /* THE VMID SETTLE. Bias is still on the 10k fast-charge path set in
     * init_seq_a; hold here so the rail is actually up before we hand it to
     * the 500k keeper and unmute. Skipping it is the pop. */
    wm8758_settle_us(WM_VMID_SETTLE_US);

    bad += WM_RUN(init_seq_c);

    /* Finally, put the user's settings back. The WM_RESET at the top of this
     * function wiped volume, balance and the EQ back to datasheet defaults,
     * and this runs once per track — without this the codec would come up at
     * 0 dB with flat tone on every track change. Deliberately AFTER the
     * unmute: no PCM is flowing yet (hal_audio_start has not been called), so
     * there is nothing to click, and it keeps the documented bring-up
     * sequence itself byte-for-byte unchanged. */
    if (g_restore) {
        g_restore();
    }
    return bad;
}

void wm8758_mute(bool mute)
{
    (void)wm8758_write(WM_DACCTRL, mute ? DACCTRL_SOFTMUTE : DACCTRL_DACOSR128);
}

/*
 * Pop-suppressed power-DOWN — the datasheet "Recommended Power Down Sequence"
 * run in the reverse spirit of init_seq: soft-mute the DAC and mute the
 * headphone outputs FIRST (so nothing is live when the rails collapse), assert
 * POBCTRL + VMIDTOG to discharge VMID through the pop-suppression path, then
 * drop the output amps, the VMID/BIAS/PLL rail, and the DAC/mixers. Leaves the
 * codec cold; the next wm8758_init() (issued per track via hal_audio_init) does
 * a full WM_RESET + pop-suppressed bring-up, so this is fully recoverable and
 * resume audio is clean. Delay-free (MCLK is still running when this is called;
 * the caller gates clocks AFTER).
 */
static const struct wm_write powerdown_seq[] = {
    { WM_DACCTRL,   DACCTRL_SOFTMUTE },                       /* ramp DAC to mute   */
    { WM_LOUT1VOL,  OUTVOL_VU | OUTVOL_MUTE },                /* mute HP outputs    */
    { WM_ROUT1VOL,  OUTVOL_VU | OUTVOL_MUTE },
    { WM_OUT4TOADC, OUT4TOADC_POBCTRL | OUT4TOADC_VMIDTOG },  /* pop ctrl + VMID discharge */
    { WM_PWRMGMT2,  0 },                                      /* output amps off    */
    { WM_PWRMGMT1,  0 },                                      /* VMID + BIAS + PLL off */
    { WM_PWRMGMT3,  0 },                                      /* DAC + mixers off   */
};

void wm8758_powerdown(void)
{
    (void)WM_RUN(powerdown_seq);
}
