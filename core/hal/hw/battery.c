/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/battery.c — battery gauge (PCF50605 PMU ADC over I2C) +
 * charge/power-state (plain GPIO input bits).
 *
 * Implements core/docs/hw/06-power.md. Freestanding: <stdint.h> only,
 * hardware touched exclusively through the mmio.h seam and the i2c.c
 * driver, so this host-compiles unchanged for the golden-trace test.
 *
 * WHY the split path:
 *   - Voltage lives behind the PMU's internal 10-bit ADC, reachable only
 *     over the shared on-SoC I2C bus (device 0x08). It costs a bus
 *     round-trip and its scaling is device-gated (see below).
 *   - Charger-present / charging are three GPIO pins with NO PMU/I2C
 *     involvement, so those two calls are near-free and calibration-safe.
 *
 * DEVICE-GATED (cannot be validated off-hardware): the x6000>>10 scaling
 * and the percent curve are the 2005 Apple-cell values. On real hardware,
 * display raw millivolts FIRST and confirm the ~3300..4200 mV range
 * before trusting battery_percent() or any shutdown threshold. Replacement
 * cells commonly read high at the low (steep) end of the curve.
 */

#include <stdint.h>

#include "pp5022.h"
#include "mmio.h"
#include "i2c.h"
#include "battery.h"

/* ---------- PCF50605 PMU (battery ADC over I2C) --------------------- */

/* 7-bit I2C device address of the PMU — the same bus the codec uses. */
#define PMU_ADDR            0x08

/*
 * ADC control/select register (ADCC1). Written to pick a channel and
 * start a conversion in one byte: (channel << 1) | START. The battery
 * sits on channel ADCVIN1 (0x2), which has the PMU's built-in resistive
 * divider, so the assembled value is (0x2 << 1) | 0x1 = 0x05.
 */
#define PMU_ADCC1           0x2F
#define PMU_ADC_START_VIN1  0x05

/*
 * ADC result registers. ADCS1 (0x30) holds the high 8 bits, ADCS2 (0x31)
 * holds the low 2 bits in its bottom two positions — a register-pointer
 * read of 2 bytes starting at ADCS1 fetches both (the pointer
 * auto-increments). 10-bit result = (data[0] << 2) | (data[1] & 0x03).
 */
#define PMU_ADCS1           0x30
#define PMU_ADC_RESULT_LEN  2
#define PMU_ADC_LOW_MASK    0x03

/*
 * Raw(0..1023) -> millivolts. Full-scale reference is 6000 mV over the
 * 10-bit range; the divider is INSIDE the PMU (ADCVIN1), so this is a
 * direct scale with no external divider math: mV = raw * 6000 / 1024.
 * (>>10 instead of /1024 keeps it integer-only, no libm.)
 */
#define PMU_ADC_FULLSCALE_MV 6000
#define PMU_ADC_BITS         10

/*
 * CONVERSION WAIT. i2c_send deliberately does not wait for its own completion
 * (09-i2c.md: the write path lets the NEXT transaction's leading BUSY-wait
 * cover it), so the old code strobed "start conversion" and immediately turned
 * the bus around to read the result. Every reading was therefore at least one
 * sample stale, and the first after boot could be whatever the result
 * registers happened to hold.
 *
 * 06-power.md documents no conversion-ready bit anywhere in the PCF50605
 * register set, so there is nothing to poll — a fixed wait is the only option.
 * The doc's own cadence for this channel is "5 reads per 2 s", i.e. ~400 ms
 * between samples, which tells us the conversion is expected to be far shorter
 * than that but not how much shorter. 2 ms is comfortably above any plausible
 * 10-bit SAR conversion time and is invisible next to a 400 ms poll interval.
 *
 * DEVICE-GATED: this needs confirming on hardware (watch that two consecutive
 * reads after a sudden load change actually differ, rather than lagging).
 *
 * Implemented as a CPU-cycle loop scaled by the core clock, NOT a USEC_TIMER
 * wait: the golden trace for battery_millivolts asserts the exact ordered bus
 * transaction and nothing else, so an MMIO read in the middle of it would be a
 * trace change.
 */
#define PMU_ADC_SETTLE_US    2000u
#define PMU_SETTLE_CALIB_HZ  30000000u
/* Cycles a volatile spin trip costs on ARM7TDMI (load/sub/store/cmp/branch),
 * conservative-low so the wait errs long rather than short. */
#define PMU_SETTLE_CYCLES    8u

/* Current core frequency; weak so battery.c still links standalone in the host
 * trace test (which has no kernel clock driver). Absent -> assume calibration. */
__attribute__((weak)) uint32_t cpu_frequency(void);

static void pmu_adc_settle(void)
{
    uint32_t hz = cpu_frequency ? cpu_frequency() : PMU_SETTLE_CALIB_HZ;
    if (hz == 0u) {
        hz = PMU_SETTLE_CALIB_HZ;
    }
    /* trips = us * MHz / cycles-per-trip */
    volatile uint32_t trips =
        PMU_ADC_SETTLE_US * (hz / 1000000u) / PMU_SETTLE_CYCLES;
    while (trips-- != 0) {
        /* wait for the conversion */
    }
}

/*
 * Plausibility clamp. The cell is a single Li-Ion: below the 3300 mV shutoff
 * threshold the system is unstable, and above ~4200 mV is past a full charge —
 * a reading outside that band is a bad sample (a stale/garbage result register,
 * a bus glitch), not a battery state. Clamping keeps a bogus sample from
 * driving the percent curve to 0% and triggering a spurious low-battery
 * shutdown. Thresholds from 06-power.md, "Brown-out / low-battery shutdown"
 * (battery_level_shutoff = 3300) and the 100% curve point (4180, rounded up to
 * the cell's 4200 mV charge ceiling).
 */
#define PMU_MV_MIN  3300
#define PMU_MV_MAX  4200

/* ---------- Power-state GPIO bits (no I2C) -------------------------- */

/* GPIOL input: main charger present is ACTIVE-LOW (bit clear = present);
 * USB charger present is ACTIVE-HIGH (bit set = present). */
#define POWER_MAIN_CHARGER_BIT  0x08   /* GPIOL bit 3, active-low  */
#define POWER_USB_CHARGER_BIT   0x10   /* GPIOL bit 4, active-high */

/* GPIOB input: currently charging is ACTIVE-LOW (bit clear = charging). */
#define POWER_CHARGING_BIT      0x01   /* GPIOB bit 0, active-low  */

/* ---------- Voltage -> percent curve --------------------------------
 * 11-point piecewise-linear discharge curve for the iPod Video cell
 * (0%,10%,...,100%). APPROXIMATE / DEVICE-GATED — see the header. Kept as
 * a static const table + integer lerp so there is no libm dependency and
 * the whole thing lives in the freestanding image.
 */
#define BATTERY_CURVE_POINTS 11
#define BATTERY_CURVE_STEP   10   /* percent between adjacent points */

static const uint16_t battery_v_curve[BATTERY_CURVE_POINTS] = {
    3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180
};

/* Linear interpolation between the curve points, integer math with
 * round-to-nearest (+span/2 before the divide). Below the 0% point -> 0,
 * at/above the 100% point -> 100. */
static int battery_pct_from_mv(int mv)
{
    if (mv <= battery_v_curve[0]) {
        return 0;
    }
    for (int i = 1; i < BATTERY_CURVE_POINTS; i++) {
        if (mv < battery_v_curve[i]) {
            int span = battery_v_curve[i] - battery_v_curve[i - 1];
            int into = mv - battery_v_curve[i - 1];
            return (i - 1) * BATTERY_CURVE_STEP
                   + (into * BATTERY_CURVE_STEP + span / 2) / span;
        }
    }
    return 100;
}

/* ---------- Public API ---------------------------------------------- */

void battery_init(void)
{
    /* No-op: the shared I2C controller is initialized in the boot path
     * (i2c_init, needed for the codec too) and each read re-selects the
     * ADC channel. Present so a future settling/calibration step has a
     * home callers already invoke. */
}

int battery_millivolts(void)
{
    /* Select channel ADCVIN1 and start the conversion (ADCC1 = 0x05). */
    uint8_t select[2] = { PMU_ADCC1, PMU_ADC_START_VIN1 };
    if (i2c_send(PMU_ADDR, select, 2) != 0) {
        return -1;
    }

    /* Let the conversion actually happen before latching the result — see
     * PMU_ADC_SETTLE_US. There is no ready bit to poll (06-power.md). */
    pmu_adc_settle();

    /* Read the two result bytes back (ADCS1 then auto-incremented ADCS2). */
    uint8_t data[PMU_ADC_RESULT_LEN];
    if (i2c_read(PMU_ADDR, PMU_ADCS1, data, PMU_ADC_RESULT_LEN) != 0) {
        return -1;
    }

    /* Assemble the 10-bit sample and scale to millivolts. */
    int raw = ((int)data[0] << 2) | (data[1] & PMU_ADC_LOW_MASK);
    int mv  = (raw * PMU_ADC_FULLSCALE_MV) >> PMU_ADC_BITS;

    /* Sanity-clamp to the cell's real operating band (see PMU_MV_MIN/MAX). */
    if (mv < PMU_MV_MIN) {
        mv = PMU_MV_MIN;
    } else if (mv > PMU_MV_MAX) {
        mv = PMU_MV_MAX;
    }
    return mv;
}

int battery_percent(void)
{
    int mv = battery_millivolts();
    if (mv < 0) {
        return -1;
    }
    return battery_pct_from_mv(mv);
}

int power_is_external(void)
{
    uint32_t l = mmio_read32(GPIOL_INPUT_VAL_ADDR);
    return ((l & POWER_MAIN_CHARGER_BIT) == 0)   /* active-low  */
        || ((l & POWER_USB_CHARGER_BIT) != 0);   /* active-high */
}

int power_is_charging(void)
{
    uint32_t b = mmio_read32(GPIOB_INPUT_VAL_ADDR);
    return (b & POWER_CHARGING_BIT) == 0;         /* active-low */
}
