/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/hal/hw/power.h — device power state (deep-sleep standby).
 *
 * "Off" on the iPod 5.5G is a PCF50605 PMU deep-sleep, not a true power
 * cut: the PMU's always-on domain stays alive to watch the wake sources
 * (a button press, or charger insertion) while everything else loses
 * power. On wake the SoC re-runs the whole boot path — so "turning it
 * back on" is a cold boot of our firmware — straight from the boot ROM,
 * with no chainloader in the path. See core/docs/hw/06-power.md.
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

/*
 * Reboot the SoC. Masks both cores' interrupts, then sets DEV_SYSTEM in
 * DEV_RS and spins — control returns to the Apple boot ROM. Never returns.
 *
 * (docs/hw/01-soc-pp5022.md, "Power management"; bit verified against
 * Rockbox pp5020.h DEV_SYSTEM + its PP502x system_reboot.)
 */
_Noreturn void power_reboot(void);

/*
 * Reboot into the Apple boot ROM's DISK MODE (USB mass storage).
 *
 * Writes the ROM's magic token to the known IRAM location and reboots; the
 * ROM recognises it and comes up in disk mode instead of loading the OS
 * image (docs/hw/08-boot-dock.md, "Disk Mode (Apple's fallback)").
 *
 * This is how the user gets the device onto a host to be flashed or to have
 * music copied on. Our firmware implements no USB at all, so without this
 * the ONLY route in is the ROM's Select+Play key combo at boot — which is
 * timing-sensitive and easy to miss. Never returns.
 */
_Noreturn void power_enter_disk_mode(void);

#endif /* CORE_HAL_HW_POWER_H */
