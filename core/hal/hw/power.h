/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/power.h — device power state (deep-sleep standby).
 *
 * "Off" on the iPod 5.5G is a PCF50605 PMU deep-sleep, not a true power
 * cut: the PMU's always-on domain stays alive to watch the wake sources
 * (a button press, or charger insertion) while everything else loses
 * power. On wake the SoC re-runs the whole boot path — so "turning it
 * back on" is a cold boot of our firmware (via the chainloader), not a
 * resume. See core/docs/hw/06-power.md.
 */
#ifndef CORE_HAL_HW_POWER_H
#define CORE_HAL_HW_POWER_H

/*
 * Enter PMU deep-sleep standby. Wakes on a face-button press (EXTONWAK) or
 * charger insertion (CHGWAK) — a wake source is ALWAYS set, since standby
 * without one can never be powered on again (06-power.md, ipodlinux
 * warning). Callers should quiesce first (stop playback, blank the panel).
 *
 * Also performs the pre-sleep IRAM clear at 0x4000C000 that stops Apple's OF
 * taking the boot-from-sleep path on the next power-on (06-power.md,
 * "Pre-sleep housekeeping").
 *
 * NORMALLY DOES NOT RETURN: the PMU cuts SoC power. It returns only when
 * standby could not be entered at all — a wedged I2C control bus that did not
 * accept the command after a bounded number of retries. In that case it
 * returns -1 and has relit the backlight so the failure is visible, and the
 * caller should carry on rather than assume it is powering down. (Previously
 * this case was an unkillable for(;;) on a dark screen.)
 */
int power_standby(void);

#endif /* CORE_HAL_HW_POWER_H */
