/*
 * MorseLink — serial console (see console.h).
 *
 * Bench and test tool: set the device ID, inspect calibration and the radio,
 * drive the motors, inject keying (so flows can be tested without touching
 * the key), simulate incoming traffic, and range-test with `radio ping`.
 *
 * Commands that change the UI's state are passed to the UI task with
 * ui_post_console() (it owns that state); radio tools talk to the network
 * task directly (net.h is safe from any task), and the pretend phone talks
 * to the BLE side directly (ble.h).
 */
#include "console.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hseq.h"
#include "keyer.h"
#include "morse.h"
#include "net.h"
#include "phone.h" /* PHONE_LINE_MAX */
#include "radio.h" /* radio_chip_name() */
#include "store.h" /* the run-down log */
#include "ttest.h"
#include "ui.h"

/* ------------------------------------------------------------------------- */
/* Key injection                                                              */
/* ------------------------------------------------------------------------- */

#define INJ_MAX 400 /* longest injected pattern */

typedef struct {
    int unit_ms;      /* dot length to key at */
    char pat[INJ_MAX];
} inj_t;

static QueueHandle_t s_inj_q;

/* sleep_until() -- block the inject task until esp_timer time t_us. */
static void sleep_until(int64_t t_us)
{
    int64_t wait = t_us - esp_timer_get_time();
    if (wait > 0) {
        vTaskDelay(pdMS_TO_TICKS((wait + 999) / 1000));
    }
}

/*
 * inj_press() -- simulate one press of dur_ms: KEY_DOWN, the hold
 * thresholds it passes (1.5 / 2 / 3 / 5 s, as the keyer would report
 * them), then KEY_UP. Blocks for the press's duration.
 */
static void inj_press(int dur_ms)
{
    static const int thr[] = {1500, 2000, 3000, 5000};
    int64_t t0 = esp_timer_get_time();
    key_evt_t e = {.kind = KEY_DOWN, .t_us = t0, .down_us = t0};
    ui_post_key(&e);
    for (int i = 0; i < 4; i++) {
        if (dur_ms > thr[i]) {
            sleep_until(t0 + (int64_t)thr[i] * 1000);
            key_evt_t h = {.kind = KEY_HOLD, .t_us = t0 + (int64_t)thr[i] * 1000, .down_us = t0, .level = (uint8_t)(i + 1)};
            ui_post_key(&h);
        }
    }
    int64_t t1 = t0 + (int64_t)dur_ms * 1000;
    sleep_until(t1);
    key_evt_t u = {.kind = KEY_UP, .t_us = t1, .down_us = t0};
    ui_post_key(&u);
}

/*
 * inject_task() -- plays queued patterns as key presses:
 *   .  dot (1 unit)          -  dash (3 units)
 *   ' ' letter gap (3 units)  /  word gap (7 units)
 *   S  2.2 s send hold        C  3.2 s command hold
 *   X  5.2 s cancel hold      W  wait 1 s
 * Each press is followed by a 1-unit gap.
 */
static void inject_task(void *arg)
{
    (void)arg;
    static inj_t job;
    for (;;) {
        if (xQueueReceive(s_inj_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int u = job.unit_ms;
        for (const char *p = job.pat; *p; p++) {
            int dur = 0;
            switch (*p) {
            case '.':
                dur = u;
                break;
            case '-':
                dur = 3 * u;
                break;
            case 'S':
                dur = 2200;
                break;
            case 'C':
                dur = 3200;
                break;
            case 'X':
                dur = 5200;
                break;
            case ' ': /* letter gap: 3 units in total */
                vTaskDelay(pdMS_TO_TICKS(2 * u));
                continue;
            case '/': /* word gap: 7 units in total */
                vTaskDelay(pdMS_TO_TICKS(6 * u));
                continue;
            case 'W':
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            default:
                continue;
            }
            inj_press(dur);
            vTaskDelay(pdMS_TO_TICKS(u)); /* gap inside a character */
        }
        printf("(injection done)\n");
    }
}

/* inject() -- queue a pattern; 0 on success, 1 if one is already waiting. */
static int inject(int unit, const char *pat)
{
    inj_t j;
    j.unit_ms = unit;
    strncpy(j.pat, pat, INJ_MAX - 1);
    j.pat[INJ_MAX - 1] = '\0';
    return xQueueSend(s_inj_q, &j, 0) == pdTRUE ? 0 : 1;
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static int g_unit = 100; /* injection unit; match it to your calibration */

/* join_args() -- argv[from..] joined with single spaces into out. */
static void join_args(int argc, char **argv, int from, char *out, size_t size)
{
    size_t n = 0;
    out[0] = '\0';
    for (int i = from; i < argc && n < size - 1; i++) {
        n += (size_t)snprintf(out + n, size - n, "%s%s", i > from ? " " : "", argv[i]);
    }
}

/* parse_id() -- a typed ID (any case) to its address; ADDR_NONE if invalid. */
static uint32_t parse_id(const char *s)
{
    char up[ID_MAX_LEN + 1];
    size_t n = strlen(s);
    if (n > ID_MAX_LEN) {
        return ADDR_NONE;
    }
    for (size_t i = 0; i <= n; i++) {
        up[i] = (char)toupper((unsigned char)s[i]);
    }
    return id_to_addr(up);
}

/* ------------------------------------------------------------------------- */
/* Commands (each: argc/argv as typed; returns 0 = ok, 1 = usage error)      */
/* ------------------------------------------------------------------------- */

/* status -- print the state, link, radio, calibration, contacts, battery. */
static int cmd_status(int argc, char **argv)
{
    ui_post_console(CON_STATUS, 0, 0, NULL);
    return 0;
}

/* id [XXX] -- show the status, or set this device's ID. */
static int cmd_id(int argc, char **argv)
{
    if (argc < 2) {
        ui_post_console(CON_STATUS, 0, 0, NULL);
        return 0;
    }
    char up[8] = {0};
    for (int i = 0; argv[1][i] && i < 7; i++) {
        up[i] = (char)toupper((unsigned char)argv[1][i]);
    }
    if (!id_valid(up)) {
        printf("invalid ID: 3 characters from %s\n", ID_ALPHABET);
        return 1;
    }
    ui_post_console(CON_SET_ID, 0, 0, up);
    return 0;
}

/* key [-u ms] PATTERN -- inject raw keying (see inject_task). */
static int cmd_key(int argc, char **argv)
{
    int from = 1;
    if (argc >= 3 && strcmp(argv[1], "-u") == 0) {
        g_unit = atoi(argv[2]);
        if (g_unit < 30) {
            g_unit = 30;
        }
        from = 3;
    }
    if (argc <= from) {
        printf("key [-u unit_ms] PATTERN   e.g. key \".... ..\"  (. - space=letter gap /=word gap S=send hold "
               "C=command hold X=cancel hold W=wait 1 s)\n");
        return 1;
    }
    char pat[INJ_MAX];
    join_args(argc, argv, from, pat, sizeof pat);
    return inject(g_unit, pat);
}

/* text [-s] MESSAGE -- key MESSAGE in Morse at the injection unit; with -s,
 * finish with a send hold. */
static int cmd_text(int argc, char **argv)
{
    int from = 1;
    bool send = false;
    if (argc >= 2 && strcmp(argv[1], "-s") == 0) {
        send = true;
        from = 2;
    }
    if (argc <= from) {
        printf("text [-s] MESSAGE   keys MESSAGE at the injection unit (-s: then hold to send)\n");
        return 1;
    }
    char msg[MSG_MAX_CHARS + 1], clean[MSG_MAX_CHARS + 1], pat[INJ_MAX];
    join_args(argc, argv, from, msg, sizeof msg);
    morse_sanitize(msg, clean, sizeof clean);
    size_t n = 0;
    for (const char *c = clean; *c && n < sizeof(pat) - 12; c++) {
        if (*c == ' ') {
            pat[n++] = '/';
            continue;
        }
        if (n && pat[n - 1] != '/') {
            pat[n++] = ' ';
        }
        const char *p = morse_encode(*c);
        while (p && *p && n < sizeof(pat) - 12) {
            pat[n++] = *p++;
        }
    }
    if (send) {
        pat[n++] = ' ';
        pat[n++] = 'S';
    }
    pat[n] = '\0';
    printf("keying \"%s\" at %d ms units: %s\n", clean, g_unit, pat);
    return inject(g_unit, pat);
}

/* rx ID MESSAGE -- pretend a message arrived from ID (skips the radio). */
static int cmd_rx(int argc, char **argv)
{
    if (argc < 3) {
        printf("rx ID MESSAGE   simulate an incoming message\n");
        return 1;
    }
    uint32_t a = parse_id(argv[1]);
    if (a == ADDR_NONE) {
        printf("invalid ID\n");
        return 1;
    }
    char msg[MSG_MAX_CHARS + 1];
    join_args(argc, argv, 2, msg, sizeof msg);
    ui_post_console(CON_RX, (int32_t)a, 0, msg);
    return 0;
}

/* sos ID [cancel] -- pretend an SOS (or its cancellation) arrived from ID. */
static int cmd_sos(int argc, char **argv)
{
    if (argc < 2) {
        printf("sos ID [cancel]   simulate an incoming SOS (or its cancellation) from ID\n");
        return 1;
    }
    uint32_t a = parse_id(argv[1]);
    if (a == ADDR_NONE) {
        printf("invalid ID\n");
        return 1;
    }
    if (argc >= 3 && strcmp(argv[2], "cancel") == 0) {
        ui_post_console(CON_SIM_SOS_CANCEL, (int32_t)a, 0, NULL);
    } else {
        ui_post_console(CON_SIM_SOS, (int32_t)a, 0, NULL);
    }
    return 0;
}

/* play TEXT -- render TEXT in Morse on the motors. */
static int cmd_play(int argc, char **argv)
{
    if (argc < 2) {
        printf("play TEXT   render TEXT in Morse on the motors\n");
        return 1;
    }
    char msg[MSG_MAX_CHARS + 1], clean[MSG_MAX_CHARS + 1];
    join_args(argc, argv, 1, msg, sizeof msg);
    morse_sanitize(msg, clean, sizeof clean);
    ui_post_console(CON_PLAY_TEXT, 0, 0, clean);
    return 0;
}

static const char *const TONE_NAMES[TONE__COUNT] = {"pip",   "send",  "command",  "cancel",    "ok",
                                                    "error", "degraded", "attention", "pending", "delivered",
                                                    "sos-armed", "sos-alarm", "quiet"};

/* tone NAME -- play one system tone. */
static int cmd_tone(int argc, char **argv)
{
    if (argc < 2) {
        printf("tone NAME:");
        for (int i = 0; i < TONE__COUNT; i++) {
            printf(" %s", TONE_NAMES[i]);
        }
        printf("\n");
        return 1;
    }
    for (int i = 0; i < TONE__COUNT; i++) {
        if (strcmp(argv[1], TONE_NAMES[i]) == 0) {
            ui_post_console(CON_TONE, i, 0, NULL);
            return 0;
        }
    }
    printf("unknown tone\n");
    return 1;
}

/* speed 1-9 -- reading speed. */
static int cmd_speed(int argc, char **argv)
{
    int q = argc >= 2 ? atoi(argv[1]) : 0;
    if (q < 1 || q > 9) {
        printf("speed 1-9   reading speed (5 = 200 ms unit)\n");
        return 1;
    }
    ui_post_console(CON_SPEED, q, 0, NULL);
    return 0;
}

/* haptic PARAM VALUE -- tune the dash sweep and intensities, or the
 * vibration strength (the motor supply, as I). */
static int cmd_haptic(int argc, char **argv)
{
    static const char *names[] = {"overlap", "split", "m1", "m2", "farnsworth", "gating", "strength", "dash"}; /* = HP_* */
    if (argc < 3) {
        printf("haptic overlap CENTI_U (30 = 0.3U together) | split CENTI_U (125: M1 1.4U, M2 1.9U) | m1 PCT | m2 PCT | "
               "farnsworth X10 (10 = none) | gating 0/1 | strength 1-9 (as I; 8 = full, 9 = full at 3.4 V) | "
               "dash 1/2 (as H: 1 sweep M1->M2, "
               "2 M2 only)\n");
        return 1;
    }
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
        if (strcmp(argv[1], names[i]) == 0) {
            ui_post_console(CON_HAPTIC, i, atoi(argv[2]), NULL);
            ui_post_console(CON_STATUS, 0, 0, NULL);
            return 0;
        }
    }
    printf("unknown parameter\n");
    return 1;
}

/* motor M1 M2 -- drive the motors directly (0-255 each). */
static int cmd_motor(int argc, char **argv)
{
    if (argc < 3) {
        printf("motor M1 M2   drive the motors directly, 0-255 each (motor 0 0 to stop)\n");
        return 1;
    }
    ui_post_console(CON_RAW, atoi(argv[1]), atoi(argv[2]), NULL);
    return 0;
}

/* peer add ID | peer del SLOT -- edit the contact table directly. */
static int cmd_peer(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "add") == 0) {
        char up[8] = {0};
        for (int i = 0; argv[2][i] && i < 7; i++) {
            up[i] = (char)toupper((unsigned char)argv[2][i]);
        }
        ui_post_console(CON_PEER_ADD, 0, 0, up);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "del") == 0) {
        ui_post_console(CON_PEER_DEL, atoi(argv[2]), 0, NULL);
        return 0;
    }
    printf("peer add ID | peer del SLOT\n");
    return 1;
}

/* on_off() -- shared parser for "<cmd> on|off" commands. */
static int on_off(int argc, char **argv, con_op_t op, const char *usage)
{
    if (argc < 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("%s on|off\n", usage);
        return 1;
    }
    ui_post_console(op, strcmp(argv[1], "on") == 0, 0, NULL);
    return 0;
}

/* loopback on|off -- a pretend partner instead of the radio (one board). */
static int cmd_loopback(int argc, char **argv)
{
    return on_off(argc, argv, CON_LOOPBACK, "loopback");
}

/* verbose on|off -- log keyed timings and everything played. */
static int cmd_verbose(int argc, char **argv)
{
    return on_off(argc, argv, CON_CAL_LOG, "verbose");
}

/*
 * radio [ping | power DBM | sf N | log on|off] -- radio status and bench tools.
 *   radio          status (also part of `status`)
 *   radio ping     every device in range answers with how well it heard us
 *                  ("PONG from ..." lines): the first two-board check, and
 *                  the range test
 *   radio power N  transmit power, 2..22 dBm (20 on an SX1276; kept across
 *                  restarts; the default is the chip's most)
 *   radio sf N     spreading factor 9..12, as G 1-4 on the device (kept);
 *                  set the other device the same, or they can't hear each other
 *   radio log on   log every packet sent and received, with RSSI and SNR
 */
static int cmd_radio(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "ping") == 0) {
        net_ping();
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "power") == 0) {
        ui_post_console(CON_TX_POWER, atoi(argv[2]), 0, NULL);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "sf") == 0) {
        ui_post_console(CON_SF, atoi(argv[2]), 0, NULL);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "log") == 0 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        net_set_log(strcmp(argv[2], "on") == 0);
        printf("packet log %s\n", argv[2]);
        return 0;
    }
    if (argc >= 2) {
        printf("radio | radio ping | radio power 2-22 | radio sf 9-12 | radio log on|off\n");
        return 1;
    }
    net_status_t s;
    net_get_status(&s);
    if (!s.radio_ok) {
        printf("radio: none: %s%s\n", radio_problem(), s.loopback ? " (loopback is on)" : "");
        return 0;
    }
    printf("radio: %s, SF%d, %d dBm (max %d)%s | duty cycle %.2f %% of the last hour (limit %d %%) | noise %d dBm\n",
           radio_chip_name(), s.sf, s.tx_dbm, s.tx_dbm_max, s.loopback ? " (idle: loopback is on)" : "",
           s.duty_used_ms / 36000.0, RADIO_DUTY_PERMILLE / 10, s.noise_dbm);
    printf("packets: sent %lu, received %lu, duplicates %lu, not ours %lu, damaged %lu\n", (unsigned long)s.pkts_tx,
           (unsigned long)s.pkts_rx, (unsigned long)s.pkts_dup, (unsigned long)s.pkts_bad, (unsigned long)s.pkts_crc);
    return 0;
}

/*
 * sim req ID | sim deaf on|off | sim disc -- loopback only.
 *   sim req ID     the pretend partner, as ID, asks to connect (try A / N
 *                  on one board)
 *   sim deaf on    the pretend partner goes "out of range": messages get no
 *                  receipt, the link is lost after 90 s; `off` brings it back
 *   sim disc       the pretend partner sweeps (D): its log line says whether
 *                  this device answered (it doesn't in silent mode, Z)
 */
static int cmd_sim(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "req") == 0) {
        uint32_t a = parse_id(argv[2]);
        if (a == ADDR_NONE) {
            printf("invalid ID\n");
            return 1;
        }
        net_sim_request(a);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "deaf") == 0 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        net_sim_deaf(strcmp(argv[2], "on") == 0);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "disc") == 0) {
        net_sim_discover();
        return 0;
    }
    printf("sim req ID | sim deaf on|off | sim disc   (loopback) drive the pretend partner\n");
    return 1;
}

/*
 * phone [pair | forget | on | off | sim on|off | send LINE] -- the owner's phone (BLE).
 *   phone           its status (also part of `status`)
 *   phone pair      as O: 60 s for a phone to pair; OK or error plays
 *   phone forget    forget the paired phone; BLE is off until the next O
 *   phone off / on  as W (v0.6.1): Bluetooth off (stored: it stays off through
 *                   restarts) / on again (needs a paired phone)
 *   phone sim on    pretend a phone is connected (no radio needed): every
 *                   line for it appears in the log as "phone << ..."
 *   phone send L    (with sim on) L as if the phone had written it, e.g.
 *                   phone send HI   or   phone send RL <base64 packet>
 */
static int cmd_phone(int argc, char **argv)
{
    if (argc == 1) {
        ui_post_console(CON_PHONE_STATUS, 0, 0, NULL);
        return 0;
    }
    if (strcmp(argv[1], "pair") == 0) {
        ui_post_console(CON_PHONE_PAIR, 0, 0, NULL);
        return 0;
    }
    if (strcmp(argv[1], "forget") == 0) {
        ui_post_console(CON_PHONE_FORGET, 0, 0, NULL);
        return 0;
    }
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
        ui_post_console(CON_PHONE_ONOFF, strcmp(argv[1], "on") == 0, 0, NULL);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "sim") == 0 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        ble_sim(strcmp(argv[2], "on") == 0);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "send") == 0) {
        char line[PHONE_LINE_MAX + 1];
        join_args(argc, argv, 2, line, sizeof line);
        ble_sim_line(line);
        return 0;
    }
    printf("phone | phone pair | phone forget | phone on|off | phone sim on|off | phone send LINE\n");
    return 1;
}

/* speed_of() -- the reading-speed digit (1-9) whose unit is `unit_ms`, or
 * 0 if none is (a unit set some other way). */
static int speed_of(unsigned unit_ms)
{
    for (int q = 1; q <= 9; q++) {
        if (hspeed_unit_ms(q) == unit_ms) {
            return q;
        }
    }
    return 0;
}

/*
 * dump_rundown() -- `test dump`: the last run-down test's log (one sample a
 * minute, kept in flash) as CSV, to copy from the serial monitor. Since
 * v0.5.1 (TEST_PLAN §14) the same layout for every run-down (v0.6.0 adds
 * sf=, also for a v0.5.1 log: 9):
 *   # morselink-rundown board=DEA peer=ABC test=rundown-m ... samples=612
 *   # (a line saying what the columns mean and how the run ended)
 *   minute,mv,charging,m1_ms,m2_ms
 * m1_ms / m2_ms: how long each motor output was on in that minute (in
 * 250 ms steps; 0 in run-downs that play nothing). A log recorded by older
 * firmware has no details stored: the header says so and the motor columns
 * are 0 (nothing played in those runs).
 */
static void dump_rundown(void)
{
    uint8_t test;
    uint32_t total;
    if (!store_rd_info(&test, &total)) {
        printf("no run-down log stored\n");
        return;
    }
    static rd_meta_t m; /* (console task only; static keeps it off the stack) */
    bool meta = store_rd_meta(&m);
    if (meta) {
        unsigned unit = m.unit_lo | ((unsigned)m.unit_hi << 8);
        printf("# morselink-rundown board=%s peer=%s test=%s paths=%s role=%s session=%s fw=%s tx_dbm=%d sf=%u speed=%d "
               "unit_ms=%u m1_pct=%u m2_pct=%u motor_log=%u end=%s samples=%lu\n",
               m.own, m.peer, tt_test_name(m.test), tt_paths_name(m.paths), tt_role_name(m.role), m.session, m.fw,
               m.tx_dbm, m.sf, speed_of(unit), unit, m.m1_pct, m.m2_pct, m.motors, m.end[0] ? m.end : "none",
               (unsigned long)total);
        printf("# one sample a minute; m1_ms/m2_ms = motor on-time in that minute; ended: %s\n",
               m.end[0] ? m.end : "never stopped (battery ran out, the board restarted, or still running)");
    } else {
        char id[ID_MAX_LEN + 1] = "";
        if (!store_load_id(id)) {
            id[0] = '\0'; /* no ID set: "-" below */
        }
        printf("# morselink-rundown board=%s test=%s fw=older end=unknown samples=%lu\n", id[0] ? id : "-",
               tt_test_name(test), (unsigned long)total);
        printf("# recorded by firmware older than v0.5.1: no details stored; nothing played, so the motor columns "
               "are 0\n");
    }
    printf("minute,mv,charging,m1_ms,m2_ms\n");
    static uint16_t buf[RD_BLOCK];    /* (console task only) */
    static uint8_t mot[2 * RD_BLOCK]; /* this hour's motor on-time, if kept */
    uint32_t n_all = 0;
    for (int b = 0; n_all < total; b++) {
        int n = store_rd_load(b, buf);
        int nm = (meta && m.motors) ? store_rd_load_motor(b, mot) : 0;
        for (int i = 0; i < n && n_all < total; i++, n_all++) {
            unsigned m1 = i < nm ? (unsigned)mot[2 * i] * RD_MOTOR_UNIT_MS : 0;
            unsigned m2 = i < nm ? (unsigned)mot[2 * i + 1] * RD_MOTOR_UNIT_MS : 0;
            printf("%lu,%u,%u,%u,%u\n", (unsigned long)n_all, (unsigned)(buf[i] & 0x7FFF), (unsigned)(buf[i] >> 15),
                   m1, m2);
        }
        if (n < RD_BLOCK) {
            break; /* the last (partly filled) hour */
        }
    }
}

/*
 * test start TEST [PATHS] [ROLE] | test start key=value... | test stop | test dump
 *   (docs/TEST_PLAN.md; the phone app normally starts and stops tests)
 *   test start dynamic both rover   a test from the console, e.g. on the bench
 *                                   (loopback may be on: then the pretend partner answers)
 *   test start test=baseline paths=lora role=base session=S seed=N   the full form
 *   test stop                       end it (the STOP line follows)
 *   test dump                       the last run-down's battery log, as CSV (header line first)
 *   test start rundown-m            the motor night: PARIS on the motors until stopped (TEST_PLAN §14)
 *   test start rundown-l            LoRa traffic only, Bluetooth off (v0.6.0)
 *   test start point                a point test with the default sweep (v0.6.1); the full form takes
 *                                   plan=9:17,12:22 (SF:dBm steps) and n=1..20 (messages a step)
 */
static int cmd_test(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "start") == 0) {
        char args[TT_ARGS_MAX + 1];
        if (strchr(argv[2], '=')) {
            join_args(argc, argv, 2, args, sizeof args);
        } else {
            snprintf(args, sizeof args, "test=%s paths=%s role=%s session=console seed=1", argv[2],
                     argc >= 4 ? argv[3] : "both", argc >= 5 ? argv[4] : "rover");
        }
        ui_post_console(CON_TEST_START, 0, 0, args);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        ui_post_console(CON_TEST_STOP, 0, 0, NULL);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        dump_rundown();
        return 0;
    }
    printf("test start baseline|dynamic|rundown-a|rundown-b|rundown-m|rundown-l|point [both|lora|relay] [rover|base] | "
           "test start test=point plan=9:17,9:22,12:22 [n=20] role=rover | test stop | test dump\n");
    return 1;
}

/*
 * bench power | bench motor [m1|m2] | bench stop -- the power and motor
 * measurements (bench.h; docs/TEST_PLAN.md §14.1, §14.3, §17).
 *   bench power        the board steps through its states by itself, a
 *                      minute or so each, for a multimeter in series with
 *                      the battery: unplug USB once it has printed the plan
 *                      (it waits for that, however long; the phone app can
 *                      also start it, with no USB at all: Tests > Meter session)
 *   bench motor [m1]   one motor plays the phyphox pattern (about 4 min)
 *   bench stop         end either early (a 5 s hold ends bench power too)
 */
static int cmd_bench(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "power") == 0) {
        ui_post_console(CON_BENCH, 1, 0, NULL);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "motor") == 0) {
        int m = (argc >= 3 && (strcmp(argv[2], "m2") == 0 || strcmp(argv[2], "2") == 0)) ? 2 : 1;
        ui_post_console(CON_BENCH, 2, m, NULL);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        ui_post_console(CON_BENCH, 0, 0, NULL);
        return 0;
    }
    printf("bench power | bench motor [m1|m2] | bench stop   (TEST_PLAN §14: power by meter, motor by phone)\n");
    return 1;
}

/* calibrate -- run the PARIS calibration again. */
static int cmd_calibrate(int argc, char **argv)
{
    ui_post_console(CON_CALIBRATE, 0, 0, NULL);
    return 0;
}

/* factory-reset yes -- erase settings, contacts, calibration and the paired phone. */
static int cmd_factory(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        printf("factory-reset yes   erases settings, contacts, calibration and the paired phone (keeps the device ID)\n");
        return 1;
    }
    ui_post_console(CON_FACTORY, 0, 0, NULL);
    return 0;
}

/* off -- switch the device off, exactly as F does (power.h): it then
 * sleeps until the key is held for 2 s. Refused while our SOS is active. */
static int cmd_off(int argc, char **argv)
{
    ui_post_console(CON_POWER_OFF, 0, 0, NULL);
    return 0;
}

/* restart -- reboot the board. */
static int cmd_restart(int argc, char **argv)
{
    esp_restart();
    return 0;
}

/* ------------------------------------------------------------------------- */

/* console_start() -- start the key-injection task and the REPL on UART0. */
esp_err_t console_start(void)
{
    s_inj_q = xQueueCreate(2, sizeof(inj_t));
    xTaskCreatePinnedToCore(inject_task, "inject", 3072, NULL, 9, NULL, 1);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "morselink>";
    repl_cfg.max_cmdline_length = 256;
    repl_cfg.task_stack_size = 6144;
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        return err;
    }

    const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "Show state, link, radio, calibration, contacts, battery", .func = cmd_status},
        {.command = "id", .help = "id [XXX]  show or set this device's ID", .func = cmd_id},
        {.command = "key", .help = "key [-u ms] PATTERN  inject keying (. - space / S C X W)", .func = cmd_key},
        {.command = "text", .help = "text [-s] MESSAGE  key a message (-s: and send it)", .func = cmd_text},
        {.command = "rx", .help = "rx ID MESSAGE  simulate an incoming message", .func = cmd_rx},
        {.command = "sos", .help = "sos ID [cancel]  simulate an incoming SOS", .func = cmd_sos},
        {.command = "play", .help = "play TEXT  render text in Morse on the motors", .func = cmd_play},
        {.command = "tone", .help = "tone NAME  play a system tone", .func = cmd_tone},
        {.command = "speed", .help = "speed 1-9  reading speed", .func = cmd_speed},
        {.command = "haptic", .help = "haptic PARAM VALUE  tune the dash sweep / intensity / strength", .func = cmd_haptic},
        {.command = "motor", .help = "motor M1 M2  drive motors directly (0-255)", .func = cmd_motor},
        {.command = "peer", .help = "peer add ID | peer del SLOT", .func = cmd_peer},
        {.command = "radio", .help = "radio [ping | power DBM | sf 9-12 | log on|off]  radio status, settings and range test", .func = cmd_radio},
        {.command = "loopback", .help = "loopback on|off  a pretend partner instead of the radio (one board)", .func = cmd_loopback},
        {.command = "sim", .help = "sim req ID | sim deaf on|off | sim disc  (loopback) drive the pretend partner", .func = cmd_sim},
        {.command = "phone", .help = "phone [pair | forget | on | off | sim on|off | send LINE]  the owner's phone (BLE)", .func = cmd_phone},
        {.command = "test", .help = "test start TEST [PATHS] [ROLE] | test start key=value... | test stop | test dump  test mode (TEST_PLAN.md)", .func = cmd_test},
        {.command = "bench", .help = "bench power | bench motor [m1|m2] | bench stop  power and motor measurements (TEST_PLAN §14)", .func = cmd_bench},
        {.command = "verbose", .help = "verbose on|off  log keyed timings and everything played on the motors", .func = cmd_verbose},
        {.command = "calibrate", .help = "re-run the PARIS calibration", .func = cmd_calibrate},
        {.command = "factory-reset", .help = "factory-reset yes  erase everything but the ID", .func = cmd_factory},
        {.command = "off", .help = "switch off (as F does); hold the key 2 s to switch on", .func = cmd_off},
        {.command = "restart", .help = "reboot", .func = cmd_restart},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    esp_console_register_help_command();
    return esp_console_start_repl(repl);
}
