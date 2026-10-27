/*
 * MorseLink — the power switch (see power.h for the behaviour).
 *
 * How "off" works on the ESP32: deep sleep, with the "ext0" wake-up source
 * on the key's pin. In deep sleep only the RTC domain runs; the key's pin is
 * one of the RTC-capable pins (GPIO38 for the test button, GPIO14 for the
 * external key since 20 Oct), so it can wake the chip. Waking from deep
 * sleep is a restart: app_main() runs from the top, and power_boot_check()
 * looks at why the chip started to decide what to do.
 *
 * Before sleeping:
 *   - the motor outputs are driven low and *held* there (gpio_hold_en), or
 *     they would float during the sleep and the transistors could leak
 *     current through the motors; app_main releases the hold at start-up;
 *   - the radio's supply rail is switched off at the PMIC (a radio left
 *     powered sits in standby at about 1 mA); on a V1.1 the motor rail
 *     (DCDC1) too;
 *   - the key's pull-up is set explicitly in the RTC domain (the external
 *     key has no resistor of its own: a floating pin could wake the chip, or
 *     never let it wake), and in the wearable build the LED's pin too (dark,
 *     no current through the LED);
 *   - we wait for the key to be released, or the level-triggered wake-up
 *     would fire at once.
 */
#include "power.h"

#include <stdio.h>

#include "board.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_rom_serial_output.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "haptics.h"
#include "pmic.h"
#include "sdkconfig.h"
#include "store.h"

static const char *TAG = "power";

/* key_down() -- is the key pressed right now? */
static bool key_down(void)
{
    return gpio_get_level(PIN_BUTTON) == BUTTON_ACTIVE_LEVEL;
}

/* key_input() -- make the key's pin a plain digital input. After an ext0
 * wake-up the pad still belongs to the RTC domain, so hand it back first.
 * Pull-up as the keyer sets it: GPIO34-39 have none (the board's own
 * resistor does the job on IO38); elsewhere (GPIO14) the internal one is the
 * key's only pull-up. */
static void key_input(void)
{
    if (rtc_gpio_is_valid_gpio(PIN_BUTTON)) {
        rtc_gpio_deinit(PIN_BUTTON);
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (PIN_BUTTON < 34) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

/* held_until_on() -- the key woke us: is it still held when the clock since
 * start-up reaches POWER_ON_HOLD_MS? (The time the chip took to start counts
 * as held, so the user feels about POWER_ON_HOLD_MS in all.) A release
 * shorter than POWER_KEY_UP_MS is contact bounce and doesn't count. */
static bool held_until_on(void)
{
    int64_t up_since = -1; /* when the key was last seen going up; -1 while it is down */
    while (esp_timer_get_time() < (int64_t)POWER_ON_HOLD_MS * 1000) {
        int64_t t = esp_timer_get_time();
        if (key_down()) {
            up_since = -1;
        } else if (up_since < 0) {
            up_since = t;
        } else if (t - up_since >= (int64_t)POWER_KEY_UP_MS * 1000) {
            return false; /* really let go: just a tap */
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

/* wait_release() -- wait until the key has been up for POWER_KEY_UP_MS, or
 * timeout_ms has passed (a stuck key mustn't hang the device). */
static void wait_release(int timeout_ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int64_t up_since = -1;
    while (esp_timer_get_time() < end) {
        int64_t t = esp_timer_get_time();
        if (key_down()) {
            up_since = -1;
        } else if (up_since < 0) {
            up_since = t;
        } else if (t - up_since >= (int64_t)POWER_KEY_UP_MS * 1000) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* buzz() -- one short buzz on the wrist motor (and the test LED): "you can
 * let go". The haptics player isn't running yet, so the pins are driven
 * directly; app_main's outputs_off_early() made them plain outputs. */
static void buzz(void)
{
    gpio_set_level(PIN_MOTOR_M1, 1);
#if USE_ONBOARD_LED
    gpio_set_level(PIN_ONBOARD_LED, 0); /* active-low: on */
#endif
    vTaskDelay(pdMS_TO_TICKS(POWER_ON_BUZZ_MS));
    gpio_set_level(PIN_MOTOR_M1, 0);
#if USE_ONBOARD_LED
    gpio_set_level(PIN_ONBOARD_LED, 1);
#endif
}

/* park_outputs() -- motors off and held off through the sleep. Both motor
 * pins are RTC-capable, which is what lets gpio_hold_en() keep them low in
 * deep sleep. haptics_running: the PWM player owns the pins (switching off
 * from the UI), so it parks them low itself; otherwise (the start-up check)
 * they are still the plain outputs app_main set up, so set them directly. */
static void park_outputs(bool haptics_running)
{
    if (haptics_running) {
        haptics_park();
    } else {
        gpio_set_level(PIN_MOTOR_M1, 0);
        gpio_set_level(PIN_MOTOR_M2, 0);
#if USE_ONBOARD_LED
        gpio_set_level(PIN_ONBOARD_LED, 1); /* active-low: dark */
#endif
    }
    gpio_hold_en(PIN_MOTOR_M1);
    gpio_hold_en(PIN_MOTOR_M2);
}

/*
 * sleep_pulls() -- the pulls that must hold through the deep sleep, set in
 * the RTC domain (which ext0 keeps powered: ESP-IDF switches its RTC_PERIPH
 * power domain on for this wake-up source):
 *   - the key: pulled up, never down. On the ESP32, gpio_config()'s pull-up
 *     on an RTC-capable pin already lands in the same RTC register (checked
 *     in ESP-IDF v6.1's gpio.c), so this only makes it explicit, in case
 *     something changed it since; the board's IO38 has no internal pull-up
 *     and doesn't need one (the board's own resistor);
 *   - the wearable build: the LED's pin (GPIO4) pulled up as app_main set
 *     it, so the LED stays dark and draws nothing while off (its reset
 *     state, a pull-down, would leak ~35 uA through the LED).
 * Draws nothing while the key is up.
 */
static void sleep_pulls(void)
{
    if (PIN_BUTTON < 34 && rtc_gpio_is_valid_gpio(PIN_BUTTON)) {
        rtc_gpio_pullup_en(PIN_BUTTON);
        rtc_gpio_pulldown_dis(PIN_BUTTON);
    }
#if !USE_ONBOARD_LED
    rtc_gpio_pullup_en(PIN_ONBOARD_LED);
    rtc_gpio_pulldown_dis(PIN_ONBOARD_LED);
#endif
}

/*
 * enter_off() -- everything off, then deep sleep until the key goes down.
 * haptics_running: the player has been started (stop it first) -- true when
 * switching off from the UI, false during the start-up check.
 */
static void enter_off(bool haptics_running) __attribute__((noreturn));
static void enter_off(bool haptics_running)
{
    park_outputs(haptics_running);
    if (pmic_present()) {
        pmic_radio_rail(false);
        pmic_motor_rail(false); /* V1.1: DCDC1 only feeds the motors. V1.2: does nothing */
    }
    sleep_pulls();
    wait_release(POWER_RELEASE_WAIT_MS);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, BUTTON_ACTIVE_LEVEL);
    ESP_LOGI(TAG, "off: hold the key %d s to switch on", POWER_ON_HOLD_MS / 1000);
    fflush(stdout);
    esp_rom_output_tx_wait_idle(CONFIG_ESP_CONSOLE_UART_NUM); /* let the log line out before the UART stops */
    esp_deep_sleep_start();
}

/* power_boot_check() -- see power.h: run, or go back to sleep. */
void power_boot_check(void)
{
    bool key_wake = (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_EXT0)) != 0;
    key_input();
    if (key_wake) {
        if (!held_until_on()) {
            ESP_LOGI(TAG, "the key was only tapped: staying off");
            enter_off(false);
        }
        buzz();
        wait_release(POWER_RELEASE_WAIT_MS);
        store_save_power_off(false);
        ESP_LOGI(TAG, "switched on");
        return;
    }
    if (store_load_power_off()) {
        ESP_LOGW(TAG, "switched off (F), so staying off although power arrived");
        enter_off(false);
    }
}

/* power_off() -- see power.h: remember "off", then everything off and sleep. */
void power_off(void)
{
    ESP_LOGW(TAG, "switching off");
    store_save_power_off(true);
    enter_off(true);
}
