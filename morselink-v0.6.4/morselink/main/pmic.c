/*
 * MorseLink — power management chip driver (see pmic.h for the rails).
 *
 * Supports the AXP192 (T-Beam V1.0/V1.1) and the AXP2101 (T-Beam V1.2).
 * Both sit at I2C address 0x34 and both have their chip ID in register 0x03,
 * so pmic_init() can tell them apart before touching anything else.
 * All register access after init goes through s_lock (the UI and haptics
 * call in from different tasks).
 */
#include "pmic.h"

#include "board.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "pmic";

#define PMIC_ADDR 0x34   /* same for both chips */
#define REG_IC_TYPE 0x03 /* chip ID register, same for both chips */
#define AXP192_CHIP_ID 0x03
#define AXP2101_CHIP_ID 0x4A

/* --- AXP192 registers --- */
#define A192_POWER_STATUS 0x00
#define A192_CHARGE_STATUS 0x01
#define A192_OUTPUT_CTRL 0x12
#define A192_DCDC1_VOLT 0x26
#define A192_LDO23_VOLT 0x28
#define A192_SHUTDOWN 0x32
#define A192_VBAT_H 0x78
#define A192_ICHG_H 0x7A
#define A192_IDCHG_H 0x7C
#define A192_ADC_EN1 0x82

/* Bits of A192_OUTPUT_CTRL */
#define OUT_DCDC1 (1u << 0)
#define OUT_DCDC3 (1u << 1)
#define OUT_LDO2 (1u << 2)
#define OUT_LDO3 (1u << 3)

/* --- AXP2101 registers (names as in X-Powers' datasheet) --- */
#define A2101_STATUS1 0x00     /* bit5 VBUS good, bit3 battery present */
#define A2101_STATUS2 0x01     /* bits 6:5 = 01 while charging */
#define A2101_COMMON_CFG 0x10  /* bit0: soft power-off */
#define A2101_ADC_EN 0x30      /* bit0: battery-voltage ADC */
#define A2101_VBAT_H 0x34      /* battery mV, bits 12:8 (low 5 bits) */
#define A2101_VBAT_L 0x35      /* battery mV, bits 7:0 */
#define A2101_BAT_DET 0x68     /* bit0: battery detection on */
#define A2101_DCDC1_VOLT 0x82  /* bits 4:0: 1.5 V + 100 mV * n (to 3.4 V); bits 7:5 kept */
#define A2101_LDO_ONOFF0 0x90  /* bit1 ALDO2, bit2 ALDO3 */
#define A2101_ALDO2_VOLT 0x93  /* bits 4:0: 0.5 V + 100 mV * n */

#define A2101_ALDO2 (1u << 1)
#define A2101_ALDO3 (1u << 2)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;
static bool s_present;
static pmic_chip_t s_chip = PMIC_NONE;
static uint8_t s_out_ctrl; /* AXP192 only: last value written to A192_OUTPUT_CTRL */

/* rd(reg, buf, n) -- read n bytes starting at reg. 50 ms timeout. */
static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

/* wr(reg, val) -- write one register. */
static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 50);
}

/* init_axp192() -- V1.1 rails: motors (DCDC1) and radio (LDO2) at 3.3 V,
 * GPS (LDO3) off, battery ADCs on. Returns ESP_OK or ESP_FAIL. */
static esp_err_t init_axp192(void)
{
    esp_err_t err = ESP_OK;
    /* DCDC1 = 3.3 V: 0.7 V + 25 mV * 104 */
    err |= wr(A192_DCDC1_VOLT, 0x68);
    /* LDO2 = 3.3 V (upper nibble, 1.8 V + 100 mV * 15); keep LDO3's setting */
    uint8_t ldo = 0;
    err |= rd(A192_LDO23_VOLT, &ldo, 1);
    err |= wr(A192_LDO23_VOLT, (uint8_t)(0xF0 | (ldo & 0x0F)));
    /* Rails: DCDC1 on (motors), DCDC3 on (ESP32!), LDO2 on (LoRa), LDO3 off (GPS).
     * Leave DCDC2/EXTEN as the board configured them. */
    uint8_t out = 0;
    err |= rd(A192_OUTPUT_CTRL, &out, 1);
    out = (uint8_t)((out | OUT_DCDC1 | OUT_DCDC3 | OUT_LDO2) & ~OUT_LDO3);
    err |= wr(A192_OUTPUT_CTRL, out);
    s_out_ctrl = out;
    /* ADCs: battery voltage + current, VBUS voltage */
    uint8_t adc = 0;
    err |= rd(A192_ADC_EN1, &adc, 1);
    err |= wr(A192_ADC_EN1, (uint8_t)(adc | 0x80 | 0x40 | 0x08));
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AXP192 (T-Beam V1.1) ready: motors 3.3 V (DCDC1), LoRa 3.3 V (LDO2), GPS off");
    return ESP_OK;
}

/* init_axp2101() -- V1.2 rails: radio (ALDO2) at 3.3 V and on, GPS (ALDO3)
 * off, battery detection and battery-voltage ADC on. DCDC1 (ESP32 + motors)
 * is left exactly as it is. Returns ESP_OK or ESP_FAIL. */
static esp_err_t init_axp2101(void)
{
    esp_err_t err = ESP_OK;
    /* ALDO2 = 3.3 V: 0.5 V + 100 mV * 28. Keep the register's upper bits. */
    uint8_t v = 0;
    err |= rd(A2101_ALDO2_VOLT, &v, 1);
    err |= wr(A2101_ALDO2_VOLT, (uint8_t)((v & 0xE0) | 28));
    /* ALDO2 on (LoRa), ALDO3 off (GPS); the other LDOs untouched. */
    uint8_t on = 0;
    err |= rd(A2101_LDO_ONOFF0, &on, 1);
    err |= wr(A2101_LDO_ONOFF0, (uint8_t)((on | A2101_ALDO2) & ~A2101_ALDO3));
    /* Battery detection + battery-voltage ADC */
    uint8_t r = 0;
    err |= rd(A2101_BAT_DET, &r, 1);
    err |= wr(A2101_BAT_DET, (uint8_t)(r | 0x01));
    err |= rd(A2101_ADC_EN, &r, 1);
    err |= wr(A2101_ADC_EN, (uint8_t)(r | 0x01));
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    /* The radio's supply has just come up: give it a moment before
     * net_init() resets and probes it (the SX1276 wants ~10 ms after POR). */
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "AXP2101 (T-Beam V1.2) ready: LoRa 3.3 V (ALDO2), GPS off; "
                  "motors on the main 3.3 V (no rail gating on this board)");
    return ESP_OK;
}

/* pmic_init() -- see pmic.h. Opens the I2C bus (SDA 21, SCL 22, 400 kHz),
 * reads the chip ID and hands over to init_axp192() or init_axp2101(). */
esp_err_t pmic_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t id = 0;
    err = rd(REG_IC_TYPE, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no PMIC answering at 0x%02x (%s)", PMIC_ADDR, esp_err_to_name(err));
        return err;
    }
    if (id == AXP2101_CHIP_ID) {
        err = init_axp2101();
        s_chip = PMIC_AXP2101;
    } else {
        if (id != AXP192_CHIP_ID) {
            ESP_LOGW(TAG, "unexpected PMIC id 0x%02x, assuming AXP192", id);
        }
        err = init_axp192();
        s_chip = PMIC_AXP192;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMIC configuration failed");
        s_chip = PMIC_NONE;
        return ESP_FAIL;
    }
    s_present = true;
    return ESP_OK;
}

/* pmic_present() -- did pmic_init() succeed? */
bool pmic_present(void)
{
    return s_present;
}

/* pmic_chip() -- which chip pmic_init() found. */
pmic_chip_t pmic_chip(void)
{
    return s_chip;
}

/* pmic_motor_rail_is_on() -- see pmic.h. */
bool pmic_motor_rail_is_on(void)
{
    /* Only the AXP192 can switch the motor rail; elsewhere it's always on. */
    return s_chip != PMIC_AXP192 || (s_out_ctrl & OUT_DCDC1);
}

/* pmic_motor_rail() -- see pmic.h. AXP192 only: DCDC1 in the output-control
 * register, written only if it changes (haptics calls this often). */
esp_err_t pmic_motor_rail(bool on)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_chip == PMIC_AXP2101) {
        return ESP_OK; /* DCDC1 also feeds the ESP32: never switch it */
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t want = on ? (uint8_t)(s_out_ctrl | OUT_DCDC1) : (uint8_t)(s_out_ctrl & ~OUT_DCDC1);
    want |= OUT_DCDC3; /* belt and braces: never drop the ESP32's own rail */
    if (want != s_out_ctrl) {
        err = wr(A192_OUTPUT_CTRL, want);
        if (err == ESP_OK) {
            s_out_ctrl = want;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* pmic_radio_rail() -- see pmic.h. AXP192: LDO2 in the output-control
 * register (DCDC3, the ESP32's own rail, is kept on whatever happens).
 * AXP2101: ALDO2 in LDO on/off control 0; the other LDOs are untouched. */
esp_err_t pmic_radio_rail(bool on)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_chip == PMIC_AXP2101) {
        uint8_t r = 0;
        err = rd(A2101_LDO_ONOFF0, &r, 1);
        if (err == ESP_OK) {
            err = wr(A2101_LDO_ONOFF0, on ? (uint8_t)(r | A2101_ALDO2) : (uint8_t)(r & ~A2101_ALDO2));
        }
    } else {
        uint8_t want = on ? (uint8_t)(s_out_ctrl | OUT_LDO2) : (uint8_t)(s_out_ctrl & ~OUT_LDO2);
        want |= OUT_DCDC3;
        err = wr(A192_OUTPUT_CTRL, want);
        if (err == ESP_OK) {
            s_out_ctrl = want;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

/*
 * pmic_set_motor_mv() -- see pmic.h. The register is read first and only
 * the voltage bits change. AXP2101: DCDC1 (0x82, bits 4:0, 1.5 V + 0.1 V
 * steps); AXP192: DCDC1 (0x26, bits 6:0, 0.7 V + 25 mV steps). The rail
 * moves to the new voltage on its own (the chip ramps it); nothing has to
 * be switched off for it.
 */
esp_err_t pmic_set_motor_mv(int mv)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mv < PMIC_MOTOR_MV_MIN || mv > PMIC_MOTOR_MV_MAX) {
        return ESP_ERR_INVALID_ARG; /* outside what the ESP32 on the same rail (V1.2) can take */
    }
    esp_err_t err;
    uint8_t r = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_chip == PMIC_AXP2101) {
        err = rd(A2101_DCDC1_VOLT, &r, 1);
        if (err == ESP_OK) {
            err = wr(A2101_DCDC1_VOLT, (uint8_t)((r & 0xE0) | ((mv - 1500) / 100)));
        }
    } else {
        err = rd(A192_DCDC1_VOLT, &r, 1);
        if (err == ESP_OK) {
            err = wr(A192_DCDC1_VOLT, (uint8_t)((r & 0x80) | ((mv - 700) / 25)));
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* pmic_motor_mv() -- see pmic.h: DCDC1's setting read back from the chip,
 * in mV; 0 without a power chip or if the read fails. */
int pmic_motor_mv(void)
{
    if (!s_present) {
        return 0;
    }
    uint8_t r = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rd(s_chip == PMIC_AXP2101 ? A2101_DCDC1_VOLT : A192_DCDC1_VOLT, &r, 1);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return 0;
    }
    if (s_chip == PMIC_AXP2101) {
        return 1500 + (r & 0x1F) * 100;
    }
    return 700 + (r & 0x7F) * 25;
}

/* status_axp2101(st) -- battery voltage, USB, charging, battery present.
 * No current meter on this chip: both currents report 0. */
static esp_err_t status_axp2101(pmic_status_t *st)
{
    uint8_t s1 = 0, s2 = 0, vh = 0, vl = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rd(A2101_STATUS1, &s1, 1);
    err |= rd(A2101_STATUS2, &s2, 1);
    err |= rd(A2101_VBAT_H, &vh, 1);
    err |= rd(A2101_VBAT_L, &vl, 1);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    st->vbat_mv = (int)((((uint32_t)vh & 0x1F) << 8) | vl); /* 1 mV per step */
    st->charge_ma = 0;
    st->discharge_ma = 0;
    st->vbus = (s1 & (1u << 5)) != 0;
    st->battery = (s1 & (1u << 3)) != 0;
    st->charging = ((s2 >> 5) & 0x03) == 0x01;
    return ESP_OK;
}

/* pmic_read_status() -- see pmic.h. AXP192 path inline below (12-bit
 * voltage, 13-bit currents); AXP2101 in status_axp2101(). */
esp_err_t pmic_read_status(pmic_status_t *st)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_chip == PMIC_AXP2101) {
        return status_axp2101(st);
    }
    uint8_t ps = 0, cs = 0, v[2] = {0}, ic[2] = {0}, id[2] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rd(A192_POWER_STATUS, &ps, 1);
    err |= rd(A192_CHARGE_STATUS, &cs, 1);
    err |= rd(A192_VBAT_H, v, 2);
    err |= rd(A192_ICHG_H, ic, 2);
    err |= rd(A192_IDCHG_H, id, 2);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    /* 12-bit battery voltage, 1.1 mV per step */
    st->vbat_mv = (int)((((uint32_t)v[0] << 4) | (v[1] & 0x0F)) * 11 / 10);
    /* 13-bit currents, 0.5 mA per step */
    st->charge_ma = (int)((((uint32_t)ic[0] << 5) | (ic[1] & 0x1F)) / 2);
    st->discharge_ma = (int)((((uint32_t)id[0] << 5) | (id[1] & 0x1F)) / 2);
    st->vbus = (ps & (1u << 5)) != 0;
    st->charging = (cs & (1u << 6)) != 0;
    st->battery = (cs & (1u << 5)) != 0;
    return ESP_OK;
}

/* pmic_power_off() -- see pmic.h: sets the chip's own "power off" bit. */
esp_err_t pmic_power_off(void)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    /* AXP192: bit 7 of 0x32. AXP2101: bit 0 of 0x10. */
    uint8_t reg = (s_chip == PMIC_AXP2101) ? A2101_COMMON_CFG : A192_SHUTDOWN;
    uint8_t bit = (s_chip == PMIC_AXP2101) ? 0x01 : 0x80;
    uint8_t r = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rd(reg, &r, 1);
    if (err == ESP_OK) {
        err = wr(reg, (uint8_t)(r | bit));
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* battery_percent_from_mv() -- state of charge from the resting voltage,
 * by linear interpolation in a typical 18650 Li-ion curve (4.18 V = 100 %,
 * 3.30 V = 0 %). Reads high while charging and low under load. */
int battery_percent_from_mv(int mv)
{
    static const struct {
        int mv, pct;
    } t[] = {{4180, 100}, {4100, 92}, {4000, 81}, {3920, 70}, {3850, 59}, {3800, 48}, {3760, 38},
             {3730, 28},  {3700, 19}, {3670, 12}, {3620, 7},  {3550, 4},  {3450, 2},  {3300, 0}};
    const int n = sizeof(t) / sizeof(t[0]);
    if (mv >= t[0].mv) {
        return 100;
    }
    for (int i = 1; i < n; i++) {
        if (mv >= t[i].mv) {
            return t[i].pct + (mv - t[i].mv) * (t[i - 1].pct - t[i].pct) / (t[i - 1].mv - t[i].mv);
        }
    }
    return 0;
}
