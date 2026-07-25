/*
 * core/hal/hw/wm8758.h — Wolfson WM8758B codec register + bit constants.
 *
 * The WM8758B is reached over the SoC I2C controller (see
 * core/hal/hw/i2c.h and core/docs/hw/09-i2c.md), NOT via MMIO — so this
 * header carries no memory addresses, only 7-bit register numbers and
 * 9-bit data bit values. The register map and bit fields below are the
 * WM8758B datasheet's own — register numbers are the datasheet's R0..R61
 * (R0 Software Reset, R1..R3 Power Management, R4 Audio Interface, R6/R7
 * Clock/Sample control, R10..R12 DAC, R36..R39 PLL, R18..R22 EQ, R49..R57
 * Output mixers/volumes, R61 Bias control), and the mnemonics are Wolfson's
 * (VMIDSEL, BIASEN, POBCTRL, …). That datasheet is the primary source; the
 * concrete init values are cross-referenced in core/docs/hw/05-audio.md,
 * "Numeric register reference".
 *
 * Register data is 9 bits: several bits (the volume-update VU latches,
 * some enable bits, PLLK2 MSBs) live at bit 8 and ride the I2C framing's
 * first-byte LSB — see wm8758_write() in wm8758.c.
 */

#ifndef CORE_HAL_HW_WM8758_H
#define CORE_HAL_HW_WM8758_H

/* 7-bit I2C device address of the iPod audio codec (05-audio.md). */
#define WM8758_I2C_ADDR   0x1A

/* ---- Register numbers (7-bit) --------------------------------------- */
#define WM_RESET      0x00
#define WM_PWRMGMT1   0x01
#define WM_PWRMGMT2   0x02
#define WM_PWRMGMT3   0x03
#define WM_AINTFCE    0x04
#define WM_CLKCTRL    0x06
#define WM_ADDCTRL    0x07
#define WM_DACCTRL    0x0A
#define WM_LDACVOL    0x0B
#define WM_RDACVOL    0x0C
#define WM_PLLN       0x24
#define WM_PLLK1      0x25
#define WM_PLLK2      0x26
#define WM_PLLK3      0x27
#define WM_EQ1        0x12  /* low shelf  (Bass)  */
#define WM_EQ2        0x13  /* peaking band 2     */
#define WM_EQ3        0x14  /* peaking band 3     */
#define WM_EQ4        0x15  /* peaking band 4     */
#define WM_EQ5        0x16  /* high shelf (Treble) */
#define WM_OUT4TOADC  0x2A
#define WM_OUTCTRL    0x31
#define WM_LOUTMIX    0x32
#define WM_ROUTMIX    0x33
#define WM_LOUT1VOL   0x34
#define WM_ROUT1VOL   0x35
#define WM_LOUT2VOL   0x36
#define WM_ROUT2VOL   0x37
#define WM_OUT3MIX    0x38
#define WM_OUT4MIX    0x39
#define WM_BIASCTRL   0x3D

/* ---- PWRMGMT1 (0x01) ------------------------------------------------ */
#define PWRMGMT1_VMIDSEL_OFF   0x000
#define PWRMGMT1_VMIDSEL_500K  0x002  /* low-power hold */
#define PWRMGMT1_VMIDSEL_10K   0x003  /* fast startup   */
#define PWRMGMT1_BUFIOEN       0x004
#define PWRMGMT1_BIASEN        0x008
#define PWRMGMT1_PLLEN         0x020

/* ---- PWRMGMT2 (0x02) ------------------------------------------------ */
#define PWRMGMT2_LOUT1EN       0x080
#define PWRMGMT2_ROUT1EN       0x100

/* ---- PWRMGMT3 (0x03) ------------------------------------------------ */
#define PWRMGMT3_DACENL        0x001
#define PWRMGMT3_DACENR        0x002
#define PWRMGMT3_LMIXEN        0x004
#define PWRMGMT3_RMIXEN        0x008
#define PWRMGMT3_ROUT2EN       0x020
#define PWRMGMT3_LOUT2EN       0x040

/* ---- AINTFCE (0x04) ------------------------------------------------- */
#define AINTFCE_FORMAT_I2S     0x010  /* 2<<3 */
#define AINTFCE_IWL_16BIT      0x000

/* ---- CLKCTRL (0x06) ------------------------------------------------- */
#define CLKCTRL_MS             0x001  /* codec is I2S clock master */
#define CLKCTRL_BCLKDIV_2      0x004
#define CLKCTRL_MCLKDIV_2      0x040
#define CLKCTRL_CLKSEL         0x100  /* clock source = PLL */

/* ---- ADDCTRL (0x07) — SR is a filter-class hint, not the real rate -- */
#define ADDCTRL_SLOWCLKEN      0x001
#define ADDCTRL_SR_48kHz       0x000

/* ---- DACCTRL (0x0A) ------------------------------------------------- */
#define DACCTRL_DACOSR128      0x008  /* 128x oversample (the "unmuted" state) */
#define DACCTRL_SOFTMUTE       0x040

/* ---- LDACVOL / RDACVOL (0x0B / 0x0C) -------------------------------- */
#define DACVOL_MASK            0x0FF
#define DACVOL_DACVU           0x100  /* latch L+R DAC volume now */

/* ---- PLLN (0x24) ---------------------------------------------------- */
#define PLLN_PLLPRESCALE       0x010

/* ---- OUT4TOADC (0x2A) ----------------------------------------------- */
#define OUT4TOADC_POBCTRL      0x004  /* VMID-independent bias / pop control */
#define OUT4TOADC_VMIDTOG      0x010  /* VMID discharge toggle (close)       */

/* ---- OUTCTRL (0x31) ------------------------------------------------- */
#define OUTCTRL_VROI           0x001
#define OUTCTRL_TSDEN          0x002
#define OUTCTRL_TSOPCTRL       0x004
#define OUTCTRL_LINE_COM       0x080
#define OUTCTRL_HP_COM         0x100

/* ---- LOUTMIX / ROUTMIX (0x32 / 0x33) -------------------------------- */
#define LOUTMIX_DACL2LMIX      0x001
#define ROUTMIX_DACR2RMIX      0x001

/* ---- OUT1/OUT2 volume (0x34-0x37) — shared bit layout --------------- */
#define OUTVOL_GAIN_MASK       0x03F  /* 6-bit amp gain */
#define OUTVOL_MUTE            0x040
#define OUTVOL_ZC              0x080  /* zero-cross gain change */
#define OUTVOL_VU              0x100  /* latch L+R gain now */

/* ---- OUT3MIX / OUT4MIX (0x38 / 0x39) -------------------------------- */
#define OUTMIX_MUTE            0x040  /* preinit mute value for OUT3/4 mixers */

/* ---- BIASCTRL (0x3D) ------------------------------------------------ */
#define BIASCTRL_BIASCUT       0x100  /* low-power bias cut */

/* ---- EQ1..EQ5 (0x12-0x16): 5-band EQ tone control (05-audio.md) -----
 * Gain field EQxG[4:0] uses code = 12 - gain_dB (0x0C = 0 dB flat, 0x00 =
 * +12 dB, 0x18 = -12 dB). EQ3DMODE (EQ1 bit 8) routes the EQ to the DAC
 * (playback) when set; cleared it sits on the ADC path (inert here). */
#define EQ_GAIN_MASK           0x01F  /* EQxG[4:0]                        */
#define EQ_GAIN_0DB            0x00C  /* flat: code = 12 - 0 dB           */
#define EQ_DAC_MODE            0x100  /* EQ1 bit 8: apply EQ to the DAC   */
#define EQ1_CUTOFF_105HZ       0x020  /* EQ1C=01: bass shelf @ 105 Hz     */
#define EQ5_CUTOFF_6K9         0x020  /* EQ5C=01: treble shelf @ 6.9 kHz  */

/* ---- Sample-rate program: 44.1 kHz (05-audio.md, resolved) ---------
 * PLL preset 0 -> fPLLOUT 22.5792 MHz; MCLKDIV/2 -> SYSCLK = 256*44.1kHz.
 * The SR field intentionally uses the 48 kHz class value; the true rate
 * comes from the PLL, not the SR hint. */
#define WM_PLLN_44     (PLLN_PLLPRESCALE | 0x7)                             /* 0x17 */
#define WM_PLLK1_44    0x21
#define WM_PLLK2_44    0x161
#define WM_PLLK3_44    0x26
#define WM_CLKCTRL_44  (CLKCTRL_CLKSEL | CLKCTRL_MCLKDIV_2 | \
                        CLKCTRL_BCLKDIV_2 | CLKCTRL_MS)                     /* 0x145 */
#define WM_ADDCTRL_44  (ADDCTRL_SR_48kHz | ADDCTRL_SLOWCLKEN)              /* 0x001 */

/* ---- Codec driver API (wm8758.c) ------------------------------------
 * Requires the SoC I2C controller to be initialised first (i2c_init).
 * wm8758_init brings the codec fully up for 44.1 kHz I2S playback: power
 * rails, I2S 16-bit format, DAC PLL, DAC->output routing, and unmute at a
 * moderate volume. Pure register writes (no CPU-timed delays), so a
 * caller wanting a VMID settle before the first sample inserts its own.
 */
#ifndef __ASSEMBLER__
#include <stdbool.h>

void wm8758_init(void);
void wm8758_mute(bool mute);

/* Pop-suppressed power-down: mute, discharge VMID, drop all power rails. The
 * codec is left cold — call wm8758_init() (per track via hal_audio_init) to
 * bring it back. Call while MCLK is still running; gate clocks afterwards. */
void wm8758_powerdown(void);
#endif

#endif /* CORE_HAL_HW_WM8758_H */
