/*
 * MorseLink — bench tests (see bench.h): the steps of `bench power` and the
 * pattern of `bench motor`. Pure C: no ESP-IDF, no globals that change.
 */
#include "bench.h"

#include <stddef.h>

/* The steps (TEST_PLAN §14.1's sheet, one row each; v0.6.1, agreed 20 Oct). */
const bench_step_t BENCH_POWER[] = {
    {"1", 1, BS_CPU, 0, 0, 0, 60, "processor only (radio standby, Bluetooth off)"},
    {"1b", 1, BS_KEYING, 0, 0, 0, 45, "+ keying: key PARIS over and over at your usual speed"},
    {"1c", 1, BS_KEYING, 0, 0, 0, 15, "+ the key held down: three holds of about 3 s (not 5 s: that stops it)"},
    {"2", 2, BS_LISTEN, 0, 0, 0, 60, "+ LoRa listening at SF9"},
    {"3a", 3, BS_BLE_ADV, 0, 0, 0, 60, "+ Bluetooth advertising, not connectable (no phone can connect)"},
    {"3b", 3, BS_BLE_CONN, 0, 0, 0, 60, "Bluetooth connectable: the paired phone reconnects (app open nearby)"},
    {"3c", 3, BS_RADIO_STBY, 0, 0, 0, 60, "LoRa to standby, Bluetooth still connected"},
    {"3d", 3, BS_LISTEN_SF12, 0, 0, 0, 60, "LoRa listening at SF12, Bluetooth connected"},
    {"4a", 4, BS_CARRIER, 17, 0, 0, 10, "back to SF9; transmitting a carrier, 17 dBm"},
    {"4b", 4, BS_CARRIER, 20, 0, 0, 10, "transmitting a carrier, 20 dBm"},
    {"4c", 4, BS_CARRIER, 22, 0, 0, 10, "transmitting a carrier, 22 dBm (SX1262 only)"},
    {"5a", 5, BS_MOTOR, 0, 255, 0, 15, "motor M1 100 %"},
    {"5b", 5, BS_MOTOR, 0, 128, 0, 15, "motor M1 50 %"},
    {"5c", 5, BS_MOTOR, 0, 0, 255, 15, "motor M2 100 %"},
    {"5d", 5, BS_MOTOR, 0, 255, 255, 15, "both motors 100 %"},
    {"6", 6, BS_OFF, 0, 0, 0, 0, "switched off (read the drain, then hold the key 2 s)"},
};
const int bench_power_n = (int)(sizeof BENCH_POWER / sizeof BENCH_POWER[0]);

/* bench_skips() -- see bench.h. */
bool bench_skips(int i, int max_dbm)
{
    return i >= 0 && i < bench_power_n && BENCH_POWER[i].kind == BS_CARRIER && BENCH_POWER[i].dbm > max_dbm;
}

/* bench_signal_ms() -- see bench.h: a new number gets that many buzzes
 * (each on and off for BENCH_BLINK_MS) and BENCH_PAUSE_MS; a part of the
 * same step one short buzz and BENCH_SUB_PAUSE_MS. */
uint32_t bench_signal_ms(int i, int prev_step)
{
    const bench_step_t *s = &BENCH_POWER[i];
    if (s->step != prev_step) {
        return 2u * BENCH_BLINK_MS * s->step + BENCH_PAUSE_MS - BENCH_BLINK_MS; /* (no still time after the last buzz:
                                                                                  the pause follows it) */
    }
    return BENCH_SUB_BLINK_MS + BENCH_SUB_PAUSE_MS;
}

/* bench_power_ms() -- see bench.h: each step's time plus its signal. */
uint32_t bench_power_ms(int max_dbm)
{
    uint32_t ms = 0;
    int last = 0;
    for (int i = 0; i < bench_power_n; i++) {
        if (bench_skips(i, max_dbm)) {
            continue; /* this chip can't: skipped */
        }
        ms += bench_signal_ms(i, last) + BENCH_POWER[i].secs * 1000u;
        last = BENCH_POWER[i].step;
    }
    return ms;
}

/* bench_power_secs() -- see bench.h. */
uint32_t bench_power_secs(int max_dbm)
{
    return bench_power_ms(max_dbm) / 1000u;
}

/* The units of the dot / dash trains: reading speeds 9 .. 1 (as hseq.c's
 * table), then three faster ones, to find where the motor smears. */
static const uint16_t UNITS[] = {120, 140, 160, 180, 200, 220, 250, 280, 320, 100, 80, 60};
const int bench_units_n = (int)(sizeof UNITS / sizeof UNITS[0]);

/* bench_unit_ms() -- see bench.h. */
uint16_t bench_unit_ms(int i)
{
    return (i >= 0 && i < bench_units_n) ? UNITS[i] : 0;
}

/* The parts of the pattern, in order. */
enum {
    BP_START = 0, /* 1 s start buzz, 2 s silence */
    BP_STEADY,    /* 3 s at 25 / 50 / 75 / 100 %, 2 s silence after each */
    BP_DOTS,      /* ten dots at the current unit, then 2 s silence */
    BP_DASHES,    /* ten dashes at the current unit, then 2 s silence */
    BP_END,       /* 1 s end buzz */
    BP_DONE,
};

#define TRAIN_N 10        /* dots (or dashes) per train */
#define GAP_MS 2000       /* silence between the parts */
#define MARK_MS 1000      /* the start and end buzzes */
#define STEADY_MS 3000    /* each steady buzz */

/* bench_motor_start() -- see bench.h. */
void bench_motor_start(bench_motor_t *b, int motor)
{
    b->motor = (uint8_t)(motor == 2 ? 2 : 1);
    b->phase = BP_START;
    b->unit = 0;
    b->k = 0;
}

/* seg() -- fill a segment: `level` on the chosen motor (0: silence), for ms. */
static void seg(const bench_motor_t *b, hseg_t *out, uint8_t level, uint16_t ms)
{
    out->ms = ms;
    out->m1 = b->motor == 1 ? level : 0;
    out->m2 = b->motor == 2 ? level : 0;
}

/*
 * bench_motor_next() -- see bench.h. A small state machine: `phase` is the
 * part, `k` counts the segments inside it (an element's "on" and its "off"
 * are two segments, so a train of ten is 20, plus the 2 s gap after it),
 * `unit` which unit the trains are at.
 */
bool bench_motor_next(void *ctx, hseg_t *out)
{
    bench_motor_t *b = ctx;
    static const uint8_t STEADY[4] = {64, 128, 191, 255}; /* 25 / 50 / 75 / 100 % of full */
    for (;;) {
        switch (b->phase) {
        case BP_START:
            if (b->k == 0) {
                b->k++;
                seg(b, out, 255, MARK_MS);
                return true;
            }
            b->phase = BP_STEADY;
            b->k = 0;
            seg(b, out, 0, GAP_MS);
            return true;
        case BP_STEADY:
            if (b->k < 8) { /* on, off for each of the four levels */
                bool on = (b->k % 2) == 0;
                seg(b, out, on ? STEADY[b->k / 2] : 0, on ? STEADY_MS : GAP_MS);
                b->k++;
                return true;
            }
            b->phase = BP_DOTS;
            b->k = 0;
            b->unit = 0;
            continue;
        case BP_DOTS:
        case BP_DASHES: {
            uint16_t u = UNITS[b->unit];
            if (b->k < 2 * TRAIN_N) {
                bool on = (b->k % 2) == 0;
                uint16_t len = on ? (uint16_t)(b->phase == BP_DOTS ? u : 3 * u) : u;
                seg(b, out, on ? 255 : 0, len);
                b->k++;
                return true;
            }
            /* The train's last "off" was one unit; make the pause after it 2 s in all. */
            seg(b, out, 0, (uint16_t)(GAP_MS - u));
            b->k = 0;
            if (b->phase == BP_DOTS) {
                b->phase = BP_DASHES;
            } else if (b->unit + 1 < bench_units_n) {
                b->unit++;
                b->phase = BP_DOTS;
            } else {
                b->phase = BP_END;
            }
            return true;
        }
        case BP_END:
            b->phase = BP_DONE;
            seg(b, out, 255, MARK_MS);
            return true;
        default:
            return false;
        }
    }
}

/* bench_motor_ms() -- see bench.h: walk the pattern once and add it up. */
uint32_t bench_motor_ms(void)
{
    bench_motor_t b;
    hseg_t s;
    uint32_t ms = 0;
    bench_motor_start(&b, 1);
    while (bench_motor_next(&b, &s)) {
        ms += s.ms;
    }
    return ms;
}
