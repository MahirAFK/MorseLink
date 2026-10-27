/*
 * MorseLink — the power switch (agreed 8 Oct).
 *
 * The device has an on/off switch kept in firmware and remembered in flash:
 *
 *   Switch off:  command mode, F, hold 2 s. The echo plays OFF; a press
 *                during it aborts. Then the radio's supply is cut, the motors
 *                are parked off, and the ESP32 goes into deep sleep, watching
 *                only the key. Refused while our own SOS is active.
 *   Switch on:   hold the key for 2 s. One short buzz says "let go"; the
 *                device then starts and plays OK when ready. A shorter press
 *                does nothing: it goes straight back to sleep.
 *   Power comes: USB plugged in, a battery fitted, the reset button, a crash:
 *                the device starts only if the switch is on. Switched off, it
 *                goes straight back to sleep (and charges, on USB).
 *
 * "Off" is a deep sleep rather than a true power cut, because something has
 * to watch the key and only the ESP32 can. It draws very little, but not
 * nothing: for storage over months, use the PWR button or take the battery
 * out.
 *
 * The board's PWR button keeps its hardware behaviour (hold it: everything
 * loses power). That doesn't move the switch, so when power returns the
 * device comes back on if it was on.
 */
#pragma once

#include <stdbool.h>

/*
 * power_boot_check() -- call once, early in app_main(): after store_init()
 * and pmic_init(), before anything that vibrates, transmits or reads the key.
 *   - Woken by the key while switched off: switch on if the key stays down
 *     until POWER_ON_HOLD_MS after start-up (buzz, then wait for the
 *     release, so the press isn't seen as keying); otherwise sleep again.
 *   - Any other start: sleep again if switched off, carry on if on.
 * Returns only if the device is to run.
 */
void power_boot_check(void);

/*
 * power_off() -- switch off now: remember it, stop and park the motors, cut
 * the radio's supply, then sleep until the key is held. Never returns.
 * Called by the UI task (F, or `off` in the console); the UI has already
 * checked it's allowed and saved what needs saving.
 */
void power_off(void) __attribute__((noreturn));
