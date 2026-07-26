/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/volume.c — WM8758B output-volume HAL.
 *
 * Maps a 0..100% UI level onto the codec's HEADPHONE amp gain field
 * (LOUT1VOL/ROUT1VOL, regs 0x34/0x35), reusing the same I2C control-word
 * grammar as wm8758.c. The DAC digital volume is left at full scale by
 * wm8758_init(), so the analog amp gain is the single audible knob here.
 *
 * REGISTER CHOICE (why OUT1 amp gain, not DAC digital volume):
 *   - LOUT1VOL/ROUT1VOL are a 6-bit field (OUTVOL_GAIN_MASK = 0x3F),
 *     ~-57..+6 dB in 1 dB steps, with 0x39 = 0 dB and 0x3F = +6 dB
 *     (core/docs/hw/05-audio.md, "Volume control"; the 0x39=0 dB point is
 *     also the codec reset default used by wm8758_init()).
 *   - The MUTE bit (OUTVOL_MUTE = 0x40) is separate from the gain field
 *     (0x00 is -57 dB, NOT silence), so 0% asserts MUTE explicitly.
 *   - The ZC bit (OUTVOL_ZC = 0x80) makes gain changes wait for a signal
 *     zero-crossing (no zipper noise); we set it on every write.
 *   - The VU bit (OUTVOL_VU = 0x100) on the RIGHT write latches both
 *     channels together — the classic Wolfson "write L, then write R with
 *     update" pattern.
 * The DAC digital volume (LDACVOL/RDACVOL, 0x0B/0x0C) is a global 8-bit
 * attenuator shared by every output path; using it for the user knob
 * would also scale the line-out and is left full-scale.
 *
 * CLEANROOM NOTE: register numbers, bit positions and the 0x39=0 dB
 * headphone-gain fact are transcribed from core/docs/hw/05-audio.md
 * (which resolves the Wolfson/Rockbox mnemonics to concrete numbers).
 * No Rockbox code body is copied — the percent curve and write sequence
 * below are this project's own.
 */

#include "volume.h"
#include "wm8758.h"
#include "i2c.h"

/*
 * WM8758 control word (same framing as wm8758.c's private helper): 7-bit
 * register in bits 15:9, 9-bit data in 8:0 -> two I2C payload bytes,
 * byte0 = (reg<<1) | data bit 8, byte1 = data low 8 bits. wm8758.c keeps
 * its writer file-static, and this driver must not edit that file, so we
 * mirror the exact grammar here.
 */
static void codec_write(uint8_t reg, uint16_t data)
{
    uint8_t frame[2];
    frame[0] = (uint8_t)((reg << 1) | ((data >> 8) & 0x1));
    frame[1] = (uint8_t)(data & 0xFF);
    (void)i2c_send(WM8758_I2C_ADDR, frame, 2);
}

/*
 * Usable amp-gain range for the 1..100% span.
 *   VOL_GAIN_0DB  = 0x39  -> 0 dB at 100% (unity; NOT the +6 dB top, so a
 *                            full-scale sample cannot clip the amp).
 *   VOL_GAIN_FLOOR= 0x06  -> ~-51 dB at 1% (quiet but audible; the very
 *                            bottom of the field is effectively inaudible,
 *                            so we do not map the UI onto it). 0% is a
 *                            hard mute via OUTVOL_MUTE instead.
 */
#define VOL_GAIN_0DB    0x39
#define VOL_GAIN_FLOOR  0x06

/* Default output level at bring-up: moderate, headroom below unity. */
#define VOL_DEFAULT_PCT 70

/*
 * Cached codec state. This driver is the single owner of everything the user
 * can change on the codec, and it must be able to reconstruct all of it from
 * RAM: hal_audio_init() issues a full WM_RESET once per track, so any setting
 * that lives only in a codec register is gone at every track boundary. See
 * hal_codec_restore() at the bottom of this file.
 */
static int g_percent = VOL_DEFAULT_PCT;
static int g_balance = 0;                /* -100 (full left) .. +100 (full right) */
static int g_bass_db;                    /* -12..+12, 0 = flat (EQ left inert)    */
static int g_treble_db;                  /* -12..+12, 0 = flat                    */

uint16_t hal_volume_out1_word(int percent)
{
    if (percent <= 0) {
        /* Hard mute: the gain field is don't-care under MUTE; keep ZC so
         * the un-mute later still lands on a zero-crossing. */
        return (uint16_t)(OUTVOL_MUTE | OUTVOL_ZC);
    }
    if (percent > 100) {
        percent = 100;
    }

    /* Linear 1..100% -> [FLOOR .. 0 dB] gain codes. Rounded so 100 lands
     * exactly on VOL_GAIN_0DB and the map is monotonic non-decreasing. */
    int span = VOL_GAIN_0DB - VOL_GAIN_FLOOR;               /* 0x33 = 51 */
    int code = VOL_GAIN_FLOOR + (span * percent + 50) / 100;
    if (code > VOL_GAIN_0DB) {
        code = VOL_GAIN_0DB;                                /* belt + braces */
    }

    return (uint16_t)((code & OUTVOL_GAIN_MASK) | OUTVOL_ZC);
}

/* Build an OUT1VOL data word (ZC set, no VU) for an absolute gain code, muting
 * if the code has been panned below the audible floor. */
static uint16_t out1_word_for_code(int code)
{
    if (code < VOL_GAIN_FLOOR) {
        return (uint16_t)(OUTVOL_MUTE | OUTVOL_ZC);
    }
    if (code > VOL_GAIN_0DB) {
        code = VOL_GAIN_0DB;
    }
    return (uint16_t)((code & OUTVOL_GAIN_MASK) | OUTVOL_ZC);
}

/*
 * Latch the current (percent, balance) into LOUT1VOL/ROUT1VOL. Balance pans by
 * attenuating one channel's amp gain: at 0 both channels get the identical
 * center word (byte-for-byte the old behaviour), so nothing changes when
 * balance is untouched; toward an end, the FAR channel's gain code is walked
 * down by up to the full span and hard-muted at the extreme.
 */
static void volume_latch(void)
{
    uint16_t lword, rword;
    if (g_percent <= 0) {
        lword = rword = (uint16_t)(OUTVOL_MUTE | OUTVOL_ZC);
    } else if (g_balance == 0) {
        lword = rword = hal_volume_out1_word(g_percent);   /* exact center path */
    } else {
        int span = VOL_GAIN_0DB - VOL_GAIN_FLOOR;
        int code = VOL_GAIN_FLOOR + (span * g_percent + 50) / 100;
        if (code > VOL_GAIN_0DB) {
            code = VOL_GAIN_0DB;
        }
        int mag   = g_balance < 0 ? -g_balance : g_balance;
        int atten = span * mag / 100;              /* full pan -> down to floor */
        int lcode = code, rcode = code;
        if (g_balance > 0) {
            lcode -= atten;                        /* pan right: cut left  */
        } else {
            rcode -= atten;                        /* pan left:  cut right */
        }
        lword = (g_balance > 0 && mag >= 100) ? (uint16_t)(OUTVOL_MUTE | OUTVOL_ZC)
                                              : out1_word_for_code(lcode);
        rword = (g_balance < 0 && mag >= 100) ? (uint16_t)(OUTVOL_MUTE | OUTVOL_ZC)
                                              : out1_word_for_code(rcode);
    }

    /* Left first, then right with the update bit so both latch together. */
    codec_write(WM_LOUT1VOL, lword);
    codec_write(WM_ROUT1VOL, (uint16_t)(rword | OUTVOL_VU));
}

void hal_volume_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    g_percent = percent;
    volume_latch();
}

void hal_balance_set(int balance)
{
    if (balance < -100) {
        balance = -100;
    } else if (balance > 100) {
        balance = 100;
    }
    g_balance = balance;
    volume_latch();
}

int hal_balance_get(void)
{
    return g_balance;
}

/* EQxG gain code for a signed dB (05-audio.md: code = 12 - dB), clamped to the
 * ±12 dB field. */
static uint16_t eq_gain_code(int db)
{
    if (db >  12) db =  12;
    if (db < -12) db = -12;
    return (uint16_t)((12 - db) & EQ_GAIN_MASK);
}

void hal_tone_set(int bass_db, int treble_db)
{
    if (bass_db   >  12) bass_db   =  12; else if (bass_db   < -12) bass_db   = -12;
    if (treble_db >  12) treble_db =  12; else if (treble_db < -12) treble_db = -12;
    g_bass_db   = bass_db;      /* cached so hal_codec_restore can rebuild it */
    g_treble_db = treble_db;

    /* Only route the EQ onto the DAC when the user has actually dialed in
     * some tone; at flat 0/0 it stays on the (silent) ADC path so playback is
     * bit-identical to no-EQ. The three mid bands are held flat. */
    uint16_t dac = (bass_db != 0 || treble_db != 0) ? EQ_DAC_MODE : 0;

    codec_write(WM_EQ1, (uint16_t)(dac | EQ1_CUTOFF_105HZ | eq_gain_code(bass_db)));
    codec_write(WM_EQ2, EQ_GAIN_0DB);
    codec_write(WM_EQ3, EQ_GAIN_0DB);
    codec_write(WM_EQ4, EQ_GAIN_0DB);
    codec_write(WM_EQ5, (uint16_t)(EQ5_CUTOFF_6K9 | eq_gain_code(treble_db)));
}

void hal_volume_init(void)
{
    hal_volume_set(VOL_DEFAULT_PCT);
}

int hal_volume_get(void)
{
    return g_percent;
}

/*
 * Push the whole cached state back into a freshly reset codec. Called from
 * wm8758_init() through the wm8758_set_restore() hook (registered by
 * hal/hw/audio.c), so it runs at the tail of every per-track bring-up.
 *
 * Order: gains first, then EQ. Both are ordinary latched writes — the OUT1
 * pair carries the VU bit on the right write, the EQ bands are independent —
 * so there is no interaction to sequence beyond "after the rails are up",
 * which the caller guarantees by construction.
 *
 * Deliberately unconditional: writing the defaults back when nothing was
 * changed costs seven I2C transactions (~a millisecond) once per track and
 * removes an entire class of "was it dirty?" reasoning.
 */
void hal_codec_restore(void)
{
    volume_latch();                         /* volume + balance -> OUT1VOL pair */
    hal_tone_set(g_bass_db, g_treble_db);   /* bass + treble    -> EQ1..EQ5     */
}
