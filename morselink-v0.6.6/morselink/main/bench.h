/*
 * MorseLink — bench tests for the power and motor measurements
 * (docs/TEST_PLAN.md §14.1 and §14.3), the parts that are pure C: the steps
 * of `bench power` and the vibration pattern of `bench motor`. The UI task
 * runs them (ui.c), the haptics player plays the pattern (haptics.c), and
 * the net task switches the radio (net.c). Host-tested.
 *
 * bench power (the meter session; v0.6.1 steps, agreed 20 Oct): with USB
 * unplugged and a multimeter in series with the battery, the board steps
 * through its states by itself, about a minute each, so the owner can read
 * the current of each. Each step adds to or changes the one before:
 *   1   processor only: radio in standby, Bluetooth off          60 s
 *   1b  + keying: PARIS over and over at the usual speed           45 s
 *   1c  + the key held down: three holds of about 3 s              15 s
 *   2   + LoRa listening at SF9 (the normal idle; run-down A)      60 s
 *   3a  + Bluetooth advertising, not connectable                   60 s
 *   3b  Bluetooth connectable: the paired phone reconnects         60 s
 *   3c  LoRa to standby, Bluetooth still connected                 60 s
 *   3d  LoRa listening at SF12, Bluetooth connected                60 s
 *   4   back to SF9 listening; a plain carrier at 17, 20, 22 dBm   10 s each
 *       (22 only on an SX1262; counted in the 10 % airtime)
 *   5   motor M1 at 100 %, then 50 %; M2 100 %; both 100 %         15 s each
 *   6   switched off (as F): read the drain, then hold the key 2 s
 * What each isolates: 1 the base; 1b / 1c keying (the decoding, the key's
 * pull-up; on the onboard-button build also nothing else); 2 receiving; 3a
 * advertising; 3b connected; 3c a cross-check (3c - 1 should equal 3b - 2:
 * the currents add); 3d whether receiving depends on the SF (expected:
 * not); 4 transmitting per power; 5 the motors (4 and 5 on top of 3b's
 * state); 6 the drain while off.
 *
 * Signals, on motor M1 (agreed 20 Oct; the board LED until v0.6.0): before
 * each new step number, that many buzzes, then a pause; between the parts
 * of one step (1b, 1c, 3b ... 5d) one short buzz. They come between the
 * readings, never during one. The phone, if one is connected, is told each
 * step as it starts (BN lines, phone.h), and the app runs the same
 * timetable by itself for the steps when it can't be (Bluetooth off).
 *
 * Started from the console (`bench power`), it waits for USB to be
 * unplugged, however long that takes (a 5 s hold or `bench stop` cancels);
 * started from the phone (BENCH POWER, with no USB), at once. Then
 * BENCH_SETTLE_S for the meter to settle, and step 1's signal.
 *
 * bench motor [m1|m2]: one motor plays a fixed pattern for the phone's
 * accelerometer (phyphox), about 4 minutes:
 *   a 1 s start buzz (to line the recording up), 2 s silence;
 *   steady 3 s buzzes at 25 / 50 / 75 / 100 %, 2 s silence after each;
 *   for each reading speed 9 down to 1 (unit 120 ... 320 ms), then the
 *   faster units 100, 80 and 60 ms: ten dots (1 unit on, 1 off), 2 s
 *   silence, ten dashes (3 units on, 1 off), 2 s silence;
 *   a 1 s end buzz.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hseq.h"

#define BENCH_SETTLE_S 5      /* bench power: after USB is out (or the phone's start), this long before step 1 */
#define BENCH_BLINK_MS 300    /* a step number's signal: each buzz this long, then as long still */
#define BENCH_PAUSE_MS 2000   /* still after the buzzes, before the step */
#define BENCH_SUB_BLINK_MS 100 /* between the parts of one step: one short buzz ... */
#define BENCH_SUB_PAUSE_MS 1000 /* ... and this long still */
#define BENCH_SIGNAL_LEVEL 255 /* the buzzes: M1 at full */

/* What a step of bench power does (what changes from the step before). */
typedef enum {
    BS_CPU = 0,     /* radio standby, Bluetooth off */
    BS_KEYING,      /* nothing changes: the owner keys (1b) or holds the key (1c) */
    BS_LISTEN,      /* radio listening at SF9 */
    BS_BLE_ADV,     /* + Bluetooth advertising, not connectable */
    BS_BLE_CONN,    /* Bluetooth connectable: the paired phone reconnects */
    BS_RADIO_STBY,  /* radio standby again, Bluetooth stays */
    BS_LISTEN_SF12, /* radio listening at SF12 */
    BS_CARRIER,     /* radio at SF9, sending a plain carrier at `dbm` (back to listening after) */
    BS_MOTOR,       /* motors at levels m1, m2 */
    BS_OFF,         /* switched off */
} bench_kind_t;

typedef struct {
    const char *id;    /* "1", "1b", "3a", "4b"...: the sheet's row and the phone's BN lines */
    uint8_t step;      /* the number signalled (1-6) */
    uint8_t kind;      /* bench_kind_t */
    int8_t dbm;        /* BS_CARRIER: power */
    uint8_t m1, m2;    /* BS_MOTOR: levels 0-255 */
    uint16_t secs;     /* how long it lasts (BS_OFF: until the key wakes it) */
    const char *what;  /* for the console and the sheet */
} bench_step_t;

/* The steps of bench power, in order (bench_power_n of them). The app has
 * the same list (lib/bench.dart); the totals are checked on both sides. */
extern const bench_step_t BENCH_POWER[];
extern const int bench_power_n;

/* Total time of bench power from step 1's signal to switching off, in ms
 * (bench_power_ms) or whole seconds (bench_power_secs), with the signals;
 * steps a chip can't do (22 dBm on an SX1276: max_dbm) are left out. */
uint32_t bench_power_ms(int max_dbm);
uint32_t bench_power_secs(int max_dbm);

/* The signal before step i: how long it takes, ms (a new step number's
 * buzzes and pause, or a part's short buzz and pause); `prev_step` is the
 * number of the step done before it (0 at the start). */
uint32_t bench_signal_ms(int i, int prev_step);

/* Is step i one a chip with this maximum power can't do (22 dBm on an
 * SX1276)? */
bool bench_skips(int i, int max_dbm);

/* The vibration pattern of bench motor, as a stream of segments (TEST_PLAN
 * §14.3; v0.6.5): a 1 s start buzz (full), 2 s silence; a 3 s steady buzz at
 * each strength level I1 .. I8 (hseq.h's drive table: 45 ... 100 %), 2 s
 * silence after each; then for each unit (speeds 9 .. 1, then 100, 80,
 * 60 ms) ten dots (1 on, 1 off) and ten dashes (3 on, 1 off) at the
 * strength set with I, 2 s apart; a 1 s end buzz (full). Below full drive
 * every buzz starts with the same kick as real playback (HAPTIC_KICK_MS at
 * full, then the level), so the motor behaves as it does with I. I9 is I8
 * with the supply at 3.4 V: set I9 before the run and the I8 buzz is I9. */
typedef struct {
    uint8_t motor;     /* 1 = M1, 2 = M2 */
    uint8_t phase;     /* which part of the pattern */
    uint8_t unit;      /* index into the units list */
    uint16_t k;        /* element within the part */
    uint8_t drive_pct; /* the trains' drive (the strength set with I), % of full */
    uint8_t kick_ms;   /* the start kick below full drive (0: none) */
    bool kicked;       /* the current "on" segment's kick has been given */
    uint8_t steady_one; /* (v0.6.6, a block of the sweep) 0: the steady part is I1 .. I8; n: one steady buzz at
                           drive_pct (the block's level n) */
    bool no_end;        /* (v0.6.6) leave out the end buzz (a sweep block that another follows) */
} bench_motor_t;

/* Start the pattern on motor 1 or 2; the trains at drive_pct (% of full,
 * the I level's), each buzz below full starting with a kick of kick_ms. */
void bench_motor_start(bench_motor_t *b, int motor, uint8_t drive_pct, uint8_t kick_ms);

/* The steady part's levels: I1 .. I8 (bench_steady_n of them). */
#define BENCH_STEADY_N 8

/*
 * The sweep (v0.6.6, `bench motor m1 all`, agreed 27 Oct: no gaps in the
 * IMU data): one block per strength I1 .. I9, played back to back by the
 * UI, which sets the motor supply for each block (3.4 V for I9). A block:
 * the 1 s start buzz (full) and 2 s silence, one 3 s steady buzz at the
 * block's level and 2 s silence, then the dot / dash trains at that level;
 * the last block ends with the 1 s end buzz. Each block's start buzz lets
 * the analysis line that block up on its own.
 */
#define BENCH_SWEEP_LEVELS 9
void bench_sweep_block_start(bench_motor_t *b, int motor, int level, uint8_t kick_ms);
/* How long block `level` (1..9) takes, ms; the last one includes the end buzz. */
uint32_t bench_sweep_block_ms(int level);
/* The whole sweep, ms. */
uint32_t bench_sweep_ms(void);

/* The next segment (motor levels and duration); false when the pattern is
 * over. Usable as the haptics player's generator (haptics_play_gen). */
bool bench_motor_next(void *ctx, hseg_t *out);

/* How long the whole pattern takes, ms (the same at every drive: a kick
 * is part of its buzz). */
uint32_t bench_motor_ms(void);

/* The units the dot / dash trains use, in order (bench_units_n of them):
 * speeds 9 .. 1, then 100, 80, 60 ms. */
uint16_t bench_unit_ms(int i);
extern const int bench_units_n;
