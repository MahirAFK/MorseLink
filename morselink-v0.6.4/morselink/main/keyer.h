/*
 * MorseLink — the key (button).
 *
 * Edges are timestamped in the ISR (esp_timer, microseconds) so element
 * timing doesn't depend on task scheduling. A task then debounces by
 * re-reading the level once the line has been quiet for a few ms, instead of
 * assuming edges alternate (GPIO36/39 can glitch for ~80 ns when RTC
 * peripherals power up). While the key is held, the task reports each hold
 * threshold as it passes so the UI can buzz feedback (spec §8).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    KEY_DOWN = 0,
    KEY_UP,
    KEY_HOLD,
} key_kind_t;

/* Hold levels reported while the key stays down. */
enum {
    HOLD_NOT_MORSE = 1, /* 1.5 s: this press is not a dot or dash */
    HOLD_SEND = 2,      /* 2 s */
    HOLD_CMD = 3,       /* 3 s */
    HOLD_CANCEL = 4,    /* 5 s */
};

typedef struct {
    key_kind_t kind;
    int64_t t_us;    /* edge (or threshold) time */
    int64_t down_us; /* when the press started */
    uint8_t level;   /* KEY_HOLD only */
} key_evt_t;

typedef void (*keyer_cb_t)(const key_evt_t *e);

esp_err_t keyer_init(keyer_cb_t cb);
bool keyer_is_down(void);
