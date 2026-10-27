/*
 * MorseLink — host tests for the test traffic (ttest.c): the parameters
 * line, the baseline and dynamic schedules, and the test message text.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"
#include "config.h"
#include "link.h"
#include "proto.h"
#include "ttest.h"

extern int g_fail, g_pass; /* shared with test_main.c */

#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        if (cond) {                                                                                            \
            g_pass++;                                                                                          \
        } else {                                                                                               \
            g_fail++;                                                                                          \
            printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                                         \
            printf(__VA_ARGS__);                                                                               \
            printf("\n");                                                                                      \
        }                                                                                                      \
    } while (0)

/* The parameters line: every key, defaults, any order, unknown keys, errors. */
static void test_params(void)
{
    printf("test mode: parameters\n");
    tt_params_t p;
    const char *why;
    CHECK(tt_parse_params("test=dynamic paths=lora role=base session=20261017-101500 seed=4294967295", &p, &why),
          "full line (%s)", why ? why : "");
    CHECK(p.test == TT_DYNAMIC && p.paths == TT_LORA && p.role == TT_BASE && p.seed == 4294967295u &&
              strcmp(p.session, "20261017-101500") == 0,
          "every field read");
    CHECK(tt_parse_params("  seed=7   test=baseline  ", &p, &why) && p.test == TT_BASELINE && p.paths == TT_BOTH &&
              p.role == TT_ROVER && p.seed == 7 && p.session[0] == '\0',
          "any order, extra spaces, defaults");
    CHECK(tt_parse_params("test=rundown-b colour=blue", &p, &why) && p.test == TT_RUNDOWN_B, "unknown keys ignored");
    CHECK(!tt_parse_params("paths=both", &p, &why) && why, "test is required");
    CHECK(!tt_parse_params("test=walk", &p, &why), "unknown test refused");
    CHECK(!tt_parse_params("test=dynamic paths=wifi", &p, &why), "unknown paths refused");
    CHECK(!tt_parse_params("test=dynamic role=boss", &p, &why), "unknown role refused");
    CHECK(!tt_parse_params("test=dynamic seed=-1", &p, &why) && !tt_parse_params("test=dynamic seed=4294967296", &p, &why),
          "bad seeds refused");
    CHECK(!tt_parse_params("test dynamic", &p, &why), "a word without = refused");
    CHECK(tt_parse_params("test=dynamic session=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123", &p, &why) &&
              strlen(p.session) == TT_SESSION_MAX,
          "a long session name is cut");
    CHECK(strcmp(tt_test_name(TT_RUNDOWN_A), "rundown-a") == 0 && strcmp(tt_paths_name(TT_RELAY), "relay") == 0 &&
              strcmp(tt_role_name(TT_BASE), "base") == 0 && strcmp(tt_test_name(9), "?") == 0,
          "names");
    CHECK(tt_sends(TT_BASELINE) && tt_sends(TT_DYNAMIC) && tt_sends(TT_RUNDOWN_B) && !tt_sends(TT_RUNDOWN_A) &&
              !tt_sends(TT_RUNDOWN_M),
          "rundown-a and rundown-m send nothing");
    /* rundown-m (v0.5.1, TEST_PLAN §14): parsed by name, numbered after the
     * older tests (the run-down log stores the number), a run-down that
     * plays and switches Bluetooth off. */
    CHECK(tt_parse_params("test=rundown-m", &p, &why) && p.test == TT_RUNDOWN_M && TT_RUNDOWN_M == 4 &&
              strcmp(tt_test_name(TT_RUNDOWN_M), "rundown-m") == 0,
          "rundown-m parsed and named");
    CHECK(tt_is_rundown(TT_RUNDOWN_A) && tt_is_rundown(TT_RUNDOWN_B) && tt_is_rundown(TT_RUNDOWN_M) &&
              !tt_is_rundown(TT_BASELINE) && !tt_is_rundown(TT_DYNAMIC),
          "which tests are run-downs");
    CHECK(tt_plays(TT_RUNDOWN_M) && !tt_plays(TT_RUNDOWN_A) && !tt_plays(TT_RUNDOWN_B) && !tt_plays(TT_BASELINE) &&
              !tt_plays(TT_DYNAMIC),
          "only rundown-m plays");
    CHECK(tt_ble_off(TT_RUNDOWN_A) && tt_ble_off(TT_RUNDOWN_M) && !tt_ble_off(TT_RUNDOWN_B) &&
              !tt_ble_off(TT_BASELINE) && !tt_ble_off(TT_DYNAMIC),
          "Bluetooth off in rundown-a and rundown-m only");
    CHECK(tt_uses_lora(TT_BOTH) && tt_uses_relay(TT_BOTH) && tt_uses_lora(TT_LORA) && !tt_uses_relay(TT_LORA) &&
              !tt_uses_lora(TT_RELAY) && tt_uses_relay(TT_RELAY),
          "paths");
    /* rundown-l (v0.6.0, agreed 15 Oct): numbered after rundown-m, sends
     * (LoRa only), a run-down with Bluetooth off, plays nothing. */
    CHECK(tt_parse_params("test=rundown-l", &p, &why) && p.test == TT_RUNDOWN_L && TT_RUNDOWN_L == 5 &&
              strcmp(tt_test_name(TT_RUNDOWN_L), "rundown-l") == 0,
          "rundown-l parsed and named");
    CHECK(tt_sends(TT_RUNDOWN_L) && tt_is_rundown(TT_RUNDOWN_L) && tt_ble_off(TT_RUNDOWN_L) && !tt_plays(TT_RUNDOWN_L),
          "rundown-l: sends, a run-down, Bluetooth off, plays nothing");
}

/* run_schedule() -- step a schedule in 100 ms ticks until `until_ms`,
 * recording each message's time, sequence number and size. */
static int run_schedule(tt_sched_t *s, int64_t until_ms, int64_t *t, uint16_t *q, int *sz, int max)
{
    int n = 0;
    for (int64_t now = 0; now <= until_ms; now += 100) {
        uint16_t seq;
        int size;
        if (tt_next(s, now, &seq, &size) && n < max) {
            t[n] = now;
            q[n] = seq;
            sz[n] = size;
            n++;
        }
    }
    return n;
}

/* The pace follows the spreading factor (agreed 15 Oct): each board stays
 * under ~6 % of the airtime at every setting. */
static void test_pace(void)
{
    printf("test mode: pace per spreading factor\n");
    static const int32_t want_lo[4] = {10000, 20000, 40000, 90000}, want_hi[4] = {20000, 40000, 60000, 130000};
    static const int32_t want_grid[4] = {20000, 30000, 45000, 90000};
    int ok = 1;
    for (int sf = 9; sf <= 12; sf++) {
        int32_t lo, hi;
        tt_dyn_gap(sf, &lo, &hi);
        ok &= lo == want_lo[sf - 9] && hi == want_hi[sf - 9] && tt_baseline_gap_ms(sf) == want_grid[sf - 9];
    }
    int32_t lo0, hi0;
    tt_dyn_gap(0, &lo0, &hi0);
    CHECK(ok && lo0 == TT_DYN_GAP_MIN_MS && hi0 == TT_DYN_GAP_MAX_MS && tt_baseline_gap_ms(0) == TT_BASELINE_GAP_MS &&
              tt_baseline_n(0) == TT_BASELINE_N,
          "gaps 10-20 / 20-40 / 40-60 / 90-130 s, grid 20 / 30 / 45 / 90 s; SF 0 counts as SF9");
    /* Airtime per board at the pace: the average message (about 52
     * characters) plus a receipt for the other board's, plus keepalives. */
    for (int sf = 9; sf <= 12; sf++) {
        lora_params_t lp = {(uint8_t)sf, RADIO_BW_HZ, RADIO_CR};
        link_timing_t tm;
        link_timing(&lp, &tm);
        int32_t lo, hi;
        tt_dyn_gap(sf, &lo, &hi);
        double gap = (lo + hi) / 2.0;
        double msg = lora_airtime_us(PROTO_HDR_LEN + 52, &lp, 8) / 1000.0, rcpt = lora_airtime_us(PROTO_HDR_LEN + 2, &lp, 8) / 1000.0;
        double ka = lora_airtime_us(PROTO_HDR_LEN + KA_REPORT_LEN, &lp, 8) / 1000.0;
        double pct = 100.0 * ((msg + rcpt) / gap + ka / tm.keepalive_ms);
        printf("  SF%d dynamic: about %.1f %% airtime per board\n", sf, pct);
        CHECK(pct < 6.0, "SF%d: under 6 %% (%.1f %%), leaving room under the 8 %% skip rule", sf, pct);
    }
    /* Baseline at SF12: 60 messages, 15 of each size, every 90 s; the base
     * half a grid after the rover. */
    static int64_t t[200];
    static uint16_t q[200];
    static int sz[200];
    tt_params_t p = {.test = TT_BASELINE, .role = TT_ROVER, .seed = 7, .sf = 12};
    tt_sched_t s;
    tt_init(&s, &p, 0);
    int n = run_schedule(&s, 4 * 3600 * 1000LL, t, q, sz, 200);
    int counts[101] = {0}, grid_ok = 1;
    for (int i = 0; i < n; i++) {
        counts[sz[i]]++;
        grid_ok &= t[i] == TT_ROVER_FIRST_MS + (int64_t)i * 90000;
    }
    CHECK(n == 60 && tt_finished(&s) && counts[5] == 15 && counts[25] == 15 && counts[50] == 15 && counts[100] == 15 &&
              grid_ok,
          "SF12 baseline: %d messages, 15 of each size, every 90 s", n);
    p.role = TT_BASE;
    tt_init(&s, &p, 0);
    n = run_schedule(&s, 4 * 3600 * 1000LL, t, q, sz, 200);
    CHECK(n == 60 && t[0] == TT_ROVER_FIRST_MS + 45000, "the base starts 45 s after the rover");
    /* Dynamic at SF12 for 10 h: every gap within 90-130 s. */
    static int64_t td[1000];
    static uint16_t qd[1000];
    static int szd[1000];
    p = (tt_params_t){.test = TT_DYNAMIC, .role = TT_ROVER, .seed = 3, .sf = 12};
    tt_init(&s, &p, 0);
    n = run_schedule(&s, 10 * 3600 * 1000LL, td, qd, szd, 1000);
    int64_t gmin = INT64_MAX, gmax = 0;
    for (int i = 1; i < n; i++) {
        int64_t g = td[i] - td[i - 1];
        gmin = g < gmin ? g : gmin;
        gmax = g > gmax ? g : gmax;
    }
    CHECK(n > 270 && n < 400 && gmin >= 90000 && gmax <= 130100, "SF12 dynamic: %d in 10 h, gaps %lld..%lld ms", n,
          (long long)gmin, (long long)gmax);
}

/* Baseline: 120 messages, 30 of each size, a fixed 20 s grid starting at
 * 5 s (rover) or 15 s (base), the same for the same seed, then nothing. */
static void test_baseline(void)
{
    printf("test mode: baseline schedule\n");
    static int64_t t[200], t2[200];
    static uint16_t q[200], q2[200];
    static int sz[200], sz2[200];
    tt_params_t p = {.test = TT_BASELINE, .role = TT_ROVER, .seed = 12345};
    tt_sched_t s;
    tt_init(&s, &p, 0);
    int n = run_schedule(&s, 3 * 3600 * 1000LL, t, q, sz, 200);
    CHECK(n == TT_BASELINE_N, "120 messages (%d)", n);
    CHECK(tt_finished(&s), "then finished");
    int counts[101] = {0}, grid_ok = 1, seq_ok = 1;
    for (int i = 0; i < n; i++) {
        counts[sz[i]]++;
        grid_ok &= t[i] == TT_ROVER_FIRST_MS + (int64_t)i * TT_BASELINE_GAP_MS;
        seq_ok &= q[i] == i + 1;
    }
    CHECK(counts[5] == 30 && counts[25] == 30 && counts[50] == 30 && counts[100] == 30, "30 of each size (%d %d %d %d)",
          counts[5], counts[25], counts[50], counts[100]);
    CHECK(grid_ok, "one every 20 s from 5 s");
    CHECK(seq_ok, "sequence numbers 1..120");
    int runs = 0;
    for (int i = 1; i < n; i++) {
        runs += sz[i] != sz[i - 1];
    }
    CHECK(runs > 40, "sizes are shuffled (%d changes)", runs);
    tt_sched_t s2;
    tt_init(&s2, &p, 0);
    int n2 = run_schedule(&s2, 3 * 3600 * 1000LL, t2, q2, sz2, 200);
    CHECK(n2 == n && memcmp(sz, sz2, sizeof(int) * (size_t)n) == 0, "same seed, same order");
    p.role = TT_BASE;
    tt_init(&s2, &p, 0);
    n2 = run_schedule(&s2, 3 * 3600 * 1000LL, t2, q2, sz2, 200);
    CHECK(n2 == n && t2[0] == TT_BASE_FIRST_MS && memcmp(sz, sz2, sizeof(int) * (size_t)n) != 0,
          "the base starts 10 s later, in another order");
    p.role = TT_ROVER;
    p.seed = 54321;
    tt_init(&s2, &p, 0);
    n2 = run_schedule(&s2, 3 * 3600 * 1000LL, t2, q2, sz2, 200);
    CHECK(memcmp(sz, sz2, sizeof(int) * (size_t)n) != 0, "another seed, another order");
}

/* Dynamic: gaps 10-20 s and sizes 5-100, both ends reached, reproducible;
 * rundown-a sends nothing; a late caller catches up one at a time. */
static void test_dynamic(void)
{
    printf("test mode: dynamic schedule\n");
    static int64_t t[3000];
    static uint16_t q[3000];
    static int sz[3000];
    tt_params_t p = {.test = TT_DYNAMIC, .role = TT_ROVER, .seed = 99};
    tt_sched_t s;
    tt_init(&s, &p, 0);
    int n = run_schedule(&s, 10 * 3600 * 1000LL, t, q, sz, 3000);
    int64_t gmin = INT64_MAX, gmax = 0;
    int smin = 1000, smax = 0;
    for (int i = 0; i < n; i++) {
        if (i) {
            int64_t g = t[i] - t[i - 1];
            gmin = g < gmin ? g : gmin;
            gmax = g > gmax ? g : gmax;
        }
        smin = sz[i] < smin ? sz[i] : smin;
        smax = sz[i] > smax ? sz[i] : smax;
    }
    CHECK(n > 1700 && n < 2700, "about one every 15 s over 10 h (%d)", n);
    CHECK(t[0] == TT_ROVER_FIRST_MS, "first after 5 s");
    CHECK(gmin >= TT_DYN_GAP_MIN_MS && gmax <= TT_DYN_GAP_MAX_MS + 100 && gmin < 10500 && gmax > 19500,
          "gaps within 10-20 s, both ends reached (%lld..%lld)", (long long)gmin, (long long)gmax);
    CHECK(smin == TT_SIZE_MIN && smax == TT_SIZE_MAX, "sizes 5..100 (%d..%d)", smin, smax);
    CHECK(!tt_finished(&s), "never finished");
    tt_sched_t s2;
    tt_init(&s2, &p, 0);
    uint16_t seq;
    int size, same = 1;
    for (int i = 0; i < 50; i++) {
        int64_t at = s2.next_ms;
        same &= tt_next(&s2, at, &seq, &size) && size == sz[i] && t[i] >= at && t[i] < at + 100; /* (t: 100 ms ticks) */
    }
    CHECK(same, "reproducible");
    p.test = TT_RUNDOWN_A;
    tt_init(&s2, &p, 0);
    CHECK(!tt_next(&s2, 100LL * 3600 * 1000, &seq, &size), "rundown-a never sends");
    p.test = TT_RUNDOWN_M;
    tt_init(&s2, &p, 0);
    CHECK(!tt_next(&s2, 100LL * 3600 * 1000, &seq, &size), "rundown-m never sends");
    p.test = TT_DYNAMIC;
    tt_init(&s2, &p, 0);
    int made = 0;
    while (tt_next(&s2, 60000, &seq, &size)) {
        made++;
        if (made > 10) {
            break;
        }
    }
    CHECK(made >= 3 && made <= 6, "late by a minute: catches up one call at a time (%d)", made);
}

/* Bench tests (bench.h, TEST_PLAN §14): the power steps and the motor
 * pattern. */
static void test_bench(void)
{
    printf("bench: power steps and the motor pattern\n");
    int ordered = 1, carriers = 0, motors = 0;
    for (int i = 1; i < bench_power_n; i++) {
        ordered &= BENCH_POWER[i].step >= BENCH_POWER[i - 1].step && BENCH_POWER[i].step <= BENCH_POWER[i - 1].step + 1;
    }
    for (int i = 0; i < bench_power_n; i++) {
        carriers += BENCH_POWER[i].kind == BS_CARRIER;
        motors += BENCH_POWER[i].kind == BS_MOTOR;
    }
    CHECK(ordered && BENCH_POWER[0].step == 1 && BENCH_POWER[0].kind == BS_CPU &&
              BENCH_POWER[bench_power_n - 1].kind == BS_OFF && BENCH_POWER[bench_power_n - 1].step == 6,
          "steps 1..6 in order, from processor-only to switched off");
    CHECK(carriers == 3 && motors == 4, "three carrier levels (17, 20, 22 dBm), four motor states");
    uint32_t t22 = bench_power_secs(22), t20 = bench_power_secs(20);
    CHECK(t22 >= 500 && t22 < 600 && t22 - t20 == 11, "about %u s in all (an SX1276 skips 22 dBm: %u s)", t22, t20);
    /* v0.6.1 (agreed 20 Oct): the steps by name, as the sheet and the app
     * have them; the app's lib/bench.dart checks the same total. */
    static const char *const IDS[] = {"1", "1b", "1c", "2", "3a", "3b", "3c", "3d", "4a", "4b", "4c",
                                      "5a", "5b", "5c", "5d", "6"};
    int ids_ok = bench_power_n == (int)(sizeof IDS / sizeof IDS[0]);
    for (int i = 0; ids_ok && i < bench_power_n; i++) {
        ids_ok = strcmp(BENCH_POWER[i].id, IDS[i]) == 0 && BENCH_POWER[i].step == (uint8_t)(IDS[i][0] - '0');
    }
    CHECK(ids_ok, "the steps 1, 1b, 1c, 2, 3a-3d, 4a-4c, 5a-5d, 6, each under its number");
    CHECK(bench_power_ms(22) == 543800 && bench_power_ms(20) == 532700,
          "the total, to the ms: %u (the app's timetable must say the same)", bench_power_ms(22));
    CHECK(bench_signal_ms(0, 0) == 2300 && bench_signal_ms(1, 1) == 1100 && bench_signal_ms(15, 5) == 5300,
          "signals: one buzz and the pause before 1, a short one before 1b, six before 6");
    CHECK(BENCH_POWER[1].kind == BS_KEYING && BENCH_POWER[2].kind == BS_KEYING && BENCH_POWER[4].kind == BS_BLE_ADV &&
              BENCH_POWER[5].kind == BS_BLE_CONN && BENCH_POWER[6].kind == BS_RADIO_STBY &&
              BENCH_POWER[7].kind == BS_LISTEN_SF12 && bench_skips(10, 20) && !bench_skips(10, 22),
          "the new steps' kinds; 4c skipped on a 20 dBm chip");
    /* The motor pattern on M2: nothing on M1, the parts in order. */
    bench_motor_t b;
    hseg_t sg;
    bench_motor_start(&b, 2);
    uint32_t total = 0;
    int segs = 0, m1_used = 0, on_steady = 0, dots = 0, dashes = 0;
    uint16_t first_unit = bench_unit_ms(0);
    while (bench_motor_next(&b, &sg) && segs < 2000) {
        segs++;
        total += sg.ms;
        m1_used += sg.m1 != 0;
        if (sg.m2 && sg.ms == 3000) {
            on_steady++;
        }
        if (sg.m2 == 255 && sg.ms == first_unit) {
            dots++;
        }
        if (sg.m2 == 255 && sg.ms == 3 * first_unit) {
            dashes++;
        }
    }
    uint32_t units = 0;
    for (int i = 0; i < bench_units_n; i++) {
        units += bench_unit_ms(i);
    }
    /* each train: ten elements, the last one's "off" stretched to a 2 s pause */
    uint32_t want = 1000 + 2000 + 4 * (3000 + 2000) + 58 * units + bench_units_n * 4000 + 1000;
    CHECK(total == want && total == bench_motor_ms(), "the pattern takes %u ms (expected %u)", total, want);
    CHECK(m1_used == 0, "bench motor m2 never drives M1");
    CHECK(on_steady == 4, "four steady 3 s buzzes (%d)", on_steady);
    CHECK(dots == 10 && dashes == 10, "ten dots and ten dashes at %u ms (%d, %d)", first_unit, dots, dashes);
    CHECK(bench_units_n == 12 && bench_unit_ms(0) == 120 && bench_unit_ms(8) == 320 && bench_unit_ms(11) == 60,
          "units: speeds 9..1 (120..320 ms), then 100, 80, 60 ms");
}

/* The text: exact sizes, the marker round trip, and what is not a test text. */
static void test_text(void)
{
    printf("test mode: message text\n");
    char buf[TT_SIZE_MAX + 1];
    CHECK(tt_text(buf, sizeof buf, 0x1C + 0, 'L', 25) == 25 && strcmp(buf, "00SLPARIS PARIS PARIS PAR") == 0,
          "seq 28 = 00S: \"%s\"", buf);
    CHECK(tt_text(buf, sizeof buf, 48, 'L', 25) == 25 && strcmp(buf, "01CLPARIS PARIS PARIS PAR") == 0,
          "the plan's example: \"%s\"", buf);
    int ok = 0, total = 0;
    for (unsigned seq = 0; seq < TT_SEQ_MOD; seq += 7) {
        for (int len = TT_SIZE_MIN; len <= TT_SIZE_MAX; len += 19) {
            char path = (seq & 1) ? 'R' : 'L';
            uint16_t s2;
            char p2;
            total++;
            ok += tt_text(buf, sizeof buf, (uint16_t)seq, path, len) == len && (int)strlen(buf) == len &&
                  tt_parse(buf, len, &s2, &p2) && s2 == seq && p2 == path;
        }
    }
    CHECK(ok == total, "round trip for many sequence numbers and sizes (%d of %d)", ok, total);
    CHECK(tt_text(buf, sizeof buf, 1, 'L', 4) < 0 && tt_text(buf, sizeof buf, 1, 'L', 101) < 0 &&
              tt_text(buf, sizeof buf, 1, 'X', 10) < 0 && tt_text(buf, 10, 1, 'L', 10) < 0,
          "bad sizes, path letters and buffers refused");
    uint16_t s;
    char pl;
    CHECK(!tt_parse("HELLO", 5, &s, &pl) && !tt_parse("SOS", 3, &s, &pl), "ordinary messages aren't test texts");
    CHECK(!tt_parse("001LPARIS PARIZ", 15, &s, &pl), "the filler must be exact");
    CHECK(!tt_parse("001XPARIS", 9, &s, &pl) && !tt_parse("0a1LPARIS", 9, &s, &pl), "path letter and digits checked");
    CHECK(tt_parse("ZZZRP", 5, &s, &pl) && s == TT_SEQ_MOD - 1 && pl == 'R', "the largest sequence number");
}

/* The point test (v0.6.1): its parameters, its plan, and the timetable both
 * boards compute from the plan alone. */
static void test_point(void)
{
    printf("test mode: the point test (plan, timetable)\n");
    tt_params_t p, q;
    const char *why;
    char buf[64];
    CHECK(tt_parse_params("test=point paths=both role=rover session=20261021-101500 seed=9 plan=9:17,10:20,12:22", &p,
                          &why) &&
              p.test == TT_POINT && TT_POINT == 6 && p.paths == TT_LORA && p.n_steps == 3 && p.plan[0].sf == 9 &&
              p.plan[0].dbm == 17 && p.plan[1].sf == 10 && p.plan[1].dbm == 20 && p.plan[2].sf == 12 && p.plan[2].dbm == 22,
          "a plan of three steps; LoRa only whatever paths said (%s)", why ? why : "");
    CHECK(strcmp(tt_plan_text(&p, ',', buf, sizeof buf), "9:17,10:20,12:22") == 0, "the plan as text: %s", buf);
    CHECK(strcmp(tt_plan_text(&p, '/', buf, sizeof buf), "9:17/10:20/12:22") == 0, "... and for a T line: %s", buf);
    CHECK(tt_parse_params("test=point plan=9:17/12:22", &q, &why) && q.n_steps == 2 && q.plan[1].sf == 12,
          "the T lines' spelling is read too");
    CHECK(tt_parse_params("test=point", &p, &why) && p.n_steps == 3 && p.plan[0].sf == 9 && p.plan[0].dbm == 17 &&
              p.plan[1].sf == 9 && p.plan[1].dbm == 22 && p.plan[2].sf == 12 && p.plan[2].dbm == 22 && tt_point_n(&p) == 20,
          "no plan: the default sweep 9:17, 9:22, 12:22, 20 messages a step");
    CHECK(tt_parse_params("test=point plan=11:20 n=2", &p, &why) && p.n_steps == 1 && tt_point_n(&p) == 2,
          "a single step, and n= for short tests");
    CHECK(tt_parse_params("test=point plan=9:2,12:22,9:22,10:17,11:20,12:17,9:20,10:22", &p, &why) && p.n_steps == 8,
          "eight steps (the most)");
    CHECK(!tt_parse_params("test=point plan=9:2,12:22,9:22,10:17,11:20,12:17,9:20,10:22,9:17", &p, &why) && why,
          "nine refused");
    const char *bad[] = {"plan=8:17", "plan=13:17", "plan=9:1", "plan=9:23", "plan=9", "plan=9:", "plan=:17",
                         "plan=9:17,", "plan=9:17,,12:22", "plan=9-17", "plan=", "n=0", "n=21", "n=x"};
    int refused = 0;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char line[64];
        snprintf(line, sizeof line, "test=point %s", bad[i]);
        refused += !tt_parse_params(line, &p, &why);
    }
    CHECK(refused == (int)(sizeof bad / sizeof bad[0]), "bad plans and counts refused (%d of %zu)", refused,
          sizeof bad / sizeof bad[0]);
    CHECK(tt_sends(TT_POINT) && !tt_is_rundown(TT_POINT) && !tt_plays(TT_POINT) && !tt_ble_off(TT_POINT) &&
              strcmp(tt_test_name(TT_POINT), "point") == 0,
          "point sends, isn't a run-down, plays nothing, keeps Bluetooth on");

    /* The timetable. */
    CHECK(tt_point_gap_ms(9) == 15000 && tt_point_gap_ms(10) == 20000 && tt_point_gap_ms(11) == 25000 &&
              tt_point_gap_ms(12) == 40000 && tt_point_settle_ms(9) == 10000 && tt_point_settle_ms(10) == 10000 &&
              tt_point_settle_ms(11) == 20000 && tt_point_settle_ms(12) == 20000,
          "gaps 15 / 20 / 25 / 40 s, settle 10 / 10 / 20 / 20 s");
    CHECK(tt_point_step_ms(9, 20) == 325000 && tt_point_step_ms(10, 20) == 430000 && tt_point_step_ms(11, 20) == 545000 &&
              tt_point_step_ms(12, 20) == 860000 && tt_point_step_ms(9, 2) == 55000,
          "steps of 325 / 430 / 545 / 860 s (and 55 s with n=2)");
    tt_parse_params("test=point role=rover", &p, &why);
    tt_parse_params("test=point role=base seed=77", &q, &why);
    CHECK(tt_point_offset_ms(&p, 0) == 0 && tt_point_offset_ms(&p, 1) == 325000 && tt_point_offset_ms(&p, 2) == 650000 &&
              tt_point_offset_ms(&p, 3) == 1510000,
          "the default sweep: steps at 0, 325, 650 s, over at 1510 s (25.2 min)");
    int same = 1;
    for (int i = 0; i <= 3; i++) {
        same &= tt_point_offset_ms(&p, i) == tt_point_offset_ms(&q, i);
    }
    CHECK(same, "rover and base compute the same timetable (it depends on the plan alone)");
    CHECK(tt_point_step_at(&p, 0) == 0 && tt_point_step_at(&p, 324999) == 0 && tt_point_step_at(&p, 325000) == 1 &&
              tt_point_step_at(&p, 1509999) == 2 && tt_point_step_at(&p, 1510000) == 3 && tt_point_step_at(&p, -500) == 0,
          "which step is on: the boundaries");

    /* Each board's messages: 20 per step, inside the step, the base half a
     * gap after the rover, the last one a gap and a half before the step's
     * end (time for its result). */
    for (int role = 0; role < 2; role++) {
        tt_params_t *pp = role ? &q : &p;
        tt_sched_t sc;
        tt_init(&sc, pp, 1000);
        int per[3] = {0, 0, 0}, inside = 1, spacing = 1, sizes = 1, steps_ok = 1, n = 0;
        int64_t last[3] = {0, 0, 0};
        uint16_t seq;
        int size;
        for (int64_t t = 1000; t < 1000 + 1510000 + 60000; t += 100) {
            while (tt_next(&sc, t, &seq, &size)) {
                n++;
                int i = sc.step - 1;
                steps_ok &= i >= 0 && i < 3 && seq == n;
                if (i < 0 || i > 2) {
                    break;
                }
                const int sf = pp->plan[i].sf;
                const int64_t gap = tt_point_gap_ms(sf);
                const int64_t due = 1000 + tt_point_offset_ms(pp, i) + tt_point_settle_ms(sf) +
                                    (int64_t)per[i] * gap + (role ? gap / 2 : 0);
                spacing &= t >= due && t < due + 100;
                inside &= t >= 1000 + tt_point_offset_ms(pp, i) + tt_point_settle_ms(sf) &&
                          t + gap + gap / 2 <= 1000 + tt_point_offset_ms(pp, i + 1) + (role ? 0 : gap / 2);
                sizes &= size == TT_POINT_SIZE;
                last[i] = t;
                per[i]++;
            }
        }
        CHECK(per[0] == 20 && per[1] == 20 && per[2] == 20 && n == 60 && steps_ok, "%s: 20 messages in each step (%d %d %d)",
              role ? "base" : "rover", per[0], per[1], per[2]);
        CHECK(spacing && inside && sizes, "%s: on its grid, inside its step, 25 characters each", role ? "base" : "rover");
        CHECK(!tt_next(&sc, 1000 + 10LL * 3600 * 1000, &seq, &size), "%s: nothing after the plan", role ? "base" : "rover");
        CHECK(last[2] + 40000 + 20000 <= 1000 + 1510000 + (role ? 0 : 20000), "%s: the last message has time for its result",
              role ? "base" : "rover");
    }

    /* Airtime: a default sweep, every message delivered at the first try,
     * plus the receipts for the partner's and the keepalives: well under 8 %
     * of an hour for two sweeps an hour. */
    double air = 0;
    for (int i = 0; i < p.n_steps; i++) {
        lora_params_t lp = {p.plan[i].sf, RADIO_BW_HZ, RADIO_CR};
        link_timing_t tm;
        link_timing(&lp, &tm);
        double msg = lora_airtime_us(PROTO_HDR_LEN + TT_POINT_SIZE, &lp, RADIO_PREAMBLE_LONG) / 1000.0;
        double rcpt = lora_airtime_us(PROTO_HDR_LEN + 2, &lp, RADIO_PREAMBLE_SHORT) / 1000.0;
        double ka = lora_airtime_us(PROTO_HDR_LEN + KA_REPORT_LEN, &lp, RADIO_PREAMBLE_SHORT) / 1000.0;
        air += 20 * msg + 20 * rcpt + (double)tt_point_step_ms(p.plan[i].sf, 20) / tm.keepalive_ms * ka;
    }
    printf("  a default sweep: 25.2 min, about %.0f s of airtime per board (two an hour: %.1f %%)\n", air / 1000,
           2 * air / 36000);
    CHECK(air > 60000 && 2 * air < 0.06 * 3600000, "a default sweep: %.0f s of airtime per board (two an hour: %.1f %%)",
          air / 1000, 2 * air / 36000);
}

/* test_ttest_all() -- every test-traffic test, called from main(). */
void test_ttest_all(void)
{
    test_params();
    test_baseline();
    test_dynamic();
    test_pace();
    test_bench();
    test_text();
    test_point();
}
