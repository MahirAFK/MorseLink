/*
 * MorseLink — haptic player (see haptics.h).
 *
 * A job (hseq.h) is a list of tones, Morse text and gaps. hseq turns it into
 * segments: "M1 at level a, M2 at level b, for n ms". This file plays the
 * segments with one esp_timer one-shot per segment boundary. Deadlines are
 * kept as absolute times (s_next_us), so a late timer callback shortens the
 * next segment instead of shifting everything after it.
 *
 * Concurrency: the UI task calls play/stop, the esp_timer task runs the
 * callbacks, so all state is guarded by s_mtx. A generation counter (s_gen)
 * lets a callback that fires just after a stop or a new play recognise it
 * belongs to an old job and do nothing.
 *
 * Rail gating (V1.1 only): the motor supply (DCDC1) is switched on before a
 * job and off HAPTIC_RAIL_OFF_DELAY_MS after the last one, to save the
 * regulator's idle current. On V1.2 pmic.c makes this a no-op.
 */
#include "haptics.h"

#include "board.h"
#include "config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pmic.h"

static const char *TAG = "haptics";

#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_RES LEDC_TIMER_10_BIT
#define DUTY_FULL (1u << 10) /* 2^res = constant high */
#define CH_M1 LEDC_CHANNEL_0
#define CH_M2 LEDC_CHANNEL_1
#define CH_LED LEDC_CHANNEL_2 /* test setup: board LED mirrors the motors */

static SemaphoreHandle_t s_mtx;            /* guards everything below */
static esp_timer_handle_t s_seg_timer;     /* fires at the end of each segment */
static esp_timer_handle_t s_rail_timer;    /* switches the motor rail off after a quiet spell */
static haptics_done_cb_t s_done_cb;        /* told when a job finishes by itself */

static hjob_t s_job;           /* the job playing (a copy: the caller's may go away) */
static hseq_t s_seq;           /* where we are in it */
static haptics_gen_fn s_gen_fn; /* or: the generator playing (haptics_play_gen), NULL for a job */
static void *s_gen_ctx;         /* ... and its state */
static hparams_t s_params;     /* reading speed, dash split, ... */
static bool s_playing;
static uint32_t s_tag;         /* the caller's tag for the job playing */
static uint32_t s_gen;         /* bumped by every play / stop / raw */
static uint32_t s_armed_gen;   /* s_gen when the segment timer was armed */
static int64_t s_next_us;      /* absolute time the current segment ends */
static uint8_t s_int_m1 = 100, s_int_m2 = 100; /* intensity trims, % */
static bool s_gating = HAPTIC_RAIL_GATING;

/* On-time bookkeeping (run-down logs, TEST_PLAN §14): how long each motor
 * output has been on (any level above 0) since start-up. A motor that is on
 * right now has its current stretch added when it goes off, or when someone
 * asks (haptics_on_time_ms). Microseconds, so nothing is lost to rounding. */
static bool s_on1, s_on2;             /* is each output on right now? */
static int64_t s_since1, s_since2;    /* when it went on (valid while on) */
static uint64_t s_total1, s_total2;   /* finished stretches, us */

/* duty_for() -- PWM duty for a segment level (0-255) scaled by a motor's
 * intensity trim (%). Full level at 100 % is a constant high output. */
static uint32_t duty_for(uint8_t level, uint8_t pct)
{
    if (level == 0 || pct == 0) {
        return 0;
    }
    if (level == 255 && pct >= 100) {
        return DUTY_FULL;
    }
    return (uint32_t)level * pct * DUTY_FULL / (255u * 100u);
}

/* track_on() -- on-time bookkeeping for one output: note when it goes on,
 * add the stretch to the total when it goes off. Called with s_mtx held. */
static void track_on(bool on, bool *was_on, int64_t *since, uint64_t *total, int64_t now)
{
    if (on && !*was_on) {
        *since = now; /* a stretch starts */
    } else if (!on && *was_on) {
        *total += (uint64_t)(now - *since); /* a stretch ends */
    }
    *was_on = on;
}

/* set_motors() -- drive both motors (and the test LED) at these levels,
 * and keep the on-time totals. Called with s_mtx held. */
static void set_motors(uint8_t m1, uint8_t m2)
{
    uint32_t d1 = duty_for(m1, s_int_m1), d2 = duty_for(m2, s_int_m2);
    ledc_set_duty(LEDC_MODE, CH_M1, d1);
    ledc_update_duty(LEDC_MODE, CH_M1);
    ledc_set_duty(LEDC_MODE, CH_M2, d2);
    ledc_update_duty(LEDC_MODE, CH_M2);
    int64_t now = esp_timer_get_time();
    track_on(d1 > 0, &s_on1, &s_since1, &s_total1, now); /* "on" = the pin is driven at all */
    track_on(d2 > 0, &s_on2, &s_since2, &s_total2, now);
#if USE_ONBOARD_LED
    /* One LED for two motors: the wrist motor (or both) shows bright, the
     * forearm motor alone shows dim, so a dash reads as bright-then-dim. */
    uint8_t led = m1 ? m1 : (uint8_t)((uint32_t)m2 * LED_M2_PCT / 100u);
    ledc_set_duty(LEDC_MODE, CH_LED, duty_for(led, 100));
    ledc_update_duty(LEDC_MODE, CH_LED);
#endif
}

/* rail_on_locked() -- make sure the motors have power before playing:
 * switch the rail on if it is off and give it HAPTIC_RAIL_SETTLE_MS to come
 * up. Called with s_mtx held. */
static void rail_on_locked(void)
{
    if (pmic_present() && !pmic_motor_rail_is_on()) {
        pmic_motor_rail(true);
        vTaskDelay(pdMS_TO_TICKS(HAPTIC_RAIL_SETTLE_MS));
    }
}

/* arm_rail_off_locked() -- (re)start the countdown that switches the
 * motor rail off once nothing has played for HAPTIC_RAIL_OFF_DELAY_MS.
 * Called with s_mtx held. */
static void arm_rail_off_locked(void)
{
    if (s_gating && pmic_present()) {
        esp_timer_stop(s_rail_timer);
        esp_timer_start_once(s_rail_timer, (uint64_t)HAPTIC_RAIL_OFF_DELAY_MS * 1000);
    }
}

/* step_locked() -- start the next segment and arm the timer for its end;
 * returns true if there was none (the job just finished, motors off).
 * Called with s_mtx held. A segment due in under 50 us is given 50 us, so a
 * late callback can never make the timer fire in the past. */
static bool step_locked(void)
{
    hseg_t seg;
    bool more = s_gen_fn ? s_gen_fn(s_gen_ctx, &seg) : hseq_next(&s_seq, &seg);
    if (more) {
        set_motors(seg.m1, seg.m2);
        s_next_us += (int64_t)seg.ms * 1000;
        int64_t wait = s_next_us - esp_timer_get_time();
        if (wait < 50) {
            wait = 50;
        }
        s_armed_gen = s_gen;
        esp_timer_start_once(s_seg_timer, (uint64_t)wait);
        return false;
    }
    set_motors(0, 0);
    s_playing = false;
    arm_rail_off_locked();
    return true;
}

/* seg_timer_cb() -- esp_timer task: a segment ended, play the next one.
 * Ignored if the job was stopped or replaced since the timer was armed. The
 * done callback runs outside the lock (it posts to the UI's queue). */
static void seg_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_playing || s_armed_gen != s_gen) {
        xSemaphoreGive(s_mtx); /* stopped or replaced meanwhile */
        return;
    }
    bool finished = step_locked();
    uint32_t tag = s_tag;
    xSemaphoreGive(s_mtx);
    if (finished && s_done_cb) {
        s_done_cb(tag);
    }
}

/* rail_timer_cb() -- esp_timer task: nothing has played for a while, so
 * switch the motor rail off (unless a job started meanwhile). */
static void rail_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_playing && s_gating) {
        pmic_motor_rail(false);
    }
    xSemaphoreGive(s_mtx);
}

/* haptics_init() -- the PWM timer (20 kHz, 10-bit), one channel per motor
 * (plus the test LED), and the two esp_timers. Starts with the motor rail
 * off if gating is on. Returns ESP_FAIL if any piece fails. */
esp_err_t haptics_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    hparams_default(&s_params);

    ledc_timer_config_t t = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = HAPTIC_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&t);
    if (err != ESP_OK) {
        return err;
    }
    ledc_channel_config_t c = {
        .gpio_num = PIN_MOTOR_M1,
        .speed_mode = LEDC_MODE,
        .channel = CH_M1,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&c);
    c.gpio_num = PIN_MOTOR_M2;
    c.channel = CH_M2;
    err |= ledc_channel_config(&c);
#if USE_ONBOARD_LED
    /* The LED is active-low (lit when the pin is low). Driven open-drain, so
     * the pin is only ever pulled low or let go: if the external key is wired
     * to GPIO4 while this is still on, a press can't short a driven pin. */
    c.gpio_num = PIN_ONBOARD_LED;
    c.channel = CH_LED;
    c.flags.output_invert = 1;
    err |= ledc_channel_config(&c);
    err |= gpio_od_enable(PIN_ONBOARD_LED);
#endif
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    const esp_timer_create_args_t seg_args = {.callback = seg_timer_cb, .name = "hseg"};
    const esp_timer_create_args_t rail_args = {.callback = rail_timer_cb, .name = "hrail"};
    err = esp_timer_create(&seg_args, &s_seg_timer);
    err |= esp_timer_create(&rail_args, &s_rail_timer);
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    if (s_gating && pmic_present()) {
        pmic_motor_rail(false);
    }
    ESP_LOGI(TAG, "M1=GPIO%d M2=GPIO%d, PWM %d Hz, rail gating %s", PIN_MOTOR_M1, PIN_MOTOR_M2, HAPTIC_PWM_HZ,
             s_gating ? "on" : "off");
#if USE_ONBOARD_LED
    ESP_LOGW(TAG, "test setup: output mirrored on the board LED (GPIO4): dot = flash, dash = bright then dim");
#endif
    return ESP_OK;
}

/* haptics_set_done_cb() -- who is told when a job finishes by itself. */
void haptics_set_done_cb(haptics_done_cb_t cb)
{
    s_done_cb = cb;
}

/* haptics_set_params() -- reading speed and dash shape for the next job. */
void haptics_set_params(const hparams_t *p)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_params = *p;
    xSemaphoreGive(s_mtx);
}

/* haptics_get_params() -- a copy of the current playback parameters. */
void haptics_get_params(hparams_t *p)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *p = s_params;
    xSemaphoreGive(s_mtx);
}

/* haptics_set_intensity() -- per-motor intensity trims, % (capped at 100). */
void haptics_set_intensity(uint8_t m1_pct, uint8_t m2_pct)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_int_m1 = m1_pct > 100 ? 100 : m1_pct;
    s_int_m2 = m2_pct > 100 ? 100 : m2_pct;
    xSemaphoreGive(s_mtx);
}

/* haptics_get_intensity() -- the per-motor intensity trims, %. */
void haptics_get_intensity(uint8_t *m1_pct, uint8_t *m2_pct)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *m1_pct = s_int_m1;
    *m2_pct = s_int_m2;
    xSemaphoreGive(s_mtx);
}

/* haptics_set_rail_gating() -- switch the motor rail off between jobs, or
 * keep it on all the time (and switch it on now). */
void haptics_set_rail_gating(bool on)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_gating = on;
    if (!on) {
        esp_timer_stop(s_rail_timer);
        if (pmic_present()) {
            pmic_motor_rail(true);
        }
    } else if (!s_playing) {
        arm_rail_off_locked();
    }
    xSemaphoreGive(s_mtx);
}

/* haptics_play() -- see haptics.h: replace whatever is playing with `job`.
 * The first segment starts at once, from the calling task. */
void haptics_play(const hjob_t *job, uint32_t tag)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_timer_stop(s_seg_timer);
    esp_timer_stop(s_rail_timer);
    s_gen++;
    s_job = *job;
    hseq_start(&s_seq, &s_job, &s_params);
    s_gen_fn = NULL; /* (a job, not a generator) */
    s_tag = tag;
    s_playing = true;
    rail_on_locked();
    s_next_us = esp_timer_get_time();
    bool finished = step_locked();
    xSemaphoreGive(s_mtx);
    if (finished && s_done_cb) {
        s_done_cb(tag); /* empty job */
    }
}

/* haptics_play_gen() -- see haptics.h: as haptics_play(), with the segments
 * coming from `next` instead of a job's sequence. */
void haptics_play_gen(haptics_gen_fn next, void *ctx, uint32_t tag)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_timer_stop(s_seg_timer);
    esp_timer_stop(s_rail_timer);
    s_gen++;
    s_gen_fn = next;
    s_gen_ctx = ctx;
    s_tag = tag;
    s_playing = true;
    rail_on_locked();
    s_next_us = esp_timer_get_time();
    bool finished = step_locked();
    xSemaphoreGive(s_mtx);
    if (finished && s_done_cb) {
        s_done_cb(tag); /* an empty pattern */
    }
}

/* haptics_stop() -- cut the current job short (no done callback); motors
 * off, rail countdown started. */
void haptics_stop(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_timer_stop(s_seg_timer);
    s_gen++;
    if (s_playing) {
        s_playing = false;
        arm_rail_off_locked();
    }
    set_motors(0, 0);
    xSemaphoreGive(s_mtx);
}

/* haptics_park() -- see haptics.h. ledc_stop() leaves each pin at the given
 * idle level, still driven by the PWM peripheral; reconfiguring the pins as
 * plain GPIOs instead would trip ESP-IDF's "pin already in use" warnings.
 * The LED channel's output is inverted, so idle 0 means the pin is let go:
 * dark. */
void haptics_park(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_timer_stop(s_seg_timer);
    esp_timer_stop(s_rail_timer);
    s_gen++;
    s_playing = false;
    ledc_stop(LEDC_MODE, CH_M1, 0);
    ledc_stop(LEDC_MODE, CH_M2, 0);
#if USE_ONBOARD_LED
    ledc_stop(LEDC_MODE, CH_LED, 0);
#endif
    int64_t now = esp_timer_get_time(); /* both outputs are low now: close the on-time stretches */
    track_on(false, &s_on1, &s_since1, &s_total1, now);
    track_on(false, &s_on2, &s_since2, &s_total2, now);
    xSemaphoreGive(s_mtx);
}

/* haptics_on_time_ms() -- see haptics.h: each motor's total on-time since
 * start-up, the stretch running right now included. Truncated to whole ms;
 * the 32-bit result wraps after 49 days (callers take differences). */
void haptics_on_time_ms(uint32_t *m1_ms, uint32_t *m2_ms)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    uint64_t t1 = s_total1 + (s_on1 ? (uint64_t)(now - s_since1) : 0);
    uint64_t t2 = s_total2 + (s_on2 ? (uint64_t)(now - s_since2) : 0);
    xSemaphoreGive(s_mtx);
    *m1_ms = (uint32_t)(t1 / 1000);
    *m2_ms = (uint32_t)(t2 / 1000);
}

/* haptics_motors_on() -- see haptics.h. A plain read of two flags: a
 * snapshot that may be a moment old, which is all the caller needs. */
bool haptics_motors_on(void)
{
    return s_on1 || s_on2;
}

/* haptics_busy() -- is a job playing? */
bool haptics_busy(void)
{
    return s_playing;
}

/* haptics_led() -- see haptics.h. Only the LED's channel is written; the
 * next motor change (set_motors) takes the LED back. */
bool haptics_led(uint8_t level)
{
#if USE_ONBOARD_LED
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ledc_set_duty(LEDC_MODE, CH_LED, duty_for(level, 100));
    ledc_update_duty(LEDC_MODE, CH_LED);
    xSemaphoreGive(s_mtx);
    return true;
#else
    (void)level;
    return false;
#endif
}

/* haptics_raw() -- console `motor`: drive the motors at fixed levels
 * until changed (0, 0 stops and starts the rail countdown). */
void haptics_raw(uint8_t m1, uint8_t m2)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_timer_stop(s_seg_timer);
    s_gen++;
    s_playing = false;
    if (m1 || m2) {
        esp_timer_stop(s_rail_timer);
        rail_on_locked();
    } else {
        arm_rail_off_locked();
    }
    set_motors(m1, m2);
    xSemaphoreGive(s_mtx);
}
