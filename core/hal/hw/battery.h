/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/battery.h — battery gauge + charge/power-state contract.
 *
 * The iPod 5G/5.5G has NO SoC ADC for the cell: battery voltage comes
 * from the PCF50605 PMU's internal 10-bit ADC, read over the same on-SoC
 * I2C control bus the WM8758 codec uses (7-bit device address 0x08). See
 * battery.c and core/docs/hw/06-power.md for the mechanism and the
 * cleanroom facts it was built from.
 *
 * Charge/power-state (external supply present, actively charging) is
 * NOT on the PMU/I2C path — it is three plain GPIO input bits, so the
 * power-state calls are cheap and never touch the (slower, harder) I2C
 * read. A UI can poll power_is_external()/power_is_charging() every frame.
 *
 * DEVICE-GATED CALIBRATION: battery_millivolts()'s scaling and the
 * percent curve are transcribed from the 2005 Apple cell and CANNOT be
 * validated off-hardware. The safe first on-device step is to display
 * raw millivolts and sanity-check the ~3300..4200 mV range before
 * trusting battery_percent() or any shutdown threshold. Treat percent as
 * cosmetic until calibrated against the real (often replacement) cell.
 */

#ifndef CORE_HAL_HW_BATTERY_H
#define CORE_HAL_HW_BATTERY_H

/* One-time setup. Currently a no-op: the shared I2C controller is brought
 * up by i2c_init() in the boot path (the codec needs it too), and each
 * read re-selects the ADC channel, so there is no per-gauge state to
 * prime. Kept in the API so a future settling/calibration step has a home
 * that callers already invoke. */
void battery_init(void);

/*
 * Battery terminal voltage in millivolts via the PCF50605 ADC
 * (select channel ADCVIN1, read the 10-bit result, scale x6000>>10).
 * Returns -1 if the I2C transaction fails. DEVICE-GATED scaling.
 */
int battery_millivolts(void);

/*
 * State-of-charge 0..100 from the piecewise-linear voltage->percent
 * curve (integer lerp between the 11 calibration points). Returns -1 if
 * the underlying voltage read fails. APPROXIMATE / DEVICE-GATED — the
 * low end (0..20%) is the steep, cell-dependent region; do not drive
 * shutdown off percent, use a raw-mV threshold.
 */
int battery_percent(void);

/*
 * External power present: main charger (dock/FireWire/USB power) OR a
 * USB charger is attached. Pure GPIO read, no I2C. Returns 1/0.
 */
int power_is_external(void);

/*
 * The charger is actively charging the cell right now. Pure GPIO read,
 * no I2C. Returns 1/0. "Full while plugged" is approximately
 * (power_is_external() && !power_is_charging()).
 */
int power_is_charging(void);

/*
 * Tell the LTC4066 how much input current it may draw (06-power.md, "Charge
 * current control"). 500 => HPWR asserted, the charger may pull up to 500 mA;
 * anything less => the 100 mA default cap. The charger is otherwise autonomous
 * (CV/CC, fast/trickle, end-of-charge are all internal) — this is the only
 * charge knob the firmware has.
 *
 * Deliberately never raises SUSP: a latched SUSP high is "the iPod silently
 * refuses to charge", and we have no feature that needs zero-current mode.
 */
void charger_set_max_current(int milliamps);

#endif /* CORE_HAL_HW_BATTERY_H */
