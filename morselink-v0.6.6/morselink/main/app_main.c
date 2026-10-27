/*
 * MorseLink — wearable Morse-code communicator.
 * LilyGO T-Beam V1.1 (AXP192) or V1.2 (AXP2101): ESP32 + SX1276 or SX1262 868 MHz, ESP-IDF v6.1.
 *
 * Build steps covered so far (spec §14): board bring-up, haptic output
 * engine, key input + decoder + adaptive calibration, command mode, the
 * LoRa radio with linking, pairing, delivery receipts and SOS over the air,
 * discovery (D), the BLE link to the owner's phone (O; ble.h; the phone app
 * is a separate project), test mode and the bench tests.
 *
 * Start-up order matters:
 *   1. motor outputs off, before anything else can glitch them on (and the
 *      hold that kept them off through a deep sleep released); the board
 *      LED's pin set so the LED is dark and draws nothing;
 *   2. NVS (settings, contacts, calibration, ID, the power switch);
 *   3. the PMIC, which powers the radio (LDO2 on V1.1, ALDO2 on V1.2) and,
 *      on V1.1, the motors (DCDC1);
 *   4. the power switch (power.h): if the device is switched off, or the key
 *      only tapped it, it goes back to sleep here and nothing below runs;
 *   5. haptics, then the network task (it finds and configures the radio),
 *      then the BLE side's line queue (BLE itself stays off), then the UI
 *      task (it tells the network our ID and link, and starts BLE if a
 *      phone is paired), then the key, whose presses go to the UI;
 *   6. the serial console.
 */
#include "ble.h"
#include "board.h"
#include "config.h"
#include "console.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "haptics.h"
#include "keyer.h"
#include "net.h"
#include "pmic.h"
#include "power.h"
#include "store.h"
#include "ui.h"

static const char *TAG = "main";

/* outputs_off_early() -- hold both motor transistors off (and the board
 * LED dark) from the first moment, before the drivers take over the pins. */
static void outputs_off_early(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_MOTOR_M1) | (1ULL << PIN_MOTOR_M2),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_MOTOR_M1, 0);
    gpio_set_level(PIN_MOTOR_M2, 0);
    /* Switching off (power.c) holds these pins low through the deep sleep.
     * The level is already 0, so releasing the hold can't make a glitch. */
    gpio_hold_dis(PIN_MOTOR_M1);
    gpio_hold_dis(PIN_MOTOR_M2);
#if USE_ONBOARD_LED
    /* Board LED off (active-low, open-drain: let the pin go). */
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << PIN_ONBOARD_LED,
        .mode = GPIO_MODE_OUTPUT_OD,
    };
    gpio_config(&led);
    gpio_set_level(PIN_ONBOARD_LED, 1);
#else
    /* The LED isn't used (the wearable build), but it is still there, from
     * 3.3 V to GPIO4. Left alone the pin keeps its reset state, which on
     * this pin includes a weak pull-down: a faint glow and ~35 uA through
     * the LED all the time, switched off included. An input with the
     * pull-up keeps both ends of the LED at 3.3 V: dark, no current. The
     * pull-up lives in the RTC domain, so it stays on in deep sleep too
     * (power.c sets it again before sleeping, to be sure). */
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << PIN_ONBOARD_LED,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&led);
#endif
}

/* app_main() -- the ESP-IDF entry point: bring everything up in order
 * (see the top of this file) and return; the tasks carry on. */
void app_main(void)
{
    outputs_off_early();
    ESP_LOGI(TAG, "MorseLink %s starting (M1=GPIO%d, M2=GPIO%d, key=GPIO%d%s)", MORSELINK_VERSION, PIN_MOTOR_M1,
             PIN_MOTOR_M2, PIN_BUTTON, PIN_BUTTON < 34 ? ", internal pull-up" : "");
#if USE_ONBOARD_BUTTON || USE_ONBOARD_LED
    ESP_LOGW(TAG, "TEST SETUP (board.h):%s%s", USE_ONBOARD_BUTTON ? " key = the board's middle button (IO38)." : "",
             USE_ONBOARD_LED ? " Output mirrored on the board LED." : "");
#endif

    /* One GPIO interrupt service for the key and the radio's DIO0, installed
     * here, before either task starts, so they never race to install it
     * (both then simply add their handler). IRAM: interrupts keep working
     * while flash is being written. */
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));

    ESP_ERROR_CHECK(store_init());

    esp_err_t err = pmic_init();
    if (err != ESP_OK) {
        /* Keep going: on USB power the board still runs, and the motors and
         * radio may already be powered by the PMIC's defaults. */
        ESP_LOGE(TAG, "PMIC init failed (%s): power rails left as they are", esp_err_to_name(err));
    }

    /* Switched off, or the key only tapped? Then this goes back to sleep. */
    power_boot_check();

    ESP_ERROR_CHECK(haptics_init());
    net_init();
    ble_init();
    ui_start();
    ESP_ERROR_CHECK(keyer_init(ui_post_key));

    err = console_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console failed to start: %s", esp_err_to_name(err));
    }
    /* How much RAM is left with everything running (BLE, when it starts,
     * takes some more): worth watching as features are added. */
    ESP_LOGI(TAG, "running: %u bytes of RAM free (largest block %u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
