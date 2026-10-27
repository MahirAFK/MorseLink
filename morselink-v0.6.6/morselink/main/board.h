/*
 * MorseLink — pin map for the LilyGO T-Beam V1.1 (AXP192) and V1.2 (AXP2101),
 * SX1276 or SX1262 radio, 868 MHz. Both versions use the same pins; pmic.c handles the
 * different power chips.
 *
 * Plain integers so the checks at the bottom work in the preprocessor.
 */
#pragma once

/* ---- build switches ---------------------------------------------------------
 *
 *   USE_ONBOARD_BUTTON 1  The key is the T-Beam's own IO38 button (the middle
 *                         one of the three; RST and PWR are the others) instead
 *                         of the external key on GPIO14.
 *   USE_ONBOARD_LED    1  Everything the motors play is also mirrored on the
 *                         board's LED (GPIO4):
 *                           dot          bright flash
 *                           dash         bright, then dim (the dim part is
 *                                        where the forearm motor takes over)
 *                           system tones bright
 *                         The LED draws a few mA while lit, so it adds to every
 *                         current measured: keep it 0 for the meter session
 *                         (bench power).
 *   VERBOSE_LOG        1  Log every packet sent and received, everything played
 *                         and the keyed timings (the console's `radio log` and
 *                         `verbose` can still switch each off). Until v0.6.0 this
 *                         came with USE_ONBOARD_LED; since the key moved off the
 *                         LED's pin (20 Oct) the two are separate.
 *
 * v0.6.1 (agreed 20 Oct): the default is the wearable build, the external key
 * on GPIO14 and the LED off, so the bench tests run on the real motors and
 * key. The motor outputs (GPIO25/13) are driven in every build. To go back to
 * the board's own button (no external key wired), set USE_ONBOARD_BUTTON to 1;
 * the LED switch is independent of it.
 */
#define USE_ONBOARD_BUTTON 0
#define USE_ONBOARD_LED 0
#define VERBOSE_LOG 1

/* ---- project wiring ----
 * Motors (confirmed 3 Oct; driver as built 18 Oct): each pin drives an NPN
 * transistor (2N2222) through a 330 ohm base resistor (1 k was too weak for
 * the motor's ~120 mA start-up), the motor from 3.3 V with a diode across it.
 * Key (agreed 20 Oct): moved from GPIO4 to GPIO14, a push button from the pin
 * to GND, nothing else. GPIO4 is the board LED's pin (the LED lit on every
 * press and cost ~2 mA while the key was down); GPIO14 has nothing else on
 * it, isn't read by the chip at start-up (unlike GPIO0/2/12/15), can wake the
 * chip from deep sleep (the power switch) and has an internal pull-up
 * (~45 kohm), which is the key's only pull-up: no current flows while the key
 * is up. If stray dots ever show up while the motors run (the wire picking
 * up noise), a 10 kohm resistor from GPIO14 to 3.3 V makes it stiffer.
 */
#define PIN_MOTOR_M1 25   /* wrist motor, NPN base via 330 ohm */
#define PIN_MOTOR_M2 13   /* forearm motor (~9 cm proximal), NPN base via 330 ohm */
#define PIN_EXT_BUTTON 14 /* key: to GND when pressed; the ESP32's internal pull-up */
#define BUTTON_ACTIVE_LEVEL 0

/* ---- fixed by the board ---- */
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define PIN_PMU_IRQ 35

#define PIN_LORA_SCK 5
#define PIN_LORA_MISO 19
#define PIN_LORA_MOSI 27
#define PIN_LORA_CS 18
#define PIN_LORA_RST 23
/* The T-Beam comes with either radio chip on the same SPI pins. The other
 * radio pins mean different things on each: */
#define PIN_LORA_DIO0 26        /* SX1276: interrupt (TxDone / RxDone) */
#define PIN_LORA_DIO1 33        /* SX1276: DIO1 (unused) */
#define PIN_LORA_DIO2 32        /* SX1276: DIO2 (unused) */
#define PIN_LORA_SX1262_DIO1 33 /* SX1262: interrupt (TxDone / RxDone) */
#define PIN_LORA_BUSY 32        /* SX1262: BUSY, high while the chip works on a command */

#define PIN_GPS_RX 34         /* unused: GPS rail is switched off */
#define PIN_GPS_TX 12
#define PIN_ONBOARD_BUTTON 38 /* input-only; the board has its own pull-up */
#define PIN_ONBOARD_LED 4     /* active-low: the LED runs from 3.3 V to this pin */

#if USE_ONBOARD_BUTTON
#define PIN_BUTTON PIN_ONBOARD_BUTTON
#else
#define PIN_BUTTON PIN_EXT_BUTTON
#endif

/* ---- checks ---- */
#if USE_ONBOARD_LED && (PIN_BUTTON == PIN_ONBOARD_LED)
#error "The board LED and the key share GPIO4: set USE_ONBOARD_LED to 0 (or move the key)."
#endif
#if (PIN_MOTOR_M1 == PIN_LORA_DIO2) || (PIN_MOTOR_M2 == PIN_LORA_DIO2) || (PIN_BUTTON == PIN_LORA_DIO2)
#error "GPIO32 is the radio's DIO2 (SX1276) or BUSY (SX1262) on the T-Beam; move that wire."
#endif
#if (PIN_MOTOR_M1 == PIN_LORA_DIO1) || (PIN_MOTOR_M2 == PIN_LORA_DIO1) || (PIN_BUTTON == PIN_LORA_DIO1)
#error "GPIO33 is the radio's DIO1 on the T-Beam; move that wire."
#endif
#if (PIN_MOTOR_M1 >= 34) || (PIN_MOTOR_M2 >= 34)
#error "GPIO34-39 are input-only and cannot drive a motor."
#endif
#if (PIN_MOTOR_M1 == PIN_MOTOR_M2) || (PIN_MOTOR_M1 == PIN_BUTTON) || (PIN_MOTOR_M2 == PIN_BUTTON)
#error "M1, M2 and the key need three different pins."
#endif
#if USE_ONBOARD_LED && ((PIN_MOTOR_M1 == PIN_ONBOARD_LED) || (PIN_MOTOR_M2 == PIN_ONBOARD_LED))
#error "A motor is on GPIO4, the LED's pin: set USE_ONBOARD_LED to 0."
#endif
/* The ESP32 reads GPIO0, 2, 12 and 15 at reset to choose how it boots: a key
 * held (or a pull-up) there at the wrong moment stops the board starting or
 * being flashed. */
#if (PIN_BUTTON == 0) || (PIN_BUTTON == 2) || (PIN_BUTTON == 12) || (PIN_BUTTON == 15)
#error "The key is on a strapping pin (GPIO0/2/12/15): pick another (GPIO14 is the project's)."
#endif
/* The power switch wakes the chip from deep sleep on the key (ext0), which
 * only the RTC-capable pins can do. */
#if !((PIN_BUTTON == 4) || ((PIN_BUTTON >= 12) && (PIN_BUTTON <= 15)) || ((PIN_BUTTON >= 25) && (PIN_BUTTON <= 27)) || \
      ((PIN_BUTTON >= 32) && (PIN_BUTTON <= 39)))
#error "The key must be on an RTC-capable pin (4, 12-15, 25-27, 32-39): the power switch wakes the chip on it."
#endif
