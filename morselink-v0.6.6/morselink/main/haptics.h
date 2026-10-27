/*
 * MorseLink — haptic player.
 *
 * Walks a job's segment stream (hseq.h) with esp_timer one-shots: microsecond
 * timing, deadlines tracked absolutely so latency never accumulates (spec:
 * no vTaskDelay for element timing). Motors are driven through LEDC so per-
 * motor intensity can be trimmed; at 100 % the outputs are simply high.
 *
 * One job plays at a time; playing a new job replaces the current one. The
 * UI sequences everything, so there is no queue here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hseq.h"

typedef void (*haptics_done_cb_t)(uint32_t tag); /* runs in the esp_timer task: keep it short */

esp_err_t haptics_init(void);
void haptics_set_done_cb(haptics_done_cb_t cb);

void haptics_set_params(const hparams_t *p);
void haptics_get_params(hparams_t *p);
void haptics_set_intensity(uint8_t m1_pct, uint8_t m2_pct);
void haptics_get_intensity(uint8_t *m1_pct, uint8_t *m2_pct); /* the trims, % */
void haptics_set_rail_gating(bool on);

/* Stop whatever is playing and start `job`. The done callback fires with
 * `tag` when it finishes naturally (not when stopped or replaced). */
void haptics_play(const hjob_t *job, uint32_t tag);

/* The same for a stream of segments from a generator instead of a job:
 * next(ctx, &seg) gives each segment, false at the end (bench motor's
 * pattern, bench.h). ctx must stay valid while it plays. The intensity
 * trims apply as usual. */
typedef bool (*haptics_gen_fn)(void *ctx, hseg_t *seg);
void haptics_play_gen(haptics_gen_fn next, void *ctx, uint32_t tag);
void haptics_stop(void);
bool haptics_busy(void);

/* Direct drive for bench tests (stops playback). Levels 0..255. */
void haptics_raw(uint8_t m1, uint8_t m2);

/* Test setup only (USE_ONBOARD_LED): the board LED alone at `level`
 * (0..255), the motors untouched: bench power's step signals, which must
 * not run a motor. Without the LED it does nothing (returns false). */
bool haptics_led(uint8_t level);

/* How long each motor output has been on (driven at any level) since
 * start-up, in ms, the stretch playing right now included. For the
 * run-down log's per-minute motor time (TEST_PLAN §14); wraps after 49 days,
 * so use differences. Any task may call it. */
void haptics_on_time_ms(uint32_t *m1_ms, uint32_t *m2_ms);

/* Is either motor output on right now? (rundown-m reads the battery only
 * while both are off: a running motor pulls the voltage down a little.) */
bool haptics_motors_on(void);

/* Before switching off (power.c): stop for good. Playback and the rail
 * timer stop, and the PWM channels stop with the motor outputs low (the
 * test LED dark), ready for power.c to hold the pins there through the deep
 * sleep. Nothing should play afterwards. */
void haptics_park(void);
