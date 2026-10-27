/*
 * MorseLink — test traffic (docs/TEST_PLAN.md §2 and §4.4): which test is
 * running, when a board in test mode sends a message and how long it is,
 * and the text of a test message.
 *
 * Tests:
 *   baseline   120 messages, 30 each of 5 / 25 / 50 / 100 characters in a
 *              shuffled order, one every 20 s; the rover starts 5 s after
 *              the start, the base 10 s later, so the two alternate
 *   dynamic    one message every 10-20 s (uniform), 5-100 characters
 *              (uniform), until stopped
 *   rundown-a  no messages (listening only; Bluetooth off), until the
 *              battery gives out
 *   rundown-b  dynamic traffic, until the battery gives out
 *   rundown-m  no messages; "PARIS" plays on the motors over and over, at
 *              the reading speed set (Bluetooth off); until the battery
 *              gives out or it is stopped (TEST_PLAN §14: the motor night)
 *   rundown-l  dynamic traffic over LoRa only, Bluetooth off (agreed 15
 *              Oct: run-down A plus LoRa traffic), until the battery gives
 *              out or it is stopped (5 s hold)
 *   point      (v0.6.1, agreed 20 Oct) both boards still, at one spot: a
 *              plan of one or more steps, each a radio setting (SF, power)
 *              applied to both boards for the test only. Per step each board
 *              sends 20 messages of 25 characters over LoRa only (the relay
 *              is not used), and keeps sending while the link is lost (the
 *              delivery at the edge is the measurement). One step is a
 *              single point test, several a sweep; the default sweep is
 *              SF9 / 17 dBm, SF9 / 22 dBm, SF12 / 22 dBm. See "The point
 *              test's timetable" below.
 *
 * The pace follows the spreading factor (agreed 15 Oct), so that each board
 * stays under ~6 % of the airtime (the skip rule is 8 %): the dynamic gap is
 * 10-20 s at SF9, 20-40 s at SF10, 40-60 s at SF11, 90-130 s at SF12; the
 * baseline grid 20 / 30 / 45 / 90 s, with 60 messages instead of 120 at SF12
 * (40 / 60 / 90 / 90 minutes).
 *
 * Test message text: a 4-character marker, then filler to the exact size.
 * The marker is the sequence number in base 36 (3 characters, "000"-"ZZZ")
 * and the path letter, L (LoRa) or R (relay); the filler repeats "PARIS ".
 * Example, 25 characters: "01CLPARIS PARIS PARIS PAR". The two copies of
 * one message have the same sequence number and differ in the path letter.
 *
 * The point test's timetable (agreed 20 Oct). Both boards get the whole
 * plan in their START and run it on their own clocks; no packets coordinate
 * them. Step i starts at START + tt_point_offset_ms(p, i), computed from the
 * plan alone, so both boards compute the same timetable. Each step is
 *   settle   the new setting, nothing sent: 10 s at SF9-10, 20 s at SF11-12
 *            (it absorbs the 0.1-2 s by which the two boards' STARTs differ);
 *   sending  n messages (20) one gap apart: the rover's at settle + k gaps,
 *            the base's half a gap later, so the two interleave;
 *   tail     one more gap after the last, so the base's last message has its
 *            result (receipt, or given up) before the step ends.
 * The gap (15 / 20 / 25 / 40 s at SF9 / 10 / 11 / 12) is longer than a
 * message that fails takes to give up (5 copies and the receipt wait: about
 * 13 / 14 / 21 / 35 s, plus a packet's worth of listen-before-talk), so each
 * message is finished before the next is due and never fails with the one
 * before it (the engine fails every waiting message with the one being
 * delivered). A step lasts settle + (n + 1) gaps: 5.4 / 7.2 / 9.1 / 14.3 min;
 * the default sweep 25.2 min. Airtime per board, first-try delivery: about
 * 9 s per SF9 step and 76 s per SF12 step, so two sweeps an hour stay near
 * 5 % (the skip rule is 8 %).
 *
 * Everything is reproducible: the same parameters (seed included) give the
 * same sequence of times and sizes. Pure C (no ESP-IDF): host-tested.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TT_BASELINE_N 120        /* baseline: messages per board (30 of each size; 60 at SF12) */
#define TT_BASELINE_GAP_MS 20000 /* baseline: one message every 20 s (at SF9: tt_baseline_gap_ms) */
#define TT_ROVER_FIRST_MS 5000   /* the rover's first message, after the start */
#define TT_BASE_FIRST_MS 15000   /* the base's: half a grid later (10 s at SF9), so the two alternate */
#define TT_DYN_GAP_MIN_MS 10000  /* dynamic: a message every 10-20 s (uniform; at SF9: tt_dyn_gap) */
#define TT_DYN_GAP_MAX_MS 20000
#define TT_SIZE_MIN 5            /* dynamic: 5-100 characters (uniform); 5 = marker + 1 */
#define TT_SIZE_MAX 100
#define TT_MARKER_LEN 4          /* "SSSP": sequence (base 36) + path letter */
#define TT_SEQ_MOD 46656         /* 36^3: sequence numbers wrap here */
#define TT_SESSION_MAX 23        /* longest session name */
#define TT_ARGS_MAX 160          /* longest parameters line (a point test's plan makes it longer than 100) */

/* The point test (agreed 20 Oct; the timetable above). */
#define TT_POINT_N 20            /* messages per board per step */
#define TT_POINT_SIZE 25         /* ... of 25 characters each */
#define TT_PLAN_MAX 8            /* steps in a plan */

/* (New tests go at the end: the run-down log stores the number.) */
typedef enum {
    TT_BASELINE = 0,
    TT_DYNAMIC,
    TT_RUNDOWN_A,
    TT_RUNDOWN_B,
    TT_RUNDOWN_M,
    TT_RUNDOWN_L,
    TT_POINT, /* v0.6.1 */
    TT__TESTS
} tt_test_t;
typedef enum { TT_BOTH = 0, TT_LORA, TT_RELAY, TT__PATHS } tt_paths_t;
typedef enum { TT_ROVER = 0, TT_BASE, TT__ROLES } tt_role_t;

/* One step of a point test's plan: the radio setting both boards use. */
typedef struct {
    uint8_t sf;  /* spreading factor 9-12 */
    int8_t dbm;  /* transmit power (each board clamps it to its chip's maximum) */
} tt_step_t;

/* A test's parameters, as the phone (or the console) gives them. */
typedef struct {
    uint8_t test;                       /* tt_test_t */
    uint8_t paths;                      /* tt_paths_t */
    uint8_t role;                       /* tt_role_t */
    uint32_t seed;                      /* the session's seed (same on both boards) */
    char session[TT_SESSION_MAX + 1];   /* the rover phone's start time, YYYYMMDD-HHMMSS */
    uint8_t sf;                         /* the board's spreading factor (set by the board, not the phone;
                                           0 counts as SF9): the pace follows it */
    /* point (v0.6.1): */
    uint8_t n_steps;                    /* steps in the plan (0: none given; tt_parse_params then sets the
                                           default sweep for a point test) */
    tt_step_t plan[TT_PLAN_MAX];        /* plan=9:17,9:22,12:22 */
    uint8_t n;                          /* n=: messages per board per step (1..TT_POINT_N; 0 = TT_POINT_N) */
} tt_params_t;

/* The schedule of one board's test traffic. */
typedef struct {
    tt_params_t p;
    uint32_t rng;                 /* xorshift32 state, from the seed and the role */
    int made;                     /* messages generated so far */
    int64_t next_ms;              /* when the next one is due (INT64_MAX: none) */
    uint8_t sizes[TT_BASELINE_N]; /* baseline: the shuffled sizes */
    /* point: */
    int64_t start_ms;             /* the START (the timetable counts from it) */
    int step;                     /* the step of the message tt_next() returned last (1-based; 0: none) */
    int k;                        /* the next message's number within its step (0-based) */
} tt_sched_t;

/* Names, as the parameters and log lines spell them ("?" when out of range). */
const char *tt_test_name(int test);
const char *tt_paths_name(int paths);
const char *tt_role_name(int role);

/*
 * Parse "test=dynamic paths=both role=rover session=20261017-101500
 * seed=12345": key=value words in any order, separated by spaces. `test` is
 * required; the others default to both, rover, "" and 0. A point test also
 * takes plan=<sf>:<dBm>,... (steps separated by ',' or '/'; SF 9-12, power
 * RADIO_TX_DBM_MIN-MAX, at most TT_PLAN_MAX steps; default: the sweep
 * 9:17,9:22,12:22) and n=1..20
 * (default 20); a point test always runs on LoRa only (paths = lora). Unknown
 * keys are ignored (a newer app may send more). False, with *why set, if a
 * value is not understood.
 */
bool tt_parse_params(const char *s, tt_params_t *p, const char **why);

/* The plan as text, the steps separated by `sep`: ',' as the parameters
 * spell it ("9:17,9:22,12:22"), '/' in the T lines, whose fields are
 * separated by commas ("plan=9:17/9:22/12:22"). Into out[size] (cut short if
 * it doesn't fit). Returns out. */
const char *tt_plan_text(const tt_params_t *p, char sep, char *out, int size);

/* Does this test send messages? (baseline, dynamic, rundown-b, rundown-l,
 * point.) */
bool tt_sends(int test);
/* Is it a run-down (the battery logged to flash every minute)? */
bool tt_is_rundown(int test);
/* Does it play on the motors? (rundown-m: everything else plays nothing.) */
bool tt_plays(int test);
/* Does it switch Bluetooth off for the run? (rundown-a, rundown-m,
 * rundown-l.) Such a test can only use LoRa (rundown-l's paths are lora). */
bool tt_ble_off(int test);

/* The pace at spreading factor `sf` (0 = SF9): the dynamic gap's range,
 * the baseline grid, and the number of baseline messages per board. */
void tt_dyn_gap(int sf, int32_t *min_ms, int32_t *max_ms);
int32_t tt_baseline_gap_ms(int sf);
int tt_baseline_n(int sf);
/* Does this setting send an L copy / an R copy of each message? */
bool tt_uses_lora(int paths);
bool tt_uses_relay(int paths);

/* Start the schedule at `start_ms`. */
void tt_init(tt_sched_t *s, const tt_params_t *p, int64_t start_ms);

/* Is a message due at `now`? If so: true, its sequence number (from 1,
 * wrapping at TT_SEQ_MOD) and size, and the schedule moves on. At most one
 * per call, so a late caller catches up one message at a time. */
bool tt_next(tt_sched_t *s, int64_t now, uint16_t *seq, int *size);

/* Baseline: all its messages made (tt_baseline_n). (Never true for the
 * others; a point test ends by time: tt_point_step_at.) */
bool tt_finished(const tt_sched_t *s);

/* ---- the point test's timetable (pure; the same on both boards) ---- */

/* The gap between one board's messages at `sf` (15 / 20 / 25 / 40 s), and
 * the settle time at the start of a step (10 / 10 / 20 / 20 s). */
int32_t tt_point_gap_ms(int sf);
int32_t tt_point_settle_ms(int sf);
/* How long one step at `sf` lasts with n messages (0 = TT_POINT_N):
 * settle + (n + 1) gaps. */
int32_t tt_point_step_ms(int sf, int n);
/* When step i (0-based) starts, counted from the START; i = n_steps gives the
 * end of the test. */
int64_t tt_point_offset_ms(const tt_params_t *p, int i);
/* Which step is on at `since_ms` after the START: 0 .. n_steps - 1, or
 * n_steps once the test is over. */
int tt_point_step_at(const tt_params_t *p, int64_t since_ms);
/* Messages per step this plan uses (p->n, or TT_POINT_N). */
int tt_point_n(const tt_params_t *p);

/* Write the test text for `seq`, path letter 'L' or 'R', exactly `len`
 * characters (TT_SIZE_MIN..TT_SIZE_MAX) and a terminating NUL into out.
 * Returns len, or -1 if it doesn't fit or len is out of range. */
int tt_text(char *out, int out_size, uint16_t seq, char path, int len);

/* Is text[0..len) exactly a test text? If so, its sequence number and path
 * letter. Anything a person keys never matches (the filler must be exact). */
bool tt_parse(const char *text, int len, uint16_t *seq, char *path);
