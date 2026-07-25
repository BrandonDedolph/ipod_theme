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
 * Asm-free and delay-free so it host-compiles for the mock-bus tests;
 * any VMID settle delay is the caller's responsibility.
 */

#include "wm8758.h"
#include "i2c.h"

/*
 * WM8758 control word: 7-bit register in bits 15:9, 9-bit data in 8:0,
 * packed into two I2C payload bytes — byte0 = (reg<<1) | data bit 8,
 * byte1 = data low 8 bits (05-audio.md, "DAC I²C interface").
 */
static void wm8758_write(uint8_t reg, uint16_t data)
{
    uint8_t frame[2];
    frame[0] = (uint8_t)((reg << 1) | ((data >> 8) & 0x1));
    frame[1] = (uint8_t)(data & 0xFF);
    (void)i2c_send(WM8758_I2C_ADDR, frame, 2);
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

static const struct wm_write init_seq[] = {
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

    /* --- 44.1 kHz DAC PLL + dividers (05-audio.md, resolved) -------- */
    { WM_PLLN,      WM_PLLN_44 },
    { WM_PLLK1,     WM_PLLK1_44 },
    { WM_PLLK2,     WM_PLLK2_44 },
    { WM_PLLK3,     WM_PLLK3_44 },
    { WM_CLKCTRL,   WM_CLKCTRL_44 },
    { WM_ADDCTRL,   WM_ADDCTRL_44 },

    /* --- route DAC to the output mixers (without this: silence) ----- */
    { WM_LOUTMIX,   LOUTMIX_DACL2LMIX },
    { WM_ROUTMIX,   ROUTMIX_DACR2RMIX },
    { WM_OUT4TOADC, 0 },                         /* drop the bias toggle */

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

void wm8758_init(void)
{
    for (unsigned i = 0; i < sizeof init_seq / sizeof init_seq[0]; i++) {
        wm8758_write(init_seq[i].reg, init_seq[i].data);
    }
}

void wm8758_mute(bool mute)
{
    wm8758_write(WM_DACCTRL, mute ? DACCTRL_SOFTMUTE : DACCTRL_DACOSR128);
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
    for (unsigned i = 0; i < sizeof powerdown_seq / sizeof powerdown_seq[0]; i++) {
        wm8758_write(powerdown_seq[i].reg, powerdown_seq[i].data);
    }
}
