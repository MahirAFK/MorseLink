/*
 * MorseLink — the key (see keyer.h).
 *
 * How a press becomes events:
 *   1. key_isr() runs on every edge of the key's pin and only queues the
 *      edge's timestamp (microseconds since boot).
 *   2. keyer_task() waits for edges. After one, it waits until the line has
 *      been quiet for KEY_DEBOUNCE_MS (contact bounce), then reads the level;
 *      a real change becomes KEY_DOWN or KEY_UP, stamped with the *first*
 *      edge of the burst, so bounce never shortens or lengthens an element.
 *   3. While the key is down, the task also wakes at each hold threshold
 *      (1.5 / 2 / 3 / 5 s) and reports KEY_HOLD with the level reached, so
 *      the UI can buzz the feedback while the user is still holding.
 * Everything goes to the callback given to keyer_init() (the UI's queue).
 */
#include "keyer.h"

#include "board.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "keyer";

static QueueHandle_t s_edges; /* edge timestamps, from the interrupt to the task */
static keyer_cb_t s_cb;        /* where key events go (the UI) */
static volatile bool s_down;   /* debounced state, for keyer_is_down() */

/* The hold thresholds, in the order they are passed (KEY_HOLD level 1-4). */
static const int64_t s_hold_ms[] = {HOLD_NOT_MORSE_MS, HOLD_SEND_MS, HOLD_CMD_MS, HOLD_CANCEL_MS};

/* key_isr() -- the key's pin changed (either direction). Interrupt context,
 * in IRAM: only timestamps the edge and hands it to keyer_task(). A full
 * queue (a very long bounce burst) just loses edges, which the debounce
 * logic tolerates because it reads the level at the end anyway. */
static void IRAM_ATTR key_isr(void *arg)
{
    (void)arg;
    int64_t t = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_edges, &t, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

/* level_pressed() -- is the key pressed right now (raw pin level)? */
static bool level_pressed(void)
{
    return gpio_get_level(PIN_BUTTON) == BUTTON_ACTIVE_LEVEL;
}

/*
 * keyer_task() -- turns edges into KEY_DOWN / KEY_UP / KEY_HOLD events (see
 * the top of this file). A key already held when the task starts (e.g. the
 * press that switched the device on, if it lasted past power.c's wait) is
 * ignored until it has been released once.
 */
static void keyer_task(void *arg)
{
    (void)arg;
    bool pressed = level_pressed();
    int64_t down_us = 0;
    int holds = 0; /* thresholds already reported for this press */
    if (pressed) {
        /* Held at boot: don't report it, wait for a clean release. */
        ESP_LOGW(TAG, "key down at boot; ignoring until released");
        holds = 4;
    }

    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (pressed && holds < 4) {
            int64_t due = down_us + s_hold_ms[holds] * 1000;
            int64_t ms = (due - esp_timer_get_time() + 999) / 1000;
            wait = ms > 0 ? pdMS_TO_TICKS(ms) : 0;
        }
        int64_t t_edge;
        if (xQueueReceive(s_edges, &t_edge, wait) == pdTRUE) {
            /* Let the contact settle: wait for a quiet spell, then trust the
             * level, timestamped by the first edge of the burst. */
            int64_t extra;
            while (xQueueReceive(s_edges, &extra, pdMS_TO_TICKS(KEY_DEBOUNCE_MS)) == pdTRUE) {
            }
            bool now = level_pressed();
            if (now == pressed) {
                continue; /* glitch or bounce that came back */
            }
            pressed = now;
            s_down = now;
            key_evt_t e = {.t_us = t_edge};
            if (pressed) {
                down_us = t_edge;
                holds = 0;
                e.kind = KEY_DOWN;
                e.down_us = t_edge;
            } else {
                if (holds == 4 && down_us == 0) {
                    holds = 0; /* the boot-time press ended */
                    continue;
                }
                e.kind = KEY_UP;
                e.down_us = down_us;
            }
            if (s_cb) {
                s_cb(&e);
            }
        } else if (pressed && holds < 4) {
            int64_t now_us = esp_timer_get_time();
            while (holds < 4 && now_us >= down_us + s_hold_ms[holds] * 1000) {
                key_evt_t e = {.kind = KEY_HOLD, .t_us = down_us + s_hold_ms[holds] * 1000, .down_us = down_us,
                               .level = (uint8_t)(holds + 1)};
                holds++;
                if (s_cb) {
                    s_cb(&e);
                }
            }
        }
    }
}

/* keyer_init() -- set up the key's pin (input, interrupt on both edges),
 * attach key_isr(), and start keyer_task() on core 1. `cb` receives every
 * key event, from the keyer task. Returns the first GPIO error, if any. */
esp_err_t keyer_init(keyer_cb_t cb)
{
    s_cb = cb;
    s_edges = xQueueCreate(32, sizeof(int64_t));
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        /* GPIO34-39 have no internal pull-ups (the board's IO38 button has
         * its own resistor); on GPIO14, the external key's pin, the internal
         * ~45 kohm one is the key's only pull-up (agreed 20 Oct: no resistor
         * on the key). */
        .pull_up_en = (PIN_BUTTON < 34) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    /* The GPIO interrupt service itself is installed once, early, by
     * app_main(); installing it again here only printed an error line. */
    err = gpio_isr_handler_add(PIN_BUTTON, key_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }
    s_down = level_pressed();
    xTaskCreatePinnedToCore(keyer_task, "keyer", 3072, NULL, 12, NULL, 1);
    ESP_LOGI(TAG, "key on GPIO%d (active %s)", PIN_BUTTON, BUTTON_ACTIVE_LEVEL ? "high" : "low");
    return ESP_OK;
}

/* keyer_is_down() -- the debounced key state (any task may ask). */
bool keyer_is_down(void)
{
    return s_down;
}
