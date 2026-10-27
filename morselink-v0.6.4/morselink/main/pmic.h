/*
 * MorseLink — power management chip (PMIC) on the T-Beam.
 *
 * Two board versions exist, with different PMICs at the same I2C address
 * (0x34). pmic_init() reads the chip ID and drives whichever it finds.
 *
 * T-Beam V1.0 / V1.1 — AXP192:
 *   DCDC1  -> OLED header pads; our motor supply (3.3 V), switched off between
 *             vibrations when rail gating is enabled
 *   DCDC3  -> ESP32 (never touched)
 *   LDO2   -> SX1276 LoRa radio (3.3 V, on)
 *   LDO3   -> NEO-6M GPS (off: unused, saves ~40 mA)
 *
 * T-Beam V1.2 — AXP2101:
 *   DCDC1  -> the board's main 3.3 V: ESP32 *and* the header pads the motors
 *             use. It must never be switched off, so on this board the motor
 *             rail is always on and rail gating does nothing.
 *   ALDO2  -> LoRa radio (SX1276 or SX1262) (3.3 V, on). Off at power-up: without this
 *             the radio has no power and can't be found.
 *   ALDO3  -> GPS (off: unused)
 *   The AXP2101 has no battery-current meter, so charge_ma / discharge_ma
 *   read 0 there.
 *
 * If an OLED is ever fitted to the header pads of a V1.1, don't gate DCDC1:
 * an unpowered OLED can hold the I2C bus down and lock out the PMIC.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Which PMIC pmic_init() found. */
typedef enum {
    PMIC_NONE = 0, /* none found (or init failed) */
    PMIC_AXP192,   /* T-Beam V1.0 / V1.1 */
    PMIC_AXP2101,  /* T-Beam V1.2 */
} pmic_chip_t;

typedef struct {
    int vbat_mv;      /* battery voltage */
    int charge_ma;    /* into the battery (0 on the AXP2101: not measured) */
    int discharge_ma; /* out of the battery (0 on the AXP2101: not measured) */
    bool vbus;        /* USB power present */
    bool charging;
    bool battery; /* battery connected */
} pmic_status_t;

/* pmic_init() -- open I2C, identify the chip, switch the radio rail on and
 * the GPS rail off, enable the battery ADC. Must run before net_init(): the
 * radio has no power until this has run. Returns ESP_OK on success. */
esp_err_t pmic_init(void);

/* pmic_present() -- true once pmic_init() succeeded. */
bool pmic_present(void);

/* pmic_chip() -- which chip was found (PMIC_NONE before/without init). */
pmic_chip_t pmic_chip(void);

/* pmic_motor_rail(on) -- switch the motor supply. AXP192: DCDC1, cheap if
 * unchanged. AXP2101: the motors share the ESP32's rail, so this does nothing
 * and returns ESP_OK. ESP_ERR_INVALID_STATE without a PMIC. */
esp_err_t pmic_motor_rail(bool on);

/* pmic_motor_rail_is_on() -- whether the motors have power (true without a
 * PMIC, or on the AXP2101, whose motor rail is always on). */
bool pmic_motor_rail_is_on(void);

/* The motor supply's voltage (vibration strength, command I, v0.6.2).
 * On the V1.2 the motors share DCDC1 with the ESP32, whose flash and PSRAM
 * need 3.0-3.6 V, and the AXP2101's DCDC1 goes up to 3.4 V: hence the
 * limits (kept the same on the AXP192 for a like-for-like range). */
#define PMIC_MOTOR_MV_MIN 3000
#define PMIC_MOTOR_MV_MAX 3400

/* pmic_set_motor_mv(mv) -- set DCDC1 to mv (3000-3400, a multiple of 100).
 * ESP_ERR_INVALID_ARG outside that range, ESP_ERR_INVALID_STATE without a
 * PMIC. Not stored: the caller applies the stored level at start-up. */
esp_err_t pmic_set_motor_mv(int mv);

/* pmic_motor_mv() -- DCDC1's voltage as the chip has it now, in mV (0
 * without a PMIC). */
int pmic_motor_mv(void);

/* pmic_radio_rail(on) -- switch the LoRa radio's supply (AXP192 LDO2,
 * AXP2101 ALDO2). pmic_init() switches it on; power.c switches it off
 * while the device is switched off. ESP_ERR_INVALID_STATE without a PMIC. */
esp_err_t pmic_radio_rail(bool on);

/* pmic_read_status(st) -- fill *st with battery / USB state. */
esp_err_t pmic_read_status(pmic_status_t *st);

/* pmic_power_off() -- whole board off (PMIC shutdown). */
esp_err_t pmic_power_off(void);

/* Battery state of charge from resting voltage (pure, also used in tests). */
int battery_percent_from_mv(int mv);
