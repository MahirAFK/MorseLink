/*
 * MorseLink — user interaction state machine (see ui.h).
 *
 * Rules implemented here (spec + decisions agreed in conversation):
 *  - Holds: 2 s send/execute (pip), 3 s command mode (pip pip), 5 s cancel
 *    everything (long). Releasing between 1.5 s and 2 s does nothing.
 *  - A press during any playback stops it and also starts keying, so SOS can
 *    always be keyed. A lone tap that only stopped a playback (one element,
 *    then a word gap) is dropped.
 *  - The target ID is announced after the send hold and before the message
 *    leaves; a press during the announcement keeps the draft and cancels
 *    the send. (Moved from "before the first character" so that keying from
 *    idle -- including SOS -- never has to wait for a wake-up tap.)
 *  - Command arguments are echoed before execution; a press aborts.
 *  - SOS: recognised anywhere; confirmed by 3 s of silence (general) or 2 s
 *    after a slot/ID (targeted); then an echo of the target and three long
 *    buzzes, during which a press aborts. An unreadable target falls back to
 *    a general SOS. Resent every 30 s for 10 min, then every 2 min, until a
 *    5 s hold cancels it (which also sends an SOS-cancel).
 *  - Incoming alerts queue and play once the key has rested (3 s, or 10 s
 *    while a draft exists). SOS alarms interrupt everything.
 *  - A received message plays attention + sender ID + text, except in an
 *    active conversation (last exchange with that device at most 5 min ago,
 *    connecting included), where the text plays alone (convo.h).
 *  - F switches the device off (power.h); holding the key 2 s switches it
 *    on again. Refused while our own SOS is active.
 *  - D (discovery, agreed 13 Oct): about 4 s after the echo, the devices
 *    that answered play one at a time, near first: ID + N / M / F. Tap (E)
 *    invites: the ID is echoed (a press goes back to the list), then the
 *    request goes out exactly as S <ID> would send it, and the list ends
 *    (only one request can be open). Double tap (I) offers the next one;
 *    after the last the list ends with the cancel tone. A 2 s hold stops
 *    the list, 3 s goes to command mode, 5 s cancels everything; anything
 *    else keyed gives error + the same entry again; 30 s untouched ends it.
 *    Nobody answered: error + 0. SOS works throughout.
 *  - The owner's phone (BLE, ble.h; lines in phone.h): O opens a 60 s window
 *    for it to pair (OK when it has, error if none came). While it is
 *    connected it gets the live draft (DR), every message sent (TX, then OK
 *    or NO) and received (RX), the link and battery (ST) and events (EV);
 *    the network relays packets through it (net.c).
 *  - Test mode (docs/TEST_PLAN.md; the traffic itself is ntest.c in the net
 *    task): started from the phone (TEST START) or the console (`test
 *    start`). While it runs nothing plays (no haptics, no LED), queued
 *    alerts are dropped, incoming SOS doesn't alarm, and the key is ignored
 *    except a 5 s hold, which stops the test. The cancel tone says it's over.
 *
 * Linking (spec §7; the protocol itself is in link.c, run by net.c):
 *  - S / P ask the other device to connect; nothing plays until the answer:
 *    OK + ID (a new contact: OK + slot digit), error + BUSY, error + NO, or
 *    error + ID (unavailable). The link only moves when they accept.
 *  - An incoming request plays attention + ID + "decision pending"; A [n]
 *    accepts and N [n] declines (and blocks a stranger). Their echo plays
 *    the requester's ID.
 *  - Messages: after the announcement the message leaves; OK plays when its
 *    silent receipt arrives. No receipt: error + ID and the text goes back
 *    into the draft (in front of anything keyed since).
 *  - Connection lost / ended: error + ID; restored: OK + ID. Sending while
 *    lost or ended: error + ID, draft kept. Since v0.6.0 (agreed 15 Oct) the
 *    link is lost only when no path works (LoRa nor the phones' relay): LoRa
 *    alone dropping while the relay carries plays nothing (the phone shows
 *    it); a loss because a message got no receipt plays no second alert
 *    (the failure's error + ID said it).
 *  - G [1-4] (agreed 15 Oct): the range level, SF9 / SF10 / SF11 / SF12,
 *    stored; G alone plays the current level. Both devices must be set
 *    alike by hand (the manual says so).
 *  - W (agreed 20 Oct): Bluetooth off / on, OK + OFF / ON like Z, stored
 *    (off stays off through restarts until W again; O clears it too). Off
 *    tells the phone first (EV BLE_OFF), so the app knows why it went.
 *  - Bench power (the meter session, bench.h): from the console or the
 *    phone (BENCH POWER); the board steps through its states by itself, M1
 *    buzzing each step's number, the phone told each step (BN lines).
 *    Nothing plays meanwhile and the key only counts for a 5 s hold.
 *  - X on a contact also tells them (UNPAIR); a device that unpairs us is
 *    removed from the contacts.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "ble.h"
#include "board.h"
#include "bench.h"
#include "calib.h"
#include "convo.h"
#include "decoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "haptics.h"
#include "hseq.h"
#include "morse.h"
#include "net.h"
#include "peers.h"
#include "phone.h"
#include "radio.h" /* radio_chip_name() */
#include "pmic.h"
#include "power.h"
#include "store.h"
#include "ttest.h"

static const char *TAG = "ui";

/* ------------------------------------------------------------------------- */
/* Events                                                                     */
/* ------------------------------------------------------------------------- */

/* What can arrive in the UI task's queue. */
typedef enum {
    UIE_KEY = 0,   /* key down / up / hold threshold (keyer task) */
    UIE_PLAY_DONE, /* a playback finished (haptics player) */
    UIE_NET,       /* the network reports something (net task) */
    UIE_CONSOLE,   /* a console command (console task) */
    UIE_PHONE,     /* the BLE side reports something (uip_t) */
    UIE_TEST,      /* the net task: test mode on / off */
} uie_kind_t;

typedef struct {
    uint8_t kind; /* uie_kind_t */
    union {
        key_evt_t key;
        uint32_t tag; /* UIE_PLAY_DONE: which playback */
        uint8_t phone; /* UIE_PHONE: uip_t */
        struct {
            uint8_t on;   /* UIE_TEST: 1 = test mode on */
            uint8_t test; /* ... which test (tt_test_t) */
        } test;
        net_evt_t net;
        struct {
            uint8_t op; /* con_op_t */
            int32_t a, b;
            char text[TT_ARGS_MAX + 1]; /* (TT_ARGS_MAX >= MSG_MAX_CHARS: a test's parameters are the longest) */
        } con;
    };
} ui_evt_t;

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

typedef enum {
    ST_CALIB = 0,     /* PARIS calibration (first use or C) */
    ST_IDLE,          /* nothing keyed */
    ST_COMPOSE,       /* a draft exists */
    ST_COMMAND,       /* command mode: keying a command letter and argument */
    ST_CMD_ECHO,      /* the command's argument is being echoed (press aborts) */
    ST_SEND_ANNOUNCE, /* the recipient's ID is being announced (press aborts) */
    ST_SOS_ENTRY,     /* SOS keyed: waiting for a target or the confirming silence */
    ST_SOS_ARMED,     /* echo + three long buzzes before an SOS leaves (press aborts) */
    ST_PLAYBACK,      /* a result or alert is playing; afterwards back to idle/compose */
    ST_SOS_ALARM,     /* someone's SOS: alarm until a tap acknowledges it */
    ST_DISC_SCAN,     /* D: waiting for the sweep's answers (about 4 s) */
    ST_DISC_WALK,     /* D: an entry of the list is playing, or waits for tap / double tap */
    ST_DISC_INVITE,   /* D: the invited device's ID is being echoed (press: back to the list) */
} ui_state_t;

static const char *const ST_NAMES[] = {"calibrating", "idle",      "composing", "command",     "echo",
                                       "announce",    "sos-entry", "sos-armed", "playback",    "sos-alarm",
                                       "discovering", "disc-list", "disc-invite"};

/* Queued alerts, played when the key has rested. */
typedef enum {
    AL_MSG = 0,    /* attention + sender ID + text (the text alone if `bare`) */
    AL_TONE_ID,    /* tone + ID (e.g. error + ID: unavailable / not delivered) */
    AL_BATTERY,    /* attention + percentage */
    AL_DEGRADED,   /* keying degraded */
    AL_TONE,       /* a tone alone (OK: message delivered) */
    AL_TONE_TEXT,  /* tone + text (error + BUSY / NO, OK + slot digit) */
    AL_REQUEST,    /* attention + requester ID + decision pending */
    AL_LINK,       /* tone + ID about the link (lost / ended / restored) */
} al_kind_t;

typedef struct {
    uint8_t kind; /* al_kind_t */
    uint8_t tone; /* tone_t, for the kinds with a tone */
    uint8_t bare; /* AL_MSG: part of an active conversation, so no attention + ID */
    uint32_t addr;
    int num;      /* AL_BATTERY: percentage */
    char text[MSG_MAX_CHARS + 1];
} alert_t;

/* One received message, for R1..R9. */
typedef struct {
    uint32_t src;
    char text[MSG_MAX_CHARS + 1];
} hist_t;

#define CMD_MAX 8         /* command letter + argument */
#define SOS_ARG_MAX 4     /* SOS target: slot digit or ID */
#define CAL_CAPTURE_MAX 20
#define ACKED_MAX 4       /* SOS senders we acknowledged / responders to ours */

static struct {
    QueueHandle_t q;
    /* persisted */
    settings_t set;
    peer_table_t peers;
    calib_t cal;
    char own_id[ID_MAX_LEN + 1];
    /* keying */
    decoder_t dec;
    bool cal_log;     /* verbose: log keyed timings and everything played */
    bool cal_dirty;
    int64_t cal_last_save;
    bool key_down;
    bool swallow;     /* this press was used up (e.g. acknowledging an alarm) */
    bool tent_armed;  /* press that stopped a playback: its first character is tentative */
    bool tent_active;
    int tent_pos;
    int64_t last_key;
    /* state machine */
    ui_state_t st;
    char draft[MSG_MAX_CHARS + 1];
    int draft_len;
    bool draft_kept; /* K (v0.6.4): the draft isn't discarded after 60 s; cleared once the draft is empty */
    char cmd[CMD_MAX + 1];
    int cmd_len;
    char exec[CMD_MAX + 1]; /* the command being echoed, executed when the echo ends */
    char sos_arg[SOS_ARG_MAX + 1];
    int sos_arg_len;
    int64_t sos_ref;
    /* playback */
    bool playing;
    uint32_t play_tag, next_tag;
    int64_t play_started, last_play_end;
    /* calibration capture */
    uint8_t cal_first_use;
    bool cal_capturing;
    float cal_el[CAL_CAPTURE_MAX];
    float cal_gap[CAL_CAPTURE_MAX];
    int cal_n;
    int64_t cal_last_up;
    /* the link, as the user sees it (net.c's engine is authoritative) */
    uint32_t link_addr;
    uint8_t link_state;          /* link_state_t (link.h) */
    uint32_t inreq[LINK_PENDING_MAX]; /* requests played to the user, oldest first */
    int n_inreq;
    uint32_t req_addr;           /* our latest S / P target, for its feedback */
    char req_cmd;                /* 'S' or 'P' */
    convo_t convo;               /* who we last exchanged something with, and when (convo.h) */
    /* alerts and history */
    alert_t alerts[ALERT_QUEUE_LEN];
    int al_head, al_n;
    hist_t hist[HISTORY_LEN];
    int hist_head, hist_n;
    /* SOS, sending side */
    bool sos_active;
    uint32_t sos_target;
    int64_t sos_started, sos_next;
    uint32_t sos_responders[ACKED_MAX];
    int sos_n_resp;
    /* SOS, receiving side */
    uint32_t alarm_src;
    int64_t alarm_next;
    uint32_t alarm_pending;
    uint32_t acked[ACKED_MAX];
    int n_acked;
    /* the motor supply (vibration strength, I) as the power chip has it, mV (0: unknown / no chip) */
    int motor_mv;
    /* battery */
    int bat_pct, bat_mv;
    bool bat_charging, bat_warned1, bat_warned2;
    int64_t bat_next;
    /* discovery (D): the list being walked */
    disc_entry_t disc[DISC_MAX]; /* the devices found, in walking order (near first) */
    int disc_n;                  /* how many; 0 = no list */
    int disc_pos;                /* the one being offered */
    int64_t disc_started;        /* when D was executed (safety timeout while scanning) */
    bool disc_stop_tap;          /* a press stopped the invite's echo: if it was a lone tap, it only meant "stop" */
    /* the phone (phone.h): what it was last told */
    bool ph_hello;                      /* it has had its HI: other lines may follow */
    char ph_status[PHONE_LINE_MAX + 1]; /* the last ST line sent ("" = send again) */
    char ph_draft[MSG_MAX_CHARS + 1];   /* the draft as last sent in DR */
    bool ph_draft_sent;                 /* ph_draft is valid */
    phone_tx_t ph_tx;                   /* messages announced with TX, until their OK / NO */
    char ph_radio[24];                  /* the last RD line sent ("" = send again) */
    bool ph_cm_on;                      /* the phone has been told we are in command mode (CM) */
    char ph_cm[CMD_MAX + 1];            /* ... with this text */
    /* the learning view (protocol 4, v0.6.2) */
    bool ph_ky_open;                    /* the phone was last told something is open (KY with a pattern) */
    char ph_speeds[96];                 /* the last SP line sent ("" = send again) */
    int64_t ph_speeds_at;               /* ... and when */
    bool ph_pl_on;                      /* the phone was told a playback with Morse is on (PL), not yet ended */
    /* test mode (ntest.c runs the traffic; here: silence and the key) */
    bool test;          /* a test runs: nothing plays, the key only stops it */
    uint8_t test_kind;  /* which (tt_test_t) */
    int64_t test_since; /* when it started */
    bool test_ble_off;  /* rundown-a switched Bluetooth off: on again afterwards */
    /* bench power (bench.h): the steps run here; nothing plays meanwhile */
    bool bench;              /* bench power is running */
    uint8_t bench_phase;     /* bench_phase_t below */
    int bench_i;             /* the step (index into BENCH_POWER) */
    int bench_last_step;     /* the number of the step before (a part of the same step gets a shorter signal) */
    int bench_edges;         /* the signal: on/off changes of motor M1 still to come */
    bool bench_led;          /* ... M1 is buzzing now (the name is from v0.6.0's LED signal) */
    int64_t bench_until;     /* when the current phase (or blink) ends */
    bench_motor_t bench_mot; /* bench motor's pattern (it plays as an ordinary playback) */
} U;

/* bench power's phases. */
typedef enum {
    BPH_WAIT_USB = 0, /* waiting for USB to be unplugged (the meter then sees the battery's current) */
    BPH_SETTLE,       /* a few seconds for the meter to settle */
    BPH_SIGNAL,       /* M1 buzzes the step's number, then a still pause */
    BPH_RUN,          /* the step itself: nothing buzzes (except in step 5, the motors' own) */
} bench_phase_t;

/* now_ms() -- milliseconds since boot. */
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* id_of() -- printable ID for an address: "ALL" for broadcast, "?" if not
 * an ID. Writes into buf and returns it, so it can be used inline. */
static const char *id_of(uint32_t addr, char buf[ID_MAX_LEN + 1])
{
    if (addr == ADDR_BROADCAST) {
        strcpy(buf, "ALL");
    } else if (!addr_to_id(addr, buf)) {
        strcpy(buf, "?");
    }
    return buf;
}

/* own_addr() -- this device's address, or ADDR_NONE if no ID is set. */
static uint32_t own_addr(void)
{
    return U.own_id[0] ? id_to_addr(U.own_id) : ADDR_NONE;
}

/* ------------------------------------------------------------------------- */
/* Playback helpers                                                           */
/* ------------------------------------------------------------------------- */

/* log_job() -- verbose mode: log what is about to play, e.g.
 * play: [attention] "K7Q" "HI"  4.2 s (the LED is hard to read). */
static void log_job(const hjob_t *j)
{
    static const char *const names[TONE__COUNT] = {"pip",      "send-armed", "cmd-armed", "cancel",   "ok",
                                                   "error",    "degraded",   "attention", "pending",  "delivered",
                                                   "sos-armed", "sos-alarm", "quiet-pip"};
    char line[200];
    int n = 0;
    line[0] = '\0';
    for (int i = 0; i < j->n && n < (int)sizeof(line) - 24; i++) {
        const hitem_t *it = &j->items[i];
        if (it->kind == HI_TONE) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " [%s]", names[it->tone < TONE__COUNT ? it->tone : 0]);
        } else if (it->kind == HI_MORSE) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " \"%.60s\"", &j->text[it->arg]);
        } else {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " (%u ms)", it->arg);
        }
    }
    hparams_t p;
    haptics_get_params(&p);
    ESP_LOGI(TAG, "play:%s  %.1f s", line, hjob_duration_ms(j, &p) / 1000.0);
}

static void phone_play_start(const hjob_t *j);
static void phone_play_end(void);

/* play_job() -- start playing a job on the motors (replacing anything
 * playing). Its end comes back as UIE_PLAY_DONE with this job's tag. The
 * phone hears what Morse is in it (PL lines), for its "now playing" view. */
static void play_job(const hjob_t *j)
{
    if (U.test) {
        ESP_LOGW(TAG, "test mode: nothing plays");
        return;
    }
    if (U.bench) {
        return; /* bench power: a motor would spoil the current readings */
    }
    if (U.cal_log) {
        log_job(j);
    }
    U.play_tag = ++U.next_tag;
    U.playing = true;
    U.play_started = now_ms();
    haptics_play(j, U.play_tag);
    phone_play_start(j);
}

/* play_tone() -- play one system tone; the state is left as it is. */
static void play_tone(tone_t t)
{
    hjob_t j;
    hjob_init(&j);
    hjob_tone(&j, t);
    play_job(&j);
}

/* stop_playback() -- cut the current playback short (a key press). */
static void stop_playback(void)
{
    if (U.playing) {
        haptics_stop();
        U.playing = false;
        U.last_play_end = now_ms();
        phone_play_end(); /* the phone's "now playing" view stops too */
    }
}

/* resume_state() -- where to go when nothing special is happening. */
static ui_state_t resume_state(void)
{
    return U.draft_len > 0 ? ST_COMPOSE : ST_IDLE;
}

/* play_result() -- play a result or alert, then return to idle/compose. */
static void play_result(const hjob_t *j)
{
    U.st = ST_PLAYBACK;
    play_job(j);
}

/* play_result_tone() -- one tone as a result; afterwards back to idle/compose. */
static void play_result_tone(tone_t t)
{
    hjob_t j;
    hjob_init(&j);
    hjob_tone(&j, t);
    play_result(&j);
}

/* apply_haptic_params() -- push the stored reading speed and dash/intensity
 * settings to the player. */
static void apply_haptic_params(void)
{
    hparams_t p;
    hparams_default(&p);
    p.unit_ms = hspeed_unit_ms(U.set.speed_q);
    p.overlap_cu = U.set.overlap_cu;
    p.dash_m1_cu = U.set.dash_m1_cu;
    p.farnsworth_x10 = U.set.farnsworth_x10;
    p.dash_style = U.set.dash_style == HDASH_M2 ? HDASH_M2 : HDASH_SWEEP; /* H (v0.6.3) */
    p.drive_pct = hstrength_drive_pct(U.set.strength); /* I (v0.6.4): 0 = never set counts as the default */
    p.kick_ms = HAPTIC_KICK_MS;
    haptics_set_params(&p);
    haptics_set_intensity(U.set.m1_pct, U.set.m2_pct);
    haptics_set_rail_gating(U.set.rail_gating);
}

/*
 * apply_strength() -- the stored vibration strength (I; 1-9 since v0.6.4):
 * the drive (PWM, through apply_haptic_params()) and the motor supply (the
 * power chip's DCDC1: 3.4 V at I9, 3.3 V below). Until a level has been
 * chosen (strength 0) DCDC1 is left as the board starts it (3.3 V, = I8).
 * Without a power chip the drive still changes (I9 then = I8). Refreshes
 * U.motor_mv from the chip. Returns false if the chip refused the voltage.
 */
static bool apply_strength(void)
{
    bool ok = true;
    apply_haptic_params();
    if (U.set.strength && pmic_present()) {
        esp_err_t err = pmic_set_motor_mv(hstrength_mv(U.set.strength));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "vibration strength: the power chip didn't take %d mV (%s)", hstrength_mv(U.set.strength),
                     esp_err_to_name(err));
            ok = false;
        }
    }
    U.motor_mv = pmic_motor_mv();
    return ok;
}

/* save_settings() -- write U.set to flash (callers only do it when something changed). */
static void save_settings(void)
{
    store_save_settings(&U.set);
}

/* ------------------------------------------------------------------------- */
/* Alerts and history                                                         */
/* ------------------------------------------------------------------------- */

/* alert_same() -- would these two alerts play exactly the same thing? */
static bool alert_same(const alert_t *a, const alert_t *b)
{
    return a->kind == b->kind && a->tone == b->tone && a->addr == b->addr && a->num == b->num &&
           strcmp(a->text, b->text) == 0;
}

/*
 * alert_push() -- queue an alert, at the back (messages, requests) or the
 * front (results the user is waiting for, SOS feedback). An alert identical
 * to one already waiting is not queued twice (e.g. "OK + K7Q" for restored
 * and for accepted). When the queue is full the oldest alert is dropped.
 */
static void alert_push(const alert_t *a, bool front)
{
    if (U.test || U.bench) {
        return; /* test mode, bench power: alerts are dropped, not saved for later */
    }
    for (int i = 0; i < U.al_n; i++) {
        if (a->kind != AL_MSG && alert_same(&U.alerts[(U.al_head + i) % ALERT_QUEUE_LEN], a)) {
            return;
        }
    }
    if (U.al_n == ALERT_QUEUE_LEN) {
        U.al_head = (U.al_head + 1) % ALERT_QUEUE_LEN;
        U.al_n--;
    }
    if (front) {
        U.al_head = (U.al_head + ALERT_QUEUE_LEN - 1) % ALERT_QUEUE_LEN;
        U.alerts[U.al_head] = *a;
    } else {
        U.alerts[(U.al_head + U.al_n) % ALERT_QUEUE_LEN] = *a;
    }
    U.al_n++;
}

/* alert_remove() -- drop waiting alerts of `kind` about `addr` (any tone if
 * tone < 0). Returns how many were dropped. */
static int alert_remove(uint8_t kind, int tone, uint32_t addr)
{
    int removed = 0, w = 0;
    static alert_t keep[ALERT_QUEUE_LEN]; /* ~900 bytes: off the UI task's stack (only that task calls this) */
    for (int i = 0; i < U.al_n; i++) {
        const alert_t *a = &U.alerts[(U.al_head + i) % ALERT_QUEUE_LEN];
        if (a->kind == kind && a->addr == addr && (tone < 0 || a->tone == tone)) {
            removed++;
        } else {
            keep[w++] = *a;
        }
    }
    if (removed) {
        memcpy(U.alerts, keep, sizeof(alert_t) * (size_t)w);
        U.al_head = 0;
        U.al_n = w;
    }
    return removed;
}

/* Shorthands for the common alert shapes. */
static void alert_tone_id(tone_t t, uint32_t addr, bool front)
{
    alert_t a = {.kind = AL_TONE_ID, .tone = (uint8_t)t, .addr = addr};
    alert_push(&a, front);
}

/* alert_tone_text() -- queue tone + Morse text (error + BUSY / NO, OK + slot digit). */
static void alert_tone_text(tone_t t, const char *text, bool front)
{
    alert_t a = {.kind = AL_TONE_TEXT, .tone = (uint8_t)t};
    strncpy(a.text, text, MSG_MAX_CHARS);
    alert_push(&a, front);
}

/* alert_link() -- queue tone + ID about the link (lost / restored / ended), at the front. */
static void alert_link(tone_t t, uint32_t addr)
{
    alert_t a = {.kind = AL_LINK, .tone = (uint8_t)t, .addr = addr};
    alert_push(&a, true);
}

/* play_alert() -- turn a queued alert into a playback (spec §9 sequences). */
static void play_alert(const alert_t *a)
{
    hjob_t j;
    char id[ID_MAX_LEN + 1], n[8];
    hjob_init(&j);
    switch (a->kind) {
    case AL_MSG:
        if (!a->bare) {
            hjob_tone(&j, TONE_ATTENTION);
            hjob_morse(&j, id_of(a->addr, id));
        }
        hjob_morse(&j, a->text);
        break;
    case AL_TONE_ID:
    case AL_LINK:
        hjob_tone(&j, (tone_t)a->tone);
        hjob_morse(&j, id_of(a->addr, id));
        break;
    case AL_BATTERY:
        hjob_tone(&j, TONE_ATTENTION);
        snprintf(n, sizeof n, "%d", a->num);
        hjob_morse(&j, n);
        break;
    case AL_DEGRADED:
        hjob_tone(&j, TONE_DEGRADED);
        break;
    case AL_TONE:
        hjob_tone(&j, (tone_t)a->tone);
        break;
    case AL_TONE_TEXT:
        hjob_tone(&j, (tone_t)a->tone);
        hjob_morse(&j, a->text);
        break;
    case AL_REQUEST:
        hjob_tone(&j, TONE_ATTENTION);
        hjob_morse(&j, id_of(a->addr, id));
        hjob_tone(&j, TONE_PENDING);
        break;
    }
    play_result(&j);
}

/* hist_add() -- remember a received message for R (newest is R1). */
static void hist_add(uint32_t src, const char *text)
{
    U.hist_head = (U.hist_head + HISTORY_LEN - 1) % HISTORY_LEN;
    U.hist[U.hist_head].src = src;
    strncpy(U.hist[U.hist_head].text, text, MSG_MAX_CHARS);
    U.hist[U.hist_head].text[MSG_MAX_CHARS] = '\0';
    if (U.hist_n < HISTORY_LEN) {
        U.hist_n++;
    }
}

/* hist_get() -- the n-th most recent message (1 = newest), or NULL. */
static const hist_t *hist_get(int n)
{
    if (n < 1 || n > U.hist_n) {
        return NULL;
    }
    return &U.hist[(U.hist_head + n - 1) % HISTORY_LEN];
}

/* ------------------------------------------------------------------------- */
/* The link, as the user sees it                                              */
/* ------------------------------------------------------------------------- */

/*
 * link_mirror() -- record the link as the network reports it, and persist
 * the partner's address (and whether the link ended, so that a restart
 * doesn't try to reconnect a partner who moved on). "Lost" is not stored:
 * after a restart the link is tried again.
 */
static void link_mirror(uint32_t addr, uint8_t state)
{
    U.link_addr = addr;
    U.link_state = addr == ADDR_NONE ? LS_NONE : state;
    uint8_t ended = U.link_state == LS_ENDED;
    if (U.set.link_addr != addr || U.set.link_ended != ended) {
        U.set.link_addr = addr;
        U.set.link_ended = ended;
        save_settings();
    }
}

/* link_select() -- change the link without asking the other side: SOS
 * acknowledgements (spec §10) and the console's `peer del`. */
static void link_select(uint32_t addr)
{
    net_set_link(addr);
    link_mirror(addr, LS_UP);
}

/* push_blocklist() -- give the network the user's current blocklist. */
static void push_blocklist(void)
{
    net_set_blocklist(U.peers.blocked, U.peers.n_blocked);
}

/* push_contacts() -- give the network the contacts' addresses: a general
 * SOS also goes to each of them through the phone (phone.h). */
static void push_contacts(void)
{
    uint32_t list[PEER_SLOTS];
    int n = 0;
    for (int i = 0; i < PEER_SLOTS; i++) {
        if (U.peers.slot[i].used) {
            list[n++] = U.peers.slot[i].addr;
        }
    }
    net_set_contacts(list, n);
}

/* save_peers() -- the contacts or blocklist changed: store them, and keep
 * the network's copy of the contacts up to date. */
static void save_peers(void)
{
    store_save_peers(&U.peers);
    push_contacts();
}

/*
 * contact_for() -- make sure `addr` is a contact after a successful
 * connection. Returns its slot (-1 if the table is full) and sets *added
 * when the contact is new (the user then hears the slot digit).
 */
static int contact_for(uint32_t addr, bool *added)
{
    *added = false;
    int slot = peers_find(&U.peers, addr);
    if (slot >= 0) {
        return slot;
    }
    slot = peers_add(&U.peers, addr);
    if (slot >= 0) {
        save_peers();
        *added = true;
    }
    return slot;
}

/* table_full_for() -- connecting with `addr` would need a free slot and
 * there is none. */
static bool table_full_for(uint32_t addr)
{
    return peers_find(&U.peers, addr) < 0 && peers_count(&U.peers) >= PEER_SLOTS;
}

/* ---- requests played to the user (for A / N) ---------------------------- */

/* inreq_remove() -- forget a request (answered, expired, or its sender gone). */
static void inreq_remove(uint32_t addr)
{
    for (int i = 0; i < U.n_inreq; i++) {
        if (U.inreq[i] == addr) {
            memmove(&U.inreq[i], &U.inreq[i + 1], (size_t)(U.n_inreq - i - 1) * sizeof(U.inreq[0]));
            U.n_inreq--;
            return;
        }
    }
}

/* inreq_add() -- a request was played to the user: A / N can now refer to it. */
static void inreq_add(uint32_t addr)
{
    inreq_remove(addr); /* asked again: moves to the back */
    if (U.n_inreq == LINK_PENDING_MAX) {
        inreq_remove(U.inreq[0]);
    }
    U.inreq[U.n_inreq++] = addr;
}

/* inreq_at() -- the request at position pos (1 = oldest), or ADDR_NONE. */
static uint32_t inreq_at(int pos)
{
    return (pos >= 1 && pos <= U.n_inreq) ? U.inreq[pos - 1] : ADDR_NONE;
}

/*
 * restore_draft() -- a message wasn't delivered: put its text back into the
 * draft, in front of anything keyed since, so nothing is lost (cut at
 * MSG_MAX_CHARS). The user can send it again with a 2 s hold.
 */
static void restore_draft(const char *text)
{
    char buf[2 * MSG_MAX_CHARS + 2];
    snprintf(buf, sizeof buf, "%s%s%s", text, (U.draft_len && text[0]) ? " " : "", U.draft);
    strncpy(U.draft, buf, MSG_MAX_CHARS);
    U.draft[MSG_MAX_CHARS] = '\0';
    U.draft_len = (int)strlen(U.draft);
    if (U.st == ST_IDLE) {
        U.st = ST_COMPOSE;
    }
    ESP_LOGW(TAG, "not delivered: back in the draft: \"%s\"", U.draft);
}

/* ------------------------------------------------------------------------- */
/* The phone (ble.h; the lines are in phone.h)                                */
/* ------------------------------------------------------------------------- */

/* phone_send() -- one line to the phone, if one is ready and has had its HI
 * (otherwise it is simply not sent: the phone catches up from ST and DR
 * when it comes). */
static void phone_send(const char *line)
{
    if (line[0] && U.ph_hello && ble_phone_ready()) {
        ble_send_line(line);
    }
}

/* phone_link_word() -- the link state as an ST line spells it. */
static const char *phone_link_word(void)
{
    switch (U.link_state) {
    case LS_UP:
        return U.link_addr == ADDR_NONE ? "none" : "connected";
    case LS_LOST:
        return "lost";
    case LS_ENDED:
        return "ended";
    default:
        return "none";
    }
}

/* in_command() -- is the user in command mode (keying a command, or its
 * echo playing)? */
static bool in_command(void)
{
    return U.st == ST_COMMAND || U.st == ST_CMD_ECHO;
}

/* phone_path_word() -- path `p`'s state as an ST line spells it (the
 * engine's view, from the net status), or "none" without a link. */
static const char *phone_path_word(const net_status_t *ns, uint8_t p)
{
    if (U.link_state != LS_UP && U.link_state != LS_LOST) {
        return link_path_state_name(PS_NONE);
    }
    return link_path_state_name(ns->path_st[p]);
}

/* cal_health_word() -- the calibration's health as the SP line spells it. */
static const char *cal_health_word(cal_health_t h)
{
    switch (h) {
    case CAL_HEALTHY:
        return "healthy";
    case CAL_MARGINAL:
        return "marginal";
    case CAL_COLLAPSED:
        return "collapsed";
    default:
        return "none";
    }
}

/* round_ms() -- a float time in ms as the nearest whole ms (never below 0). */
static int round_ms(float ms)
{
    return ms <= 0.f ? 0 : (int)(ms + 0.5f);
}

/*
 * phone_speeds_line() -- the SP line as it would read now (phone.h): the
 * learned keying timings and the decoder's thresholds from the
 * calibration, the reading speed, and the vibration strength. Returns its
 * length, -1 if it didn't fit.
 */
static int phone_speeds_line(char *line, int size)
{
    const cal_profile_t *p = cal_profile(&U.cal);
    phone_speeds_t sp = {
        .valid = cal_valid(&U.cal),
        .calibrating = U.st == ST_CALIB,
        .dot_ms = round_ms(p->dot_mean),
        .dash_ms = round_ms(p->dash_mean),
        .gap_in_ms = round_ms(p->gi_mean),
        .gap_letter_ms = round_ms(p->gl_mean),
        .boundary_ms = round_ms(cal_boundary(&U.cal)),
        .letter_ms = round_ms(cal_letter_threshold(&U.cal)),
        .word_ms = round_ms(cal_word_threshold(&U.cal)),
        .health = cal_health_word(cal_health(&U.cal)),
        .q = U.set.speed_q,
        .unit_ms = hspeed_unit_ms(U.set.speed_q),
        .fx10 = U.set.farnsworth_x10,
        .strength = U.set.strength ? U.set.strength : HSTRENGTH_DEFAULT,
        .strength_mv = U.motor_mv,
        .dash_style = U.set.dash_style == HDASH_M2 ? HDASH_M2 : HDASH_SWEEP,
    };
    return phone_fmt_speeds(line, size, &sp);
}

/*
 * phone_sync() -- keep the phone's picture current: ST when the partner,
 * the link (and each path), the battery or silent mode changed since the
 * last ST; RD when the radio setting did; DR when the draft did; CM while in
 * command mode, when what has been keyed changed. With `force` they all go
 * regardless (after HI). Runs after every UI event and at least every 250 ms
 * (the UI task's loop), so the phone sees each character as it resolves:
 * the live compose stream, without hooks in every place that edits the
 * draft. Nothing before HI.
 */
static void phone_sync(bool force)
{
    if (!U.ph_hello || !ble_phone_ready()) {
        return;
    }
    char line[PHONE_LINE_MAX + 1];
    net_status_t ns;
    net_get_status(&ns);
    if (phone_fmt_status(line, sizeof line, U.link_addr, phone_link_word(), U.bat_pct, !U.set.discoverable,
                         phone_path_word(&ns, LP_LORA), phone_path_word(&ns, LP_RELAY)) > 0 &&
        (force || strcmp(line, U.ph_status) != 0)) {
        strcpy(U.ph_status, line);
        ble_send_line(line);
    }
    if (phone_fmt_radio(line, sizeof line, ns.sf, (int)(RADIO_BW_HZ / 1000), ns.tx_dbm) > 0 &&
        (force || strcmp(line, U.ph_radio) != 0)) {
        strncpy(U.ph_radio, line, sizeof U.ph_radio - 1);
        U.ph_radio[sizeof U.ph_radio - 1] = '\0';
        ble_send_line(line);
    }
    if (force || !U.ph_draft_sent || strcmp(U.draft, U.ph_draft) != 0) {
        strcpy(U.ph_draft, U.draft);
        U.ph_draft_sent = true;
        if (phone_fmt_draft(line, sizeof line, U.draft) > 0) {
            ble_send_line(line);
        }
    }
    if (in_command()) {
        const char *keyed = U.st == ST_CMD_ECHO ? U.exec : U.cmd; /* during the echo: what is about to run */
        if (force || !U.ph_cm_on || strcmp(keyed, U.ph_cm) != 0) {
            U.ph_cm_on = true;
            strcpy(U.ph_cm, keyed);
            if (phone_fmt_cmd(line, sizeof line, keyed) > 0) {
                ble_send_line(line);
            }
        }
    } else if (U.ph_cm_on) {
        U.ph_cm_on = false; /* left without a reason recorded (phone_cm_end covers the usual ways) */
        if (phone_fmt_cmd_end(line, sizeof line, "done", U.ph_cm) > 0) {
            ble_send_line(line);
        }
    }
    /* The learning view (protocol 4). KY alone once nothing is open any more:
     * the letter closed by itself (its KC went already), was dropped (a
     * hold, a state change), or the PARIS attempt was judged. */
    bool open = U.st == ST_CALIB ? U.cal_n > 0 : dec_group_open(&U.dec);
    if (U.ph_ky_open && !open) {
        U.ph_ky_open = false;
        if (phone_fmt_key(line, sizeof line, NULL, NULL, 0, 0) > 0) {
            ble_send_line(line);
        }
    }
    /* SP when the speeds changed, at most every PHONE_SP_GAP_MS (the
     * calibration moves a little with every letter keyed). */
    int64_t now = now_ms();
    if (phone_speeds_line(line, sizeof line) > 0 && (force || strcmp(line, U.ph_speeds) != 0) &&
        (force || !U.ph_speeds[0] || now - U.ph_speeds_at >= PHONE_SP_GAP_MS)) {
        strncpy(U.ph_speeds, line, sizeof U.ph_speeds - 1);
        U.ph_speeds[sizeof U.ph_speeds - 1] = '\0';
        U.ph_speeds_at = now;
        ble_send_line(line);
    }
}

/*
 * phone_cm_end() -- command mode ended (`how`: done, cancelled, timeout,
 * sos) or a command was given up while staying in it (aborted, error):
 * "CE how command" at once, so the phone can say what happened. If still in
 * command mode, the next phone_sync() sends CM again (with what is keyed
 * now, usually nothing).
 */
static void phone_cm_end(const char *how, const char *command)
{
    if (!U.ph_cm_on) {
        return; /* the phone wasn't told we were in command mode */
    }
    U.ph_cm_on = false;
    U.ph_cm[0] = '\0';
    char line[PHONE_LINE_MAX + 1];
    if (U.ph_hello && ble_phone_ready() && phone_fmt_cmd_end(line, sizeof line, how, command) > 0) {
        ble_send_line(line);
    }
}

/*
 * phone_key_press() -- KY: a press was just counted as an element. In
 * calibration (`cal`) the pattern is the whole PARIS attempt so far, each
 * press judged by the boundary in use (before the first calibration: the
 * starting guess); otherwise the letter being keyed, as the decoder has it.
 * Also the press's length and that boundary, for the app's timing bar.
 */
static void phone_key_press(bool cal)
{
    char pat[CAL_CAPTURE_MAX + 1];
    int ms;
    if (cal) {
        int n = 0;
        for (int i = 0; i < U.cal_n && i < CAL_CAPTURE_MAX; i++) {
            pat[n++] = cal_is_dash(&U.cal, U.cal_el[i]) ? '-' : '.';
        }
        pat[n] = '\0';
        ms = U.cal_n > 0 ? round_ms(U.cal_el[U.cal_n - 1]) : 0;
    } else {
        dec_open_pattern(&U.dec, pat, sizeof pat);
        ms = U.dec.last_ms;
    }
    char line[PHONE_LINE_MAX + 1];
    if (phone_fmt_key(line, sizeof line, cal ? "cal" : "key", pat, ms, round_ms(cal_boundary(&U.cal))) > 0) {
        phone_send(line);
    }
    U.ph_ky_open = pat[0] != '\0'; /* (an empty pattern went as "KY": nothing open) */
}

/*
 * phone_key_char() -- KC: the decoder closed a letter (a character, an
 * unknown pattern, or the error prosign). Sent before the DR line that
 * shows its effect, so the app can put the pattern as keyed above the
 * letter (and above a '?'). Spaces and SOS have their own ways to show.
 */
static void phone_key_char(const dec_evt_t *e)
{
    const char *kind;
    char c = 0;
    switch (e->kind) {
    case DEC_CHAR:
        kind = "char";
        c = e->c;
        break;
    case DEC_UNKNOWN:
        kind = "unknown";
        c = '?';
        break;
    case DEC_ERROR:
        kind = "error";
        break;
    default:
        return;
    }
    if (!e->pattern[0]) {
        return;
    }
    char line[48];
    if (phone_fmt_key_char(line, sizeof line, kind, e->pattern, c) > 0) {
        phone_send(line);
    }
    U.ph_ky_open = false; /* the app clears its open letter on KC */
}

/* cal_err_word() -- why a PARIS attempt failed, as the CB line says it. */
static const char *cal_err_word(cal_err_t r)
{
    switch (r) {
    case CAL_ERR_COUNT:
        return "count"; /* not 14 presses */
    case CAL_ERR_RANGE:
        return "range"; /* a press shorter than 20 ms or longer than 1.5 s */
    case CAL_ERR_ELEMENTS:
        return "elements"; /* dots and dashes overlap */
    case CAL_ERR_GAPS:
        return "gaps"; /* letter gaps not longer than the gaps inside letters */
    default:
        return "other";
    }
}

/* phone_cal_result() -- CB: a PARIS attempt was accepted, or not and why. */
static void phone_cal_result(cal_err_t r)
{
    char line[32];
    if (phone_fmt_cal_result(line, sizeof line, r == CAL_OK, cal_err_word(r)) > 0) {
        phone_send(line);
    }
}

/*
 * phone_play_start() -- PL: a playback just started; one line per Morse
 * item in it, with when its first element comes (from now) and the timing
 * the phone needs to follow it character by character. A job without Morse
 * (a tone) ends the phone's view of whatever played before.
 */
static void phone_play_start(const hjob_t *j)
{
    if (!U.ph_hello || !ble_phone_ready()) {
        U.ph_pl_on = false;
        return;
    }
    hparams_t p;
    haptics_get_params(&p);
    uint32_t starts[HJOB_MAX_ITEMS];
    hjob_item_starts(j, &p, starts);
    /* hseq_start()'s clamps, so the phone times it as it really plays */
    int unit = p.unit_ms < 20 ? 20 : p.unit_ms;
    int fx10 = p.farnsworth_x10 < 10 ? 10 : p.farnsworth_x10;
    char line[PHONE_LINE_MAX + 1];
    int k = 0; /* Morse parts told so far */
    for (int i = 0; i < j->n; i++) {
        if (j->items[i].kind != HI_MORSE || starts[i] == HSEQ_NO_START) {
            continue;
        }
        if (phone_fmt_play(line, sizeof line, k, starts[i], unit, fx10, &j->text[j->items[i].arg]) > 0) {
            ble_send_line(line);
            k++;
        }
    }
    if (k > 0) {
        U.ph_pl_on = true;
    } else {
        phone_play_end(); /* a tone replaced a Morse playback (if one was on) */
    }
}

/* phone_play_end() -- "PL end", once, if the phone was told a playback is on. */
static void phone_play_end(void)
{
    if (!U.ph_pl_on) {
        return;
    }
    U.ph_pl_on = false;
    char line[16];
    if (phone_fmt_play_end(line, sizeof line) > 0) {
        phone_send(line);
    }
}

/* phone_hello() -- a phone is ready, or said HI, or our ID changed: who we
 * are, then the state. The phone always hears HI first. */
static void phone_hello(void)
{
    if (!ble_phone_ready()) {
        return; /* (it left again before we got to it) */
    }
    U.ph_hello = true;
    char line[PHONE_LINE_MAX + 1];
    if (phone_fmt_hi(line, sizeof line, own_addr(), MORSELINK_VERSION) > 0) {
        phone_send(line);
    }
    phone_sync(true);
}

/* phone_event() -- an EV line: something the phone shows in its timeline. */
static void phone_event(uint8_t ev, uint32_t who)
{
    char line[PHONE_LINE_MAX + 1];
    if (phone_fmt_event(line, sizeof line, ev, who) > 0) {
        phone_send(line);
    }
}

/* phone_event_detail() -- an EV line with a third field (the reason of a
 * loss, a path's new state). */
static void phone_event_detail(uint8_t ev, uint32_t who, const char *detail)
{
    char line[PHONE_LINE_MAX + 1];
    if (phone_fmt_event_detail(line, sizeof line, ev, who, detail) > 0) {
        phone_send(line);
    }
}

/*
 * phone_msg_out() -- a message left for `to`: "TX n to text". n is
 * remembered until its result (phone_msg_result), whether or not a phone is
 * there right now, so the numbers stay in step.
 */
static void phone_msg_out(uint32_t to, const char *text)
{
    unsigned n = phone_tx_add(&U.ph_tx, to);
    char line[PHONE_LINE_MAX + 1];
    if (phone_fmt_tx(line, sizeof line, n, to, text) > 0) {
        phone_send(line);
    }
}

/*
 * phone_msg_result() -- what became of messages announced with TX, as the
 * network reported it (phone_tx_res_t, phone.h): the phone gets OK n for a
 * delivered one and NO n for each one that wasn't, and they stop waiting.
 * NO also covers messages dropped because a contact was forgotten (their
 * text is not put back then: the user forgot that person).
 */
static void phone_msg_result(phone_tx_res_t r, uint32_t to)
{
    unsigned done[PHONE_TX_MAX];
    int k = phone_tx_settle(&U.ph_tx, r, to, done, PHONE_TX_MAX);
    char line[PHONE_LINE_MAX + 1];
    for (int i = 0; i < k; i++) {
        if (phone_fmt_result(line, sizeof line, done[i], r == PHONE_TX_DELIVERED) > 0) {
            phone_send(line);
        }
    }
}

/*
 * on_phone() -- what the BLE side reports (ui_post_phone; uip_t in ui.h):
 *   - a phone is ready, or says HI: it gets HI, ST and DR;
 *   - the O window ended: OK (a phone paired) or error (none came), queued
 *     at the front like the other results, so it plays once the key rests;
 *   - the phone went away: only logged. It is auxiliary: walking away from
 *     it shouldn't buzz. The next one gets the whole picture again.
 */
static void on_phone(uint8_t what)
{
    alert_t a = {.kind = AL_TONE};
    switch (what) {
    case UIP_READY:
    case UIP_HELLO:
        phone_hello();
        break;
    case UIP_PAIRED:
        a.tone = TONE_OK;
        alert_push(&a, true);
        ESP_LOGI(TAG, "phone paired: OK");
        break;
    case UIP_WINDOW_CLOSED:
        a.tone = TONE_ERROR;
        alert_push(&a, true);
        ESP_LOGW(TAG, "no phone paired within %d s: error (O to try again)", BLE_WINDOW_MS / 1000);
        break;
    case UIP_GONE:
        U.ph_hello = false;
        U.ph_status[0] = '\0';
        U.ph_radio[0] = '\0';
        U.ph_draft_sent = false;
        U.ph_cm_on = false;
        U.ph_ky_open = false;
        U.ph_speeds[0] = '\0';
        U.ph_pl_on = false;
        ESP_LOGI(TAG, "phone gone");
        break;
    default:
        break;
    }
}

/* ble_wanted() -- should Bluetooth run now that nothing else needs it off
 * (at boot, after a test or bench power)? Yes if a phone is paired (O) and
 * the user hasn't switched it off with W (v0.6.1, stored). */
static bool ble_wanted(void)
{
    return store_load_ble_on() && !store_load_ble_off();
}

/*
 * ble_switch() -- W (or `phone on` / `phone off`): want = 1 on, 0 off, -1 the
 * other way round from now. Off: the phone hears it first (EV BLE_OFF with
 * our own ID: the app then says so and tries less often), its last lines go
 * out, then Bluetooth stops (and the console's pretend phone goes, as a real
 * one would); stored, so it stays off through restarts. On: only with a
 * paired phone (O pairs one, and clears the flag too); stored. Returns 1
 * (now on), 0 (now off) or -1 (refused: no phone paired).
 */
static int ble_switch(int want)
{
    ble_status_t bs;
    ble_get_status(&bs);
    bool on_now = bs.started || bs.sim;
    if (want < 0) {
        want = on_now ? 0 : 1;
    }
    if (want == 0) {
        if (on_now) {
            phone_event(PHONE_EV_BLE_OFF, own_addr());
            ble_flush(500);
            ble_stop();
            if (bs.sim) {
                ble_sim(false);
            }
        }
        store_save_ble_off(true);
        ESP_LOGW(TAG, "Bluetooth off (W): it stays off, restarts included, until W again");
        return 0;
    }
    if (!store_load_ble_on()) {
        ESP_LOGW(TAG, "Bluetooth: no phone is paired (O pairs a phone; Bluetooth then stays on)");
        return -1;
    }
    store_save_ble_off(false);
    if (ble_start() != ESP_OK) {
        return -1; /* (the reason is logged) */
    }
    ESP_LOGI(TAG, "Bluetooth on (W): the paired phone reconnects by itself");
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Calibration                                                                */
/* ------------------------------------------------------------------------- */

/* calib_prompt() -- play the PARIS prompt (after an error tone if the last
 * attempt failed) and start capturing presses. */
static void calib_prompt(bool after_error)
{
    hjob_t j;
    hjob_init(&j);
    if (after_error) {
        hjob_tone(&j, TONE_ERROR);
        hjob_gap(&j, 700);
    }
    hjob_tone(&j, TONE_ATTENTION);
    hjob_morse(&j, "PARIS");
    U.cal_n = 0;
    U.cal_capturing = false;
    play_job(&j);
    ESP_LOGI(TAG, "calibration: key PARIS once you've felt it (stop for 3 s, or hold 2 s, when done)");
}

/* calib_enter() -- enter calibration (first use, or C). */
static void calib_enter(bool first_use)
{
    U.cal_first_use = first_use;
    U.st = ST_CALIB;
    U.cmd_len = 0;
    U.cmd[0] = '\0';
    dec_reset(&U.dec);
    calib_prompt(false);
}

/* log_calibration() -- one line with the learned timing. */
static void log_calibration(const char *prefix)
{
    const cal_profile_t *p = cal_profile(&U.cal);
    ESP_LOGI(TAG,
             "%s dot %.0f ms (mad %.0f), dash %.0f ms (mad %.0f), ratio %.2f, d'=%.1f | boundary %.0f, letter gap %.0f, "
             "word gap %.0f ms",
             prefix, p->dot_mean, p->dot_mad, p->dash_mean, p->dash_mad, p->ratio, cal_dprime(&U.cal),
             cal_boundary(&U.cal), cal_letter_threshold(&U.cal), cal_word_threshold(&U.cal));
}

static void sos_enter(void);

/* calib_capture() -- record one press (and the gap before it) of the PARIS
 * attempt. */
static void calib_capture(int64_t down, int64_t up)
{
    if (U.cal_n >= CAL_CAPTURE_MAX) {
        return;
    }
    if (U.cal_n > 0) {
        U.cal_gap[U.cal_n - 1] = (float)(down - U.cal_last_up);
    }
    U.cal_el[U.cal_n++] = (float)(up - down);
    U.cal_last_up = up;
    U.cal_capturing = true;
}

/* calib_evaluate() -- the PARIS attempt is over: accept it (save, OK) or
 * prompt again. Nine presses shaped like SOS go to the SOS flow instead. */
static void calib_evaluate(void)
{
    int n = U.cal_n;
    U.cal_n = 0;
    if (n == 0) {
        return;
    }
    /* SOS never meets a calibration prompt. */
    if (n == 9 && dec_sos_shape9(U.cal_el)) {
        sos_enter();
        return;
    }
    cal_err_t r = cal_from_paris(&U.cal, U.cal_el, n, U.cal_gap, n - 1);
    phone_cal_result(r);
    if (r == CAL_OK) {
        store_save_calib(cal_profile(&U.cal));
        U.cal_dirty = false;
        U.cal_last_save = now_ms();
        log_calibration("calibrated:");
        dec_reset(&U.dec);
        play_result_tone(TONE_OK);
        return;
    }
    ESP_LOGW(TAG, "calibration not accepted (%s, %d elements). Again, please.", cal_err_str(r), n);
    calib_prompt(true);
}

/* calib_cancel() -- 5 s hold during calibration: keep the old profile if
 * there is one, otherwise prompt again. */
static void calib_cancel(void)
{
    if (!U.cal_first_use && cal_valid(&U.cal)) {
        /* The profile is only replaced when a new PARIS is accepted. */
        ESP_LOGI(TAG, "recalibration cancelled; previous profile kept");
        dec_reset(&U.dec);
        U.st = resume_state();
        return;
    }
    calib_prompt(false);
}

/* go_resume() -- return to idle/compose, or to calibration if there is
 * still no profile (e.g. after an SOS keyed during first-use calibration). */
static void go_resume(void)
{
    if (!cal_valid(&U.cal)) {
        calib_enter(true);
        return;
    }
    U.st = resume_state();
}

/* ------------------------------------------------------------------------- */
/* SOS                                                                        */
/* ------------------------------------------------------------------------- */

/* sos_enter() -- SOS was keyed: drop the draft and command, wait for an
 * optional target and the confirming silence. */
static void sos_enter(void)
{
    if (in_command()) {
        phone_cm_end("sos", U.cmd);
    }
    U.draft_len = 0;
    U.draft[0] = '\0';
    U.cmd_len = 0;
    U.cmd[0] = '\0';
    U.sos_arg_len = 0;
    U.sos_arg[0] = '\0';
    U.tent_armed = U.tent_active = false;
    U.st = ST_SOS_ENTRY;
    U.sos_ref = now_ms();
    dec_reset(&U.dec);
    ESP_LOGW(TAG, "SOS keyed: add a slot or ID to target it, or wait 3 s for a general SOS");
}

/* sos_arm() -- the confirming silence passed: echo the target (if any),
 * then three long buzzes; a press during them aborts (on_key_down). */
static void sos_arm(void)
{
    uint32_t target = ADDR_BROADCAST;
    if (U.sos_arg_len) {
        int slot;
        uint32_t a = peers_resolve(&U.peers, U.sos_arg, &slot);
        if (a != ADDR_NONE) {
            target = a;
        } else {
            ESP_LOGW(TAG, "SOS target '%s' not understood: sending a general SOS", U.sos_arg);
        }
    }
    U.sos_target = target;
    hjob_t j;
    char id[ID_MAX_LEN + 1];
    hjob_init(&j);
    if (target != ADDR_BROADCAST) {
        hjob_morse(&j, id_of(target, id));
    }
    hjob_tone(&j, TONE_SOS_ARMED);
    U.st = ST_SOS_ARMED;
    play_job(&j);
    ESP_LOGW(TAG, "SOS armed (%s): press now to abort", id_of(target, id));
}

static void sos_start_alarm(uint32_t src);

/* sos_transmit() -- the armed buzz ended untouched: send the SOS and start
 * the resend timer. */
static void sos_transmit(void)
{
    net_send_sos(U.sos_target);
    phone_event(PHONE_EV_SOS_SENT, U.sos_target);
    U.sos_active = true;
    U.sos_n_resp = 0;
    U.sos_started = now_ms();
    U.sos_next = U.sos_started + SOS_RESEND_FAST_MS;
    dec_reset(&U.dec);
    U.st = ST_IDLE;
    play_result_tone(TONE_OK);
}

/* sos_cancel() -- stop our SOS (5 s hold) and tell responders, so their
 * alarms stop. */
static void sos_cancel(void)
{
    if (!U.sos_active) {
        return;
    }
    U.sos_active = false;
    net_send_sos_cancel(U.sos_target);
    phone_event(PHONE_EV_SOS_STOPPED, U.sos_target);
    ESP_LOGW(TAG, "SOS cancelled");
}

/* ---- SOS senders we have acknowledged ------------------------------------ */

/* acked_has() -- have we acknowledged an SOS from src (that isn't cancelled)? */
static bool acked_has(uint32_t src)
{
    for (int i = 0; i < U.n_acked; i++) {
        if (U.acked[i] == src) {
            return true;
        }
    }
    return false;
}

/* acked_add() -- remember we acknowledged src's SOS (oldest forgotten when full). */
static void acked_add(uint32_t src)
{
    if (acked_has(src)) {
        return;
    }
    if (U.n_acked == ACKED_MAX) {
        memmove(U.acked, U.acked + 1, (ACKED_MAX - 1) * sizeof(U.acked[0]));
        U.n_acked--;
    }
    U.acked[U.n_acked++] = src;
}

/* acked_remove() -- src cancelled their SOS: forget it. */
static void acked_remove(uint32_t src)
{
    for (int i = 0; i < U.n_acked; i++) {
        if (U.acked[i] == src) {
            memmove(&U.acked[i], &U.acked[i + 1], (size_t)(U.n_acked - i - 1) * sizeof(U.acked[0]));
            U.n_acked--;
            return;
        }
    }
}

/* play_alarm() -- one round of the SOS alarm (5 x 1 s). */
static void play_alarm(void)
{
    hjob_t j;
    hjob_init(&j);
    hjob_tone(&j, TONE_SOS_ALARM);
    play_job(&j);
}

/* sos_start_alarm() -- someone's SOS: interrupt everything and alarm until
 * a tap acknowledges it. */
static void sos_start_alarm(uint32_t src)
{
    char id[ID_MAX_LEN + 1];
    stop_playback();
    U.cmd_len = 0;
    U.cmd[0] = '\0';
    U.alarm_src = src;
    U.st = ST_SOS_ALARM;
    play_alarm();
    ESP_LOGW(TAG, "SOS received from %s: tap to acknowledge", id_of(src, id));
}

/* on_net_sos() -- an SOS arrived (spec §10: ignores silent mode and the
 * blocklist). A resend from someone already acknowledged is re-acknowledged
 * silently; one arriving during our own SOS or another alarm waits. */
static void on_net_sos(uint32_t src, bool targeted)
{
    char id[ID_MAX_LEN + 1];
    (void)targeted;
    if (acked_has(src)) {
        net_send_sos_ack(src); /* a resend: our ack may have been lost */
        return;
    }
    if (U.st == ST_SOS_ALARM && U.alarm_src == src) {
        return; /* already alarming for this one */
    }
    phone_event(PHONE_EV_SOS, src);
    if (U.test) {
        ESP_LOGW(TAG, "test mode: SOS from %s does not alarm (a 5 s hold ends the test)", id_of(src, id));
        return;
    }
    if (U.st == ST_SOS_ENTRY || U.st == ST_SOS_ARMED || U.st == ST_SOS_ALARM) {
        /* Don't derail the user's own SOS, or drop an alarm not yet
         * acknowledged: this one alarms as soon as that is done. */
        U.alarm_pending = src;
        ESP_LOGW(TAG, "SOS received from %s; will alarm next", id_of(src, id));
        return;
    }
    sos_start_alarm(src);
}

/* sos_alarm_ack() -- the tap that acknowledges an alarm: send SOS_ACK, make
 * the sender our recipient (no handshake: spec §10) and play their ID. A
 * blocked sender is unblocked: SOS overrides the blocklist, and that must
 * hold for the conversation that follows too. */
static void sos_alarm_ack(void)
{
    char id[ID_MAX_LEN + 1];
    uint32_t src = U.alarm_src;
    stop_playback();
    acked_add(src);
    net_send_sos_ack(src);
    if (peers_unblock(&U.peers, src)) {
        save_peers();
        push_blocklist();
        ESP_LOGW(TAG, "%s was blocked: unblocked, since you answered their SOS", id_of(src, id));
    }
    link_select(src); /* a reply goes straight to them */
    ESP_LOGW(TAG, "SOS from %s acknowledged; they are now your recipient", id_of(src, id));
    hjob_t j;
    hjob_init(&j);
    hjob_morse(&j, id_of(src, id));
    U.alarm_src = ADDR_NONE;
    play_result(&j);
}

/* on_net_sos_cancel() -- the sender cancelled their SOS: stop the alarm
 * (long + ID), or tell the user if they had acknowledged it already. */
static void on_net_sos_cancel(uint32_t src)
{
    char id[ID_MAX_LEN + 1];
    if (U.st == ST_SOS_ALARM && U.alarm_src == src) {
        stop_playback();
        U.alarm_src = ADDR_NONE;
        hjob_t j;
        hjob_init(&j);
        hjob_tone(&j, TONE_CANCEL);
        hjob_morse(&j, id_of(src, id));
        play_result(&j);
    } else if (acked_has(src)) {
        alert_tone_id(TONE_CANCEL, src, true);
    } else if (U.alarm_pending == src) {
        U.alarm_pending = ADDR_NONE;
    }
    acked_remove(src);
    phone_event(PHONE_EV_SOS_CANCEL, src);
    ESP_LOGW(TAG, "%s cancelled their SOS", id_of(src, id));
}

/* on_net_sos_ack() -- someone acknowledged our SOS: "delivered" + their ID.
 * With no (working) recipient, the first responder becomes it. */
static void on_net_sos_ack(uint32_t src)
{
    char id[ID_MAX_LEN + 1];
    if (!U.sos_active) {
        return;
    }
    for (int i = 0; i < U.sos_n_resp; i++) {
        if (U.sos_responders[i] == src) {
            return;
        }
    }
    if (U.sos_n_resp < ACKED_MAX) {
        U.sos_responders[U.sos_n_resp++] = src;
    }
    phone_event(PHONE_EV_SOS_ACK, src);
    if (U.link_addr == ADDR_NONE || U.link_state == LS_ENDED) {
        link_select(src);
    }
    alert_tone_id(TONE_DELIVERED, src, true);
    ESP_LOGW(TAG, "SOS acknowledged by %s", id_of(src, id));
}

/* ------------------------------------------------------------------------- */
/* Composing and sending                                                      */
/* ------------------------------------------------------------------------- */

/* tent_note() -- a character was added at `pos`: if the press that started
 * it also stopped a playback and it is a single element, it is tentative
 * (it may have been meant only as "stop"). */
static void tent_note(const dec_evt_t *e, int pos)
{
    if (U.tent_armed) {
        U.tent_armed = false;
        U.tent_active = (e->n_el == 1);
        U.tent_pos = pos;
    } else {
        U.tent_active = false;
    }
}

/* tent_drop() -- a lone one-element character right after a press that
 * stopped a playback, followed by a word gap or a hold: that press was just
 * "stop", so remove it. Returns true if it did. */
static bool tent_drop(char *buf, int *len)
{
    if (U.tent_active && *len == U.tent_pos + 1) {
        U.tent_active = false;
        buf[--(*len)] = '\0';
        return true;
    }
    U.tent_active = false;
    return false;
}

/*
 * start_send() -- the 2 s hold in compose: check there is somewhere to send
 * it, then announce the recipient (safety rule); transmit_draft() runs when
 * the announcement ends untouched.
 */
static void start_send(void)
{
    char id[ID_MAX_LEN + 1];
    while (U.draft_len && U.draft[U.draft_len - 1] == ' ') {
        U.draft[--U.draft_len] = '\0';
    }
    if (U.draft_len == 0) {
        U.st = ST_IDLE;
        play_tone(TONE_ERROR);
        return;
    }
    if (!U.own_id[0]) {
        ESP_LOGW(TAG, "this device has no ID yet (console: id K7Q): draft kept");
        play_tone(TONE_ERROR);
        return;
    }
    if (U.link_addr == ADDR_NONE) {
        ESP_LOGW(TAG, "no recipient selected: draft kept (use S in command mode)");
        play_tone(TONE_ERROR);
        return;
    }
    hjob_t j;
    hjob_init(&j);
    if (U.link_state == LS_LOST || U.link_state == LS_ENDED) {
        hjob_tone(&j, TONE_ERROR);
        hjob_morse(&j, id_of(U.link_addr, id));
        play_job(&j);
        ESP_LOGW(TAG, "%s is %s: draft kept", id,
                 U.link_state == LS_LOST ? "out of range" : "linked with someone else now");
        return;
    }
    /* Safety rule: announce the target before the message leaves. */
    hjob_morse(&j, id_of(U.link_addr, id));
    U.st = ST_SEND_ANNOUNCE;
    play_job(&j);
}

/*
 * transmit_draft() -- the announcement ended untouched: hand the draft to
 * the network for the announced recipient. Nothing plays now: OK plays when
 * the receipt arrives (LEV_MSG_SENT), and if it doesn't, the text comes back
 * (LEV_MSG_FAILED). If it can't even be queued, error and the draft stays.
 */
static void transmit_draft(void)
{
    char id[ID_MAX_LEN + 1];
    ESP_LOGI(TAG, "sending to %s: \"%s\"", id_of(U.link_addr, id), U.draft);
    net_result_t r = net_send_text(U.link_addr, U.draft);
    if (r != NET_OK) {
        ESP_LOGW(TAG, "%s: draft kept", r == NET_ERR_NO_TRANSPORT ? "no radio, and loopback is off" : "network busy");
        U.st = ST_COMPOSE;
        play_tone(TONE_ERROR);
        return;
    }
    phone_msg_out(U.link_addr, U.draft);
    U.draft_len = 0;
    U.draft[0] = '\0';
    dec_reset(&U.dec);
    go_resume();
}

/* compose_evt() -- a decoder event while composing (or idle). */
static void compose_evt(const dec_evt_t *e)
{
    switch (e->kind) {
    case DEC_CHAR:
    case DEC_UNKNOWN:
        if (U.draft_len >= MSG_MAX_CHARS) {
            U.tent_armed = false;
            play_tone(TONE_ERROR);
            ESP_LOGW(TAG, "message is full (%d characters)", MSG_MAX_CHARS);
            return;
        }
        U.draft[U.draft_len++] = e->c;
        U.draft[U.draft_len] = '\0';
        tent_note(e, U.draft_len - 1);
        U.st = ST_COMPOSE;
        ESP_LOGI(TAG, "draft: \"%s\"", U.draft);
        break;
    case DEC_SPACE:
        if (tent_drop(U.draft, &U.draft_len)) {
            if (U.draft_len == 0) {
                U.st = ST_IDLE;
            }
            return;
        }
        if (U.draft_len > 0 && U.draft[U.draft_len - 1] != ' ' && U.draft_len < MSG_MAX_CHARS) {
            U.draft[U.draft_len++] = ' ';
            U.draft[U.draft_len] = '\0';
        }
        break;
    case DEC_ERROR:
        /* Delete the last letter keyed, along with any (invisible) word space
         * that a pause added after it. */
        U.tent_active = false;
        while (U.draft_len > 0 && U.draft[U.draft_len - 1] == ' ') {
            U.draft[--U.draft_len] = '\0';
        }
        if (U.draft_len > 0) {
            U.draft[--U.draft_len] = '\0';
        }
        ESP_LOGI(TAG, "draft: \"%s\" (deleted one)", U.draft);
        if (U.draft_len == 0) {
            U.st = ST_IDLE;
        }
        break;
    case DEC_SOS:
        sos_enter();
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Command mode                                                               */
/* ------------------------------------------------------------------------- */

/* cmd_clear() -- empty the command being keyed. */
static void cmd_clear(void)
{
    U.cmd_len = 0;
    U.cmd[0] = '\0';
}

/* slot_digit() -- is the argument a single digit 1-9? */
static bool slot_digit(const char *arg)
{
    return arg[0] >= '1' && arg[0] <= '9' && arg[1] == '\0';
}

/* cmd_valid() -- does command letter c take this argument? (spec §8) */
static bool cmd_valid(char c, const char *arg)
{
    size_t n = strlen(arg);
    switch (c) {
    case 'P':
        return id_valid(arg);
    case 'D':
    case 'L':
    case 'B':
    case 'C':
    case 'Z':
    case 'E':
    case 'O':
    case 'F':
    case 'W': /* Bluetooth off / on (v0.6.1) */
        return n == 0;
    case 'A':
    case 'N':
    case 'R':
        return n == 0 || slot_digit(arg); /* optional position */
    case 'S':
    case 'V':
    case 'X':
        return slot_digit(arg) || id_valid(arg);
    case 'Q':
        return slot_digit(arg);
    case 'G':
        return n == 0 || (arg[0] >= '1' && arg[0] <= '4' && arg[1] == '\0'); /* range level 1-4, or none: "which?" */
    case 'K': /* keep the draft (no 60 s limit) on / off (v0.6.4) */
        return n == 0;
    case 'H': /* how dashes feel: 1 sweep, 2 M2 only, or none: "which?" (v0.6.3) */
        return n == 0 || ((arg[0] == '1' || arg[0] == '2') && arg[1] == '\0');
    case 'I': /* vibration strength 1-9 (v0.6.4; 1-5 in v0.6.2), or none: "which?" */
        return n == 0 || (arg[0] >= '0' + HSTRENGTH_MIN && arg[0] <= '0' + HSTRENGTH_MAX && arg[1] == '\0');
    default:
        return false;
    }
}

/*
 * start_echo() -- the 2 s hold in command mode: validate, then echo before
 * executing (a press during the echo aborts). The echo is the argument,
 * except that it names who is affected where the argument doesn't:
 *   X / V with a slot digit -> the contact's ID ("say who, before forgetting");
 *   A / N [position]         -> the requester's ID;
 *   F                        -> OFF (what is about to happen).
 */
static void start_echo(void)
{
    char c = U.cmd[0];
    const char *arg = U.cmd_len ? U.cmd + 1 : "";
    if (U.cmd_len == 0 || !cmd_valid(c, arg)) {
        ESP_LOGW(TAG, "command \"%s\" not understood", U.cmd);
        phone_cm_end("error", U.cmd); /* (still in command mode: CM follows) */
        cmd_clear();
        dec_reset(&U.dec);
        play_tone(TONE_ERROR);
        return;
    }
    char echo[ID_MAX_LEN + 2];
    if (c == 'A' || c == 'N') {
        uint32_t a = inreq_at(*arg ? arg[0] - '0' : 1);
        if (a == ADDR_NONE) {
            ESP_LOGW(TAG, "no request is waiting%s", *arg ? " at that position" : "");
            phone_cm_end("error", U.cmd);
            cmd_clear();
            dec_reset(&U.dec);
            play_tone(TONE_ERROR);
            return;
        }
        id_of(a, echo);
    } else if (c == 'F') {
        strcpy(echo, "OFF");
    } else if (!*arg) {
        echo[0] = c;
        echo[1] = '\0';
    } else if ((c == 'X' || c == 'V') && slot_digit(arg)) {
        uint32_t a = peers_addr(&U.peers, arg[0] - '1');
        if (a == ADDR_NONE) {
            strcpy(echo, arg);
        } else {
            id_of(a, echo);
        }
    } else {
        strncpy(echo, arg, sizeof(echo) - 1);
        echo[sizeof(echo) - 1] = '\0';
    }
    strcpy(U.exec, U.cmd);
    U.st = ST_CMD_ECHO;
    hjob_t j;
    hjob_init(&j);
    hjob_morse(&j, echo);
    play_job(&j);
    ESP_LOGI(TAG, "command \"%s\": echoing \"%s\" (press to abort)", U.cmd, echo);
}

/* battery_now() -- read the battery: percentage, or -1 without a reading
 * (no PMIC, or no battery). Also updates the stored mV / charging state. */
static int battery_now(void)
{
    pmic_status_t s;
    if (pmic_read_status(&s) == ESP_OK && s.battery) {
        U.bat_mv = s.vbat_mv;
        U.bat_charging = s.charging;
        return battery_percent_from_mv(s.vbat_mv);
    }
    return -1;
}

/*
 * switch_off() -- F, or `off` in the console: switch the device off
 * (power.h). Refused while our own SOS is active, because being off would
 * silently stop its resends: cancel it first (5 s hold). A calibration
 * update not yet written is saved first; the draft and anything waiting to
 * play are dropped. Returns false (reason logged) if refused; otherwise the
 * device is off and this never returns.
 */
static bool switch_off(void)
{
    if (U.sos_active) {
        ESP_LOGW(TAG, "your SOS is active: cancel it first (5 s hold), then switch off");
        return false;
    }
    if (U.cal_dirty && cal_valid(&U.cal)) {
        store_save_calib(cal_profile(&U.cal));
        U.cal_dirty = false;
    }
    if (U.draft_len) {
        ESP_LOGI(TAG, "switching off: the draft \"%s\" is dropped", U.draft);
    }
    ble_stop(); /* the phone sees us leave at once, instead of after a timeout */
    power_off();
}

/*
 * execute_command() -- the echo ended untouched: do it. Most commands play
 * their result at once (OK, error, or what was asked for). The ones that
 * need the other device (S, P, A) play nothing now: their result arrives
 * later from the network (on_net_event). So does O: OK when a phone has
 * paired, error when its 60 s are up (on_phone). D waits for its list in
 * ST_DISC_SCAN. Also used by the discovery list's invite, as "S<ID>".
 */
static void execute_command(void)
{
    char c = U.exec[0];
    const char *arg = U.exec + 1;
    char id[ID_MAX_LEN + 1], buf[64];
    hjob_t j;
    hjob_init(&j);
    bool ok = true;    /* false: play the error tone instead of j */
    bool quiet = false; /* true: nothing to play now */
    int slot;
    uint32_t a;

    cmd_clear();
    dec_reset(&U.dec);
    phone_cm_end("done", U.exec); /* (the phone hears which command ran, before its result) */

    switch (c) {
    case 'G': { /* range level (agreed 15 Oct): 1-4 = SF9-SF12; alone: play the level in use */
        if (!*arg) {
            buf[0] = (char)('1' + (U.set.sf - RADIO_SF_MIN));
            buf[1] = '\0';
            hjob_morse(&j, buf);
            ESP_LOGI(TAG, "range level %s (SF%d)", buf, U.set.sf);
            break;
        }
        int sf = RADIO_SF_MIN + (arg[0] - '1');
        U.set.sf = (uint8_t)sf;
        save_settings();
        net_set_sf(sf);
        hjob_tone(&j, TONE_OK);
        ESP_LOGW(TAG, "range level %c = SF%d. Set the SAME level on %s: on different levels the two can't hear each "
                      "other at all (no SOS either)",
                 arg[0], sf, U.link_addr == ADDR_NONE ? "the other device" : id_of(U.link_addr, id));
        break;
    }
    case 'H': { /* how dashes feel (v0.6.3): H1 = the sweep wrist -> forearm (default), H2 = forearm (M2) only;
                 alone: play which. Then a sample "A" (dot, dash) in the new style, so you feel it at once */
        int style = U.set.dash_style == HDASH_M2 ? HDASH_M2 : HDASH_SWEEP;
        if (!*arg) {
            buf[0] = (char)('0' + style);
            buf[1] = '\0';
            hjob_morse(&j, buf);
            ESP_LOGI(TAG, "dashes H%d (%s)", style, style == HDASH_M2 ? "M2 only" : "sweep M1 -> M2");
            break;
        }
        U.set.dash_style = (uint8_t)(arg[0] - '0');
        apply_haptic_params();
        save_settings();
        hjob_tone(&j, TONE_OK);
        hjob_morse(&j, "A");
        ESP_LOGI(TAG, "dashes H%d: %s", U.set.dash_style,
                 U.set.dash_style == HDASH_M2 ? "on M2 (forearm) only, dots on M1" : "sweep M1 -> M2");
        break;
    }
    case 'I': { /* vibration strength (1-9 since v0.6.4): 9 the old maximum, 8 the default; alone: play the level */
        int level = U.set.strength ? U.set.strength : HSTRENGTH_DEFAULT;
        if (!*arg) {
            buf[0] = (char)('0' + level);
            buf[1] = '\0';
            hjob_morse(&j, buf);
            ESP_LOGI(TAG, "vibration strength I%d (drive %d %%, motors at %d mV)", level, hstrength_drive_pct(level),
                     U.motor_mv);
            break;
        }
        uint8_t was = U.set.strength;
        U.set.strength = (uint8_t)(arg[0] - '0');
        if (!apply_strength()) {
            U.set.strength = was; /* the chip refused the voltage: keep the old level */
            apply_strength();
            ok = false;
            break;
        }
        save_settings();
        hjob_tone(&j, TONE_OK); /* at the new strength: you feel the difference straight away */
        hjob_morse(&j, "A");    /* ... and a dot and a dash in it */
        ESP_LOGI(TAG, "vibration strength I%d: drive %d %%, motors at %d mV%s", U.set.strength,
                 hstrength_drive_pct(U.set.strength), U.motor_mv,
                 U.set.strength == HSTRENGTH_MAX && !pmic_present() ? " (no power chip: as I8)" : "");
        break;
    }
    case 'K': /* keep the draft (v0.6.4): no 60 s limit until it is sent or cancelled; K again: the limit back */
        if (!U.draft_len) {
            ok = false; /* nothing to keep */
            ESP_LOGW(TAG, "K: there is no draft to keep");
            break;
        }
        U.draft_kept = !U.draft_kept;
        hjob_tone(&j, TONE_OK);
        hjob_morse(&j, U.draft_kept ? "ON" : "OFF");
        ESP_LOGI(TAG, "draft \"%s\" %s", U.draft,
                 U.draft_kept ? "kept: no time limit until it is sent or cancelled (K again: 60 s)" :
                                "no longer kept: discarded after 60 s untouched");
        break;
    case 'Q': /* reading speed */
        U.set.speed_q = (uint8_t)(arg[0] - '0');
        apply_haptic_params();
        save_settings();
        hjob_tone(&j, TONE_OK);
        ESP_LOGI(TAG, "reading speed Q%d (unit %d ms)", U.set.speed_q, hspeed_unit_ms(U.set.speed_q));
        break;
    case 'R': { /* replay a received message */
        const hist_t *h = hist_get(*arg ? arg[0] - '0' : 1);
        if (!h) {
            ok = false;
            break;
        }
        hjob_morse(&j, id_of(h->src, id));
        hjob_morse(&j, h->text);
        break;
    }
    case 'L': { /* list contacts: slot digit + ID each */
        int len = 0;
        buf[0] = '\0';
        for (int i = 0; i < PEER_SLOTS; i++) {
            if (U.peers.slot[i].used) {
                len += snprintf(buf + len, sizeof(buf) - (size_t)len, "%s%d %s", len ? " " : "", i + 1,
                                id_of(U.peers.slot[i].addr, id));
            }
        }
        if (!len) {
            ok = false;
            break;
        }
        hjob_morse(&j, buf);
        break;
    }
    case 'E': /* read back the draft */
        if (!U.draft_len) {
            ok = false;
            break;
        }
        hjob_morse(&j, U.draft);
        break;
    case 'B': { /* battery %, then the link: OK/error + partner */
        int pct = battery_now();
        if (pct >= 0) {
            snprintf(buf, sizeof buf, "%d", pct);
            hjob_morse(&j, buf);
        } else {
            hjob_morse(&j, "?");
        }
        if (U.link_addr != ADDR_NONE) {
            hjob_tone(&j, U.link_state == LS_UP ? TONE_OK : TONE_ERROR);
            hjob_morse(&j, id_of(U.link_addr, id));
        }
        break;
    }
    case 'C': /* recalibrate */
        calib_enter(false);
        return;
    case 'F': /* switch off: the echo (OFF) was the warning */
        ok = switch_off(); /* returns only if refused */
        break;
    case 'Z': /* silent mode (undiscoverable) */
        U.set.discoverable = !U.set.discoverable;
        save_settings();
        net_set_discoverable(U.set.discoverable); /* the engine answers sweeps (or not) from now on */
        hjob_tone(&j, TONE_OK);
        hjob_morse(&j, U.set.discoverable ? "OFF" : "ON"); /* silent mode on/off */
        ESP_LOGI(TAG, "silent mode %s", U.set.discoverable ? "off (discoverable)" : "on (undiscoverable)");
        break;
    case 'X': /* forget a contact (and tell them), or unblock */
        a = peers_resolve(&U.peers, arg, &slot);
        if (a == ADDR_NONE) {
            ok = false;
        } else if (peers_is_blocked(&U.peers, a)) {
            peers_unblock(&U.peers, a);
            save_peers();
            push_blocklist();
            hjob_tone(&j, TONE_OK);
            hjob_morse(&j, id_of(a, id));
            ESP_LOGI(TAG, "unblocked %s", id);
        } else if (slot >= 0 || a == U.link_addr) {
            if (slot >= 0) {
                peers_remove(&U.peers, slot);
                save_peers();
            }
            net_unpair(a);
            phone_msg_result(PHONE_TX_DROPPED, a); /* the engine drops what was waiting for them */
            if (U.link_addr == a) {
                link_mirror(ADDR_NONE, LS_NONE);
            }
            inreq_remove(a);
            hjob_tone(&j, TONE_OK);
            hjob_morse(&j, id_of(a, id));
            ESP_LOGI(TAG, "forgot %s%s", id, slot >= 0 ? " (its slot is free)" : "");
        } else {
            ok = false;
        }
        break;
    case 'S': /* connect to a contact or an ID */
    case 'P': /* connect to (pair with) a new ID */
        a = peers_resolve(&U.peers, arg, &slot);
        if (a == ADDR_NONE || a == own_addr()) {
            ok = false;
            break;
        }
        if (!U.own_id[0]) {
            ESP_LOGW(TAG, "this device has no ID yet (console: id K7Q)");
            ok = false;
            break;
        }
        if (!net_has_transport()) {
            ESP_LOGW(TAG, "no radio, and loopback is off: nothing to connect over (why: %s)", radio_problem());
            ok = false;
            break;
        }
        if (table_full_for(a)) {
            hjob_tone(&j, TONE_ERROR);
            hjob_morse(&j, "FULL");
            ESP_LOGW(TAG, "contact table full: forget someone with X first");
            break;
        }
        U.req_addr = a;
        U.req_cmd = c;
        ESP_LOGI(TAG, "asking %s to connect (the answer plays when it comes)", id_of(a, id));
        net_link_request(a);
        quiet = true;
        break;
    case 'A': { /* accept a request */
        a = inreq_at(*arg ? arg[0] - '0' : 1);
        if (a == ADDR_NONE) {
            ok = false;
            break;
        }
        if (table_full_for(a)) {
            hjob_tone(&j, TONE_ERROR);
            hjob_morse(&j, "FULL");
            ESP_LOGW(TAG, "contact table full: forget someone with X first (the request still waits)");
            break;
        }
        ESP_LOGI(TAG, "accepting %s (OK plays once they confirm)", id_of(a, id));
        net_link_accept(a);
        inreq_remove(a);
        quiet = true;
        break;
    }
    case 'D': /* discover: who is around? The list plays when the sweep ends (on_net_disc). */
        if (!U.own_id[0]) {
            ESP_LOGW(TAG, "this device has no ID yet (console: id K7Q)");
            ok = false;
            break;
        }
        if (!net_has_transport()) {
            ESP_LOGW(TAG, "no radio, and loopback is off: nobody to look for (why: %s)", radio_problem());
            ok = false;
            break;
        }
        net_discover();
        U.disc_n = 0;
        U.disc_started = now_ms();
        U.st = ST_DISC_SCAN; /* nothing plays until the list comes, in about 4 s */
        ESP_LOGI(TAG, "discovering: who is around? (the list plays in about 4 s; hold 2 s to stop)");
        return;
    case 'N': { /* decline a request; strangers are also blocked */
        a = inreq_at(*arg ? arg[0] - '0' : 1);
        if (a == ADDR_NONE) {
            ok = false;
            break;
        }
        net_link_decline(a);
        inreq_remove(a);
        alert_remove(AL_REQUEST, -1, a);
        if (peers_find(&U.peers, a) < 0) {
            peers_block(&U.peers, a);
            save_peers();
            push_blocklist();
            ESP_LOGI(TAG, "declined and blocked %s (X %s unblocks)", id_of(a, id), id);
        } else {
            ESP_LOGI(TAG, "declined %s", id_of(a, id));
        }
        hjob_tone(&j, TONE_OK);
        break;
    }
    case 'O': /* the owner's phone: 60 s to pair. OK or error plays when the window ends (on_phone). */
        store_save_ble_off(false); /* (pairing means Bluetooth is wanted: W's "off" no longer holds) */
        if (ble_open_window() != ESP_OK) {
            ok = false; /* BLE didn't start (reason logged) */
            break;
        }
        quiet = true;
        break;
    case 'W': { /* Bluetooth off / on (agreed 20 Oct), stored: OK + OFF / ON, like Z */
        int r = ble_switch(-1);
        if (r < 0) {
            ok = false; /* no phone paired (the log says O pairs one) */
            break;
        }
        hjob_tone(&j, TONE_OK);
        hjob_morse(&j, r ? "ON" : "OFF");
        break;
    }
    default: /* V: verification codes (Phase 2) */
        ESP_LOGW(TAG, "command %c is not available in this build yet", c);
        ok = false;
        break;
    }
    if (!ok) {
        hjob_init(&j);
        hjob_tone(&j, TONE_ERROR);
    } else if (quiet) {
        go_resume();
        return;
    }
    play_result(&j);
}

/* command_evt() -- a decoder event in command mode: build up the command. */
static void command_evt(const dec_evt_t *e)
{
    switch (e->kind) {
    case DEC_CHAR:
        if (U.cmd_len >= CMD_MAX) {
            U.tent_armed = false;
            play_tone(TONE_ERROR);
            return;
        }
        U.cmd[U.cmd_len++] = e->c;
        U.cmd[U.cmd_len] = '\0';
        tent_note(e, U.cmd_len - 1);
        ESP_LOGI(TAG, "command: \"%s\"", U.cmd);
        break;
    case DEC_UNKNOWN:
        U.tent_armed = false;
        play_tone(TONE_ERROR);
        break;
    case DEC_SPACE:
        tent_drop(U.cmd, &U.cmd_len);
        break;
    case DEC_ERROR:
        U.tent_active = false;
        if (U.cmd_len) {
            U.cmd[--U.cmd_len] = '\0';
        }
        break;
    case DEC_SOS:
        sos_enter();
        break;
    }
}

/* sos_entry_evt() -- characters keyed after SOS form its target. */
static void sos_entry_evt(const dec_evt_t *e)
{
    switch (e->kind) {
    case DEC_CHAR:
        if (U.sos_arg_len < SOS_ARG_MAX && (id_char_valid(e->c) || e->c == '1')) {
            U.sos_arg[U.sos_arg_len++] = e->c;
            U.sos_arg[U.sos_arg_len] = '\0';
            ESP_LOGW(TAG, "SOS target so far: \"%s\"", U.sos_arg);
        }
        break;
    case DEC_ERROR:
        if (U.sos_arg_len) {
            U.sos_arg[--U.sos_arg_len] = '\0';
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Discovery (D)                                                              */
/* ------------------------------------------------------------------------- */

/* disc_timeout_ms() -- how long D waits for the network's list before
 * giving up: DISC_SCAN_TIMEOUT_MS at SF9, or a sweep's whole window, the
 * longest the DISCOVER may wait to leave, and 3 s to spare, at the slow
 * settings (about 33 s at SF12). */
static int64_t disc_timeout_ms(void)
{
    lora_params_t lp = {U.set.sf, RADIO_BW_HZ, RADIO_CR};
    link_timing_t t;
    link_timing(&lp, &t);
    int64_t need = (int64_t)link_disc_window_ms(&lp) + t.disc_send_hard_ms + 3000;
    return need > DISC_SCAN_TIMEOUT_MS ? need : DISC_SCAN_TIMEOUT_MS;
}

/* disc_active() -- is the discovery list (or its scan) in charge of the key? */
static bool disc_active(void)
{
    return U.st == ST_DISC_SCAN || U.st == ST_DISC_WALK || U.st == ST_DISC_INVITE;
}

/*
 * disc_play_entry() -- offer the current entry: its ID, a word gap and its
 * group letter, e.g. "K7Q N". With after_error, the error tone comes first
 * (the last thing keyed was neither tap nor double tap). The state stays
 * ST_DISC_WALK; the answer is keyed during or after the playback.
 */
static void disc_play_entry(bool after_error)
{
    char id[ID_MAX_LEN + 1], text[ID_MAX_LEN + 3];
    const disc_entry_t *e = &U.disc[U.disc_pos];
    snprintf(text, sizeof text, "%s %c", id_of(e->addr, id), disc_prox_letter(e->prox));
    hjob_t j;
    hjob_init(&j);
    if (after_error) {
        hjob_tone(&j, TONE_ERROR);
    }
    hjob_morse(&j, text);
    U.st = ST_DISC_WALK;
    play_job(&j);
    ESP_LOGI(TAG, "discovery %d/%d: %s, %s (path loss %d dB): tap = invite, double tap = next, hold 2 s = stop",
             U.disc_pos + 1, U.disc_n, id, disc_prox_name(e->prox), disc_loss_db(e));
}

/*
 * disc_end() -- the list is over: finished, stopped by a hold, or left
 * untouched. Anything still playing (an entry, an invite's echo) stops; with
 * `tone` the cancel tone plays; then back to idle / compose (the draft, if
 * any, is untouched). `why` is for the log.
 */
static void disc_end(bool tone, const char *why)
{
    U.disc_n = 0;
    U.disc_stop_tap = false;
    stop_playback();
    dec_reset(&U.dec);
    ESP_LOGI(TAG, "discovery: list closed (%s)", why);
    if (tone) {
        play_result_tone(TONE_CANCEL);
    } else {
        go_resume();
    }
}

/* disc_next() -- double tap (I): offer the next entry; after the last one
 * the list ends. */
static void disc_next(void)
{
    if (U.disc_pos + 1 >= U.disc_n) {
        disc_end(true, "end of the list");
        return;
    }
    U.disc_pos++;
    disc_play_entry(false);
}

/*
 * disc_invite() -- tap (E): echo the device's ID before inviting it, as S
 * echoes its argument. A press during the echo goes back to the list
 * (on_key_down): that catches a double tap keyed too slowly, whose first tap
 * alone reads as E. The request leaves when the echo ends (disc_invite_go).
 */
static void disc_invite(void)
{
    char id[ID_MAX_LEN + 1];
    hjob_t j;
    hjob_init(&j);
    hjob_morse(&j, id_of(U.disc[U.disc_pos].addr, id));
    U.st = ST_DISC_INVITE;
    play_job(&j);
    ESP_LOGI(TAG, "discovery: inviting %s: echoing \"%s\" (press to go back to the list)", id, id);
}

/*
 * disc_invite_go() -- the echo ended untouched: send the request exactly as
 * the command S <ID> would (same checks: no ID, no transport, contact table
 * full -> error + FULL; same result later: OK + slot digit for a new
 * contact, error + BUSY / NO / ID). The list ends here: the link engine
 * keeps one request open at a time, so a second invite would cancel this one.
 */
static void disc_invite_go(void)
{
    char id[ID_MAX_LEN + 1];
    id_of(U.disc[U.disc_pos].addr, id);
    U.disc_n = 0;
    snprintf(U.exec, sizeof U.exec, "S%s", id);
    execute_command();
}

/*
 * disc_walk_evt() -- a decoder event while the list is offered.
 *   E (tap)         invite this one
 *   I (double tap)  next one
 *   word gap        nothing (pauses mean nothing here)
 *   anything else   error + this entry again
 * A lone tap right after a press that stopped the invite's echo is dropped:
 * that press only meant "stop" (it might be the second tap of a slow I), so
 * the entry is simply offered again. SOS never gets here: pump_decoder()
 * takes it first.
 */
static void disc_walk_evt(const dec_evt_t *e)
{
    if (e->kind == DEC_SPACE) {
        return;
    }
    if (U.disc_stop_tap) {
        U.disc_stop_tap = false;
        if (e->kind == DEC_CHAR && e->n_el == 1) {
            char id[ID_MAX_LEN + 1];
            ESP_LOGI(TAG, "discovery: that press only stopped the invite; offering %s again",
                     id_of(U.disc[U.disc_pos].addr, id));
            disc_play_entry(false);
            return;
        }
    }
    if (e->kind == DEC_CHAR && e->c == 'E') {
        disc_invite();
    } else if (e->kind == DEC_CHAR && e->c == 'I') {
        disc_next();
    } else {
        ESP_LOGW(TAG, "discovery: '%c' isn't an answer here (tap = invite, double tap = next, hold 2 s = stop)",
                 e->c ? e->c : '#');
        disc_play_entry(true);
    }
}

/*
 * on_net_disc() -- our sweep's result (LEV_DISC_DONE). The whole list goes
 * to the console with each device's path loss: those are the numbers to set
 * DISC_NEAR_LOSS_DB / DISC_FAR_LOSS_DB from (config.h). Then, if the user is
 * still waiting for it (ST_DISC_SCAN: not stopped meanwhile), the first
 * entry plays; nobody found plays error + 0 (0 is never part of an ID).
 */
static void on_net_disc(const net_evt_t *e)
{
    char id[ID_MAX_LEN + 1];
    int n = e->arg > DISC_MAX ? DISC_MAX : e->arg;
    ESP_LOGI(TAG, "discovery: %d found (near: path loss below %d dB, far: from %d dB)", n, DISC_NEAR_LOSS_DB,
             DISC_FAR_LOSS_DB);
    for (int i = 0; i < n; i++) {
        const disc_entry_t *d = &e->found[i];
        ESP_LOGI(TAG, "  %d. %s %s: path loss %d dB (heard at %d dBm, sent at %d dBm)", i + 1, id_of(d->addr, id),
                 disc_prox_name(d->prox), disc_loss_db(d), d->rssi, d->dbm);
    }
    if (U.st != ST_DISC_SCAN) {
        ESP_LOGI(TAG, "discovery: stopped before the list came: not played");
        return;
    }
    if (U.key_down) {
        U.swallow = true; /* a press already under way wasn't meant for the first entry: ignore it */
    }
    if (n == 0) {
        hjob_t j;
        hjob_init(&j);
        hjob_tone(&j, TONE_ERROR);
        hjob_morse(&j, "0");
        ESP_LOGW(TAG, "discovery: nobody answered (out of range, silent mode, or nobody there)");
        play_result(&j);
        return;
    }
    memcpy(U.disc, e->found, sizeof(disc_entry_t) * (size_t)n);
    U.disc_n = n;
    U.disc_pos = 0;
    U.disc_stop_tap = false;
    dec_reset(&U.dec); /* whatever was tapped while waiting is not an answer */
    disc_play_entry(false);
}

/* ------------------------------------------------------------------------- */
/* Decoder plumbing                                                           */
/* ------------------------------------------------------------------------- */

/* log_dec() -- verbose: one line per decoded character with its timings. */
static void log_dec(const dec_evt_t *e)
{
    if (!U.cal_log) {
        return;
    }
    char els[80];
    int n = 0;
    els[0] = '\0';
    for (int i = 0; i < e->n_el && n < (int)sizeof(els) - 8; i++) {
        n += snprintf(els + n, sizeof(els) - (size_t)n, " %u", e->el_ms[i]);
    }
    static const char *kinds[] = {"", "char", "unknown", "space", "error", "SOS"};
    ESP_LOGI(TAG, "[%s] %c %-9s%s | boundary %.0f gap %.0f word %.0f d'=%.1f", kinds[e->kind], e->c ? e->c : '#',
             e->pattern, els, cal_boundary(&U.cal), cal_letter_threshold(&U.cal), cal_word_threshold(&U.cal),
             cal_dprime(&U.cal));
}

/* pump_decoder() -- hand every pending decoder event to the current state;
 * SOS goes to the SOS flow from any state. */
static void pump_decoder(void)
{
    dec_evt_t e;
    while (dec_get(&U.dec, &e)) {
        log_dec(&e);
        phone_key_char(&e); /* the app: the letter and its pattern, before the draft changes */
        if (e.kind == DEC_CHAR || e.kind == DEC_ERROR) {
            U.cal_dirty = true;
        }
        if (e.kind == DEC_SOS) {
            sos_enter();
            continue;
        }
        switch (U.st) {
        case ST_IDLE:
        case ST_COMPOSE:
            compose_evt(&e);
            break;
        case ST_COMMAND:
            command_evt(&e);
            break;
        case ST_SOS_ENTRY:
            sos_entry_evt(&e);
            break;
        case ST_DISC_WALK:
            disc_walk_evt(&e);
            break;
        default:
            break; /* e.g. while discovering: taps before the list arrives mean nothing */
        }
    }
    if (cal_take_degraded(&U.cal)) {
        alert_t a = {.kind = AL_DEGRADED};
        alert_push(&a, false);
        log_calibration("keying degraded (consider C to recalibrate):");
    }
}

/* ------------------------------------------------------------------------- */
/* Key handling                                                               */
/* ------------------------------------------------------------------------- */

/* action_send() -- the 2 s hold: send / execute / confirm, by state. */
static void action_send(void)
{
    switch (U.st) {
    case ST_COMPOSE:
        start_send();
        break;
    case ST_COMMAND:
        start_echo();
        break;
    case ST_SOS_ENTRY:
        sos_arm();
        break;
    case ST_CALIB:
        calib_evaluate();
        break;
    case ST_DISC_SCAN:
    case ST_DISC_WALK:
    case ST_DISC_INVITE:
        disc_end(true, "stopped with a hold");
        break;
    default:
        play_tone(TONE_ERROR); /* nothing to send */
        break;
    }
}

/* action_command() -- the 3 s hold: enter command mode (the draft stays;
 * a discovery list ends). */
static void action_command(void)
{
    switch (U.st) {
    case ST_DISC_SCAN:
    case ST_DISC_WALK:
    case ST_DISC_INVITE:
        stop_playback();
        U.disc_n = 0;
        ESP_LOGI(TAG, "discovery: list closed (command mode)");
        /* fall through */
    case ST_COMMAND:
        /* (v0.6.4, agreed 24 Oct) The 3 s hold again leaves command mode;
         * the draft stays (the 5 s hold would cancel it). */
        phone_cm_end("left", U.cmd);
        cmd_clear();
        dec_reset(&U.dec);
        U.tent_armed = U.tent_active = false;
        U.st = resume_state();
        play_tone(TONE_OK);
        ESP_LOGI(TAG, "left command mode (3 s hold)%s", U.draft_len ? "; draft kept" : "");
        break;
    case ST_IDLE:
    case ST_COMPOSE:
        cmd_clear();
        dec_reset(&U.dec);
        U.tent_armed = U.tent_active = false;
        U.st = ST_COMMAND;
        ESP_LOGI(TAG, "command mode%s", U.draft_len ? " (draft kept)" : "");
        break;
    default:
        play_tone(TONE_ERROR);
        break;
    }
}

/* action_cancel() -- the 5 s hold: cancel everything, including an active
 * SOS (spec §8). */
static void action_cancel(void)
{
    bool had_sos = U.sos_active;
    if (in_command()) {
        phone_cm_end("cancelled", U.st == ST_CMD_ECHO ? U.exec : U.cmd);
    }
    sos_cancel();
    if (U.st == ST_CALIB) {
        calib_cancel();
        return;
    }
    U.draft_len = 0;
    U.draft[0] = '\0';
    cmd_clear();
    U.sos_arg_len = 0;
    U.tent_armed = U.tent_active = false;
    U.disc_n = 0; /* a discovery list too */
    dec_reset(&U.dec);
    U.st = ST_IDLE;
    ESP_LOGI(TAG, "cancelled%s", had_sos ? " (SOS stopped)" : "");
}

/*
 * on_key_down() -- the key went down at time t (ms).
 * A press acknowledges an SOS alarm (and is used up), stops any playback
 * (aborting an echo, announcement or armed SOS), and otherwise starts an
 * element for the decoder or the calibration capture.
 */
static void on_key_down(int64_t t)
{
    U.key_down = true;
    U.last_key = t;
    U.swallow = false;
    if (U.st == ST_SOS_ALARM) {
        sos_alarm_ack();
        U.swallow = true;
        return;
    }
    bool stopped = false;
    if (U.playing) {
        stop_playback();
        stopped = true;
        switch (U.st) {
        case ST_PLAYBACK:
            U.st = resume_state();
            break;
        case ST_CMD_ECHO:
            ESP_LOGI(TAG, "command aborted");
            phone_cm_end("aborted", U.exec); /* (still in command mode: CM follows) */
            U.st = ST_COMMAND;
            cmd_clear();
            dec_reset(&U.dec);
            break;
        case ST_SEND_ANNOUNCE:
            ESP_LOGI(TAG, "send aborted; draft kept");
            U.st = ST_COMPOSE;
            break;
        case ST_SOS_ARMED:
            ESP_LOGW(TAG, "SOS aborted before sending");
            dec_reset(&U.dec);
            go_resume();
            if (U.st == ST_CALIB) {
                return; /* the prompt restarted; this press is spent */
            }
            break;
        case ST_DISC_INVITE:
            ESP_LOGI(TAG, "discovery: invite stopped; back to the list");
            U.st = ST_DISC_WALK;
            U.disc_stop_tap = true; /* if this press is a lone tap, it only meant "stop" (disc_walk_evt) */
            break;
        default:
            break; /* e.g. an entry of the discovery list: the press is the answer being keyed */
        }
    }
    if (U.st == ST_CALIB) {
        U.cal_capturing = true;
        return;
    }
    if (stopped && (U.st == ST_IDLE || U.st == ST_COMPOSE || U.st == ST_COMMAND)) {
        U.tent_armed = true;
    }
    dec_key_down(&U.dec, t);
    pump_decoder();
}

/*
 * on_key_up() -- the key came up at t after going down at `down` (ms).
 * Short presses are Morse elements; long ones are holds: 2 s send, 3 s
 * command mode, 5 s cancel (1.5-2 s: deliberately nothing).
 */
static void on_key_up(int64_t t, int64_t down)
{
    U.key_down = false;
    U.last_key = t;
    if (U.swallow) {
        U.swallow = false;
        return;
    }
    int64_t dur = t - down;
    if (U.st == ST_SOS_ENTRY) {
        U.sos_ref = t;
    }
    if (dur < HOLD_NOT_MORSE_MS) {
        if (U.st == ST_CALIB) {
            calib_capture(down, t);
            phone_key_press(true);
        } else {
            uint32_t before = U.dec.n_pressed;
            dec_element(&U.dec, down, t);
            pump_decoder(); /* (a KC for the letter before may go first) */
            if (U.dec.n_pressed != before) {
                phone_key_press(false); /* it was an element, not a bounce */
            }
        }
        return;
    }
    if (dur >= HOLD_CANCEL_MS) {
        action_cancel();
    } else if (dur >= HOLD_CMD_MS) {
        action_command();
    } else if (dur >= HOLD_SEND_MS) {
        action_send();
    }
    /* 1.5-2 s: deliberately nothing */
}

/* on_key_hold() -- a hold threshold passed while the key is still down:
 * close the open character at 1.5 s, buzz the feedback at 2 / 3 / 5 s. */
static void on_key_hold(uint8_t level)
{
    if (U.swallow) {
        return;
    }
    switch (level) {
    case HOLD_NOT_MORSE:
        if (disc_active()) {
            /* In the discovery list a hold means "stop": drop whatever was
             * tapped just before it rather than closing it as a character,
             * so a tap followed by a hold can't become an invite. */
            dec_reset(&U.dec);
        } else if (U.st != ST_CALIB) {
            dec_flush(&U.dec);
            pump_decoder();
            if (U.st == ST_COMPOSE || U.st == ST_IDLE) {
                if (tent_drop(U.draft, &U.draft_len) && U.draft_len == 0) {
                    U.st = ST_IDLE;
                }
            } else if (U.st == ST_COMMAND) {
                tent_drop(U.cmd, &U.cmd_len);
            }
        }
        break;
    case HOLD_SEND:
        play_tone(TONE_SEND_ARMED);
        break;
    case HOLD_CMD:
        play_tone(TONE_CMD_ARMED);
        break;
    case HOLD_CANCEL:
        play_tone(TONE_CANCEL);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Playback completion                                                        */
/* ------------------------------------------------------------------------- */

/* on_play_done() -- a playback ended by itself: move the state machine on
 * (execute the echoed command, transmit the announced message, ...). */
static void on_play_done(void)
{
    U.playing = false;
    U.last_play_end = now_ms();
    switch (U.st) {
    case ST_PLAYBACK:
        go_resume();
        break;
    case ST_CMD_ECHO:
        execute_command();
        break;
    case ST_SEND_ANNOUNCE:
        transmit_draft();
        break;
    case ST_SOS_ARMED:
        sos_transmit();
        break;
    case ST_CALIB:
        U.cal_capturing = true;
        break;
    case ST_SOS_ALARM:
        U.alarm_next = now_ms() + SOS_ALARM_REPEAT_MS;
        break;
    case ST_DISC_INVITE:
        disc_invite_go();
        break;
    default:
        break; /* feedback tones; a discovery entry (it waits for tap / double tap) */
    }
}

/* ------------------------------------------------------------------------- */
/* Network events                                                             */
/* ------------------------------------------------------------------------- */

/*
 * on_net_msg() -- a message arrived (or was simulated with `rx`): keep it
 * for R and queue it to play once: attention + sender + text, or the text
 * alone if it continues an active conversation with that device (decided
 * now, on arrival, not when it gets to play). The message itself then
 * counts as the latest exchange.
 */
static void on_net_msg(uint32_t src, const char *text)
{
    char id[ID_MAX_LEN + 1];
    if (peers_is_blocked(&U.peers, src)) {
        return;
    }
    char clean[MSG_MAX_CHARS + 1];
    morse_sanitize(text, clean, sizeof clean);
    hist_add(src, clean);
    char line[PHONE_LINE_MAX + 1];
    if (phone_fmt_rx(line, sizeof line, src, clean) > 0) {
        phone_send(line);
    }
    int64_t now = now_ms();
    alert_t a = {.kind = AL_MSG, .addr = src, .bare = convo_active(&U.convo, src, now)};
    convo_note(&U.convo, src, now);
    strcpy(a.text, clean);
    alert_push(&a, false);
    ESP_LOGI(TAG, "message from %s: \"%s\"%s", id_of(src, id), clean,
             a.bare ? " (active conversation: plays without attention + ID)" : "");
}

/*
 * connected_feedback() -- we are now linked with `addr` (our request was
 * accepted, or our accept confirmed): make them a contact if needed, record
 * the link, and queue OK + slot digit (a new contact, or `P`) or OK + ID.
 */
static void connected_feedback(uint32_t addr, bool via_p)
{
    char id[ID_MAX_LEN + 1];
    bool added;
    int slot = contact_for(addr, &added);
    link_mirror(addr, LS_UP);
    convo_note(&U.convo, addr, now_ms()); /* connecting starts the conversation */
    if ((added || via_p) && slot >= 0) {
        char digit[2] = {(char)('1' + slot), '\0'}; /* slots 0-8 play as 1-9 */
        alert_tone_text(TONE_OK, digit, true);
    } else {
        alert_tone_id(TONE_OK, addr, true);
    }
    ESP_LOGI(TAG, "recipient: %s%s", id_of(addr, id), added ? " (new contact)" : "");
    if (slot >= 0) {
        ESP_LOGI(TAG, "%s is contact %d", id, slot + 1);
    }
}

/*
 * on_net_event() -- everything the network reports (net.c, link.h): what
 * arrived, and what came of the user's requests. Results the user is
 * waiting for are queued at the front of the alerts.
 */
static void on_net_event(const net_evt_t *e)
{
    char id[ID_MAX_LEN + 1];
    id_of(e->addr, id);
    switch (e->kind) {
    case LEV_MSG:
        on_net_msg(e->addr, e->text);
        break;
    case LEV_MSG_SENT: {
        phone_msg_result(PHONE_TX_DELIVERED, e->addr);
        alert_t a = {.kind = AL_TONE, .tone = TONE_OK};
        alert_push(&a, true);
        convo_note(&U.convo, e->addr, now_ms()); /* they have it: that's talking */
        ESP_LOGI(TAG, "delivered to %s", id);
        break;
    }
    case LEV_MSG_FAILED:
        phone_msg_result(e->arg == 1 ? PHONE_TX_REFUSED : PHONE_TX_ALL_FAILED, e->addr);
        if (e->text[0]) {
            restore_draft(e->text);
        }
        /* (the link going down with it is an event of its own since v0.6.0:
         * LEV_LINK_LOST "no-receipt", which plays nothing more) */
        alert_tone_id(TONE_ERROR, e->addr, true);
        ESP_LOGW(TAG, "%s did not confirm the message", id);
        break;
    case LEV_REQ_IN: {
        inreq_add(e->addr);
        phone_event(PHONE_EV_REQUEST, e->addr);
        alert_t a = {.kind = AL_REQUEST, .addr = e->addr};
        alert_push(&a, false);
        ESP_LOGI(TAG, "%s asks to connect: A to accept, N to decline", id);
        break;
    }
    case LEV_REQ_GONE:
        inreq_remove(e->addr);
        alert_remove(AL_REQUEST, -1, e->addr);
        ESP_LOGI(TAG, "%s's request is no longer waiting", id);
        break;
    case LEV_REQ_RESULT: {
        bool mine = e->addr == U.req_addr;
        if (mine) {
            U.req_addr = ADDR_NONE;
        }
        static const uint8_t ev_of[] = {PHONE_EV_LINKED, PHONE_EV_BUSY, PHONE_EV_DECLINED, PHONE_EV_UNAVAILABLE};
        phone_event(e->arg < 4 ? ev_of[e->arg] : PHONE_EV_UNAVAILABLE, e->addr);
        switch (e->arg) {
        case LRES_ACCEPTED:
            connected_feedback(e->addr, mine && U.req_cmd == 'P');
            break;
        case LRES_BUSY:
            alert_tone_text(TONE_ERROR, "BUSY", true);
            ESP_LOGI(TAG, "%s is busy: your request waits in their queue", id);
            break;
        case LRES_DECLINED:
            alert_tone_text(TONE_ERROR, "NO", true);
            ESP_LOGI(TAG, "%s declined", id);
            break;
        default:
            alert_tone_id(TONE_ERROR, e->addr, true);
            ESP_LOGW(TAG, "%s is unavailable (no answer)", id);
            break;
        }
        break;
    }
    case LEV_ACCEPT_RESULT:
        phone_event(e->arg ? PHONE_EV_LINKED : PHONE_EV_FAILED, e->addr);
        if (e->arg) {
            connected_feedback(e->addr, false);
        } else {
            alert_tone_id(TONE_ERROR, e->addr, true);
            ESP_LOGW(TAG, "%s did not confirm: not connected", id);
        }
        break;
    case LEV_LINK_LOST:
        /* No path works any more (LoRa nor the relay). The usual alert,
         * error + ID, unless a failed message just said it (no-receipt). */
        if (e->addr == U.link_addr) {
            U.link_state = LS_LOST;
            if (e->arg != LOST_NO_RECEIPT) {
                alert_link(TONE_ERROR, e->addr);
            }
            phone_event_detail(PHONE_EV_LOST, e->addr, link_lost_why_name(e->arg));
            ESP_LOGW(TAG, "connection to %s lost (%s)", id,
                     e->arg == LOST_NO_RECEIPT ? "a message got no receipt"
                     : e->arg == LOST_ONE_WAY  ? "one-way: they can't hear us, or we can't hear them"
                                               : "nothing heard for the lost time");
        }
        break;
    case LEV_PATH: {
        /* One path (LoRa or the relay) changed. Nothing plays (agreed 15
         * Oct: LoRa alone dropping while the relay carries is for the
         * phone); the phone hears the changes that mean something: a path
         * going down, one-way, or coming back (not a fresh link's
         * "checking" becoming "connected", nor the relay going off with the
         * phone itself). */
        uint8_t path = LINK_EV_PATH(e->arg), st = LINK_EV_STATE(e->arg);
        static uint8_t was[2] = {PS_CHECKING, PS_CHECKING}; /* the last state reported per path (UI task only) */
        static uint32_t was_addr = ADDR_NONE;               /* ... for this partner (a new link starts checking) */
        if (e->addr != was_addr) {
            was_addr = e->addr;
            was[0] = was[1] = PS_CHECKING;
        }
        uint8_t old = was[path & 1];
        was[path & 1] = st;
        bool news = (st == PS_UP || st == PS_THEY_CANT || st == PS_WE_CANT || st == PS_LOST) && old != PS_NONE &&
                    old != PS_OFF && !(old == PS_CHECKING && st == PS_UP);
        if (e->addr == U.link_addr && news) {
            phone_event_detail(path == LP_LORA ? PHONE_EV_LORA : PHONE_EV_RELAY, e->addr, link_path_state_name(st));
        }
        ESP_LOGI(TAG, "%s to %s: %s", link_path_name(path), id, link_path_state_name(st));
        break;
    }
    case LEV_LINK_RESTORED:
        if (e->addr == U.link_addr) {
            U.link_state = LS_UP;
            phone_event(PHONE_EV_RESTORED, e->addr);
            /* Lost and found again before the user even heard about it:
             * say nothing at all. */
            if (alert_remove(AL_LINK, TONE_ERROR, e->addr) == 0) {
                alert_link(TONE_OK, e->addr);
            }
            ESP_LOGI(TAG, "connection to %s restored", id);
        }
        break;
    case LEV_LINK_ENDED:
        if (e->addr == U.link_addr) {
            link_mirror(e->addr, LS_ENDED);
            alert_link(TONE_ERROR, e->addr);
            phone_event(PHONE_EV_ENDED, e->addr);
            ESP_LOGW(TAG, "%s is now linked with someone else", id);
        }
        break;
    case LEV_UNPAIRED: {
        int slot = peers_find(&U.peers, e->addr);
        if (slot >= 0) {
            peers_remove(&U.peers, slot);
            save_peers();
        }
        inreq_remove(e->addr);
        phone_msg_result(PHONE_TX_DROPPED, e->addr); /* the engine dropped what was waiting for them */
        phone_event(PHONE_EV_UNPAIRED, e->addr);
        if (e->addr == U.link_addr) {
            link_mirror(ADDR_NONE, LS_NONE);
            alert_link(TONE_ERROR, e->addr);
        }
        ESP_LOGW(TAG, "%s forgot this device%s", id, slot >= 0 ? ": removed from the contacts" : "");
        break;
    }
    case LEV_SOS:
        on_net_sos(e->addr, e->arg != 0);
        break;
    case LEV_SOS_ACK:
        on_net_sos_ack(e->addr);
        break;
    case LEV_SOS_CANCEL:
        on_net_sos_cancel(e->addr);
        break;
    case LEV_DISC_DONE:
        on_net_disc(e);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Console                                                                    */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Test mode (docs/TEST_PLAN.md; the traffic is ntest.c's, in the net task)   */
/* ------------------------------------------------------------------------- */

#define TEST_BLE_OFF_DELAY_MS 3000 /* rundown-a / -m: Bluetooth off this long after the start (the START line reaches the phone first) */

/*
 * test_motor_play() -- rundown-m (TEST_PLAN §14): play "PARIS" once on the
 * motors, then a word gap, at the reading speed set, so one round is the
 * standard 50 units (10 s at speed 5). Its end (UIE_PLAY_DONE) starts the
 * next round, so the motors keep going until the test stops. This goes
 * around play_job()'s test-mode silence on purpose: rundown-m is the one
 * test that plays.
 */
static void test_motor_play(void)
{
    hparams_t p;
    haptics_get_params(&p);
    hjob_t j;
    hjob_init(&j);
    hjob_morse(&j, "PARIS");
    hjob_gap(&j, (uint16_t)(7u * p.unit_ms * p.farnsworth_x10 / 10u)); /* the word gap, as hseq spaces words */
    U.play_tag = ++U.next_tag;
    U.playing = true;
    U.play_started = now_ms();
    haptics_play(&j, U.play_tag);
}

/* test_key() -- the key while a test runs: only the 5 s hold counts (it
 * stops the test); presses are tracked but not keyed, and nothing plays. */
static void test_key(const key_evt_t *k)
{
    switch (k->kind) {
    case KEY_DOWN:
        U.key_down = true;
        break;
    case KEY_UP:
        U.key_down = false;
        U.last_key = k->t_us / 1000;
        break;
    case KEY_HOLD:
        if (k->level == HOLD_CANCEL) {
            ESP_LOGW(TAG, "5 s hold: stopping the test");
            net_test_stop("hold");
        }
        break;
    }
}

/*
 * on_test() -- the net task says test mode is on or off (UIE_TEST). On:
 * whatever plays stops, queued alerts and half-keyed input are dropped;
 * from now on nothing plays (play_job) and alerts aren't queued
 * (alert_push), except that rundown-m starts its motor loop. Off: the key
 * works again, the motor loop stops, Bluetooth comes back if rundown-a / -m
 * switched it off, and the cancel tone says the test is over.
 */
static void on_test(bool on, uint8_t test)
{
    if (on) {
        stop_playback();
        U.test = true;
        U.test_kind = test;
        U.test_since = now_ms();
        U.al_n = 0;
        U.al_head = 0;
        cmd_clear();
        dec_reset(&U.dec);
        if (U.st != ST_IDLE && U.st != ST_COMPOSE) {
            U.st = U.draft_len ? ST_COMPOSE : ST_IDLE;
        }
        int pct = battery_now();
        net_test_battery(pct >= 0 ? U.bat_mv : -1, pct, U.bat_charging); /* the run-down's first sample (motors still) */
        if (tt_plays(test)) {
            ESP_LOGW(TAG, "test mode on (%s): PARIS on the motors until it stops; the key only stops it (hold 5 s)",
                     tt_test_name(test));
            test_motor_play();
        } else {
            ESP_LOGW(TAG, "test mode on (%s): nothing plays, the key only stops it (hold 5 s)", tt_test_name(test));
        }
        return;
    }
    if (!U.test) {
        return;
    }
    U.test = false;
    stop_playback(); /* rundown-m's motor loop */
    dec_reset(&U.dec);
    U.last_key = now_ms();
    if (U.test_ble_off) {
        U.test_ble_off = false;
        if (ble_wanted()) {
            ble_start(); /* the paired phone reconnects by itself (unless W switched Bluetooth off) */
        }
    }
    ESP_LOGW(TAG, "test mode off: the device works as usual again");
    play_tone(TONE_CANCEL);
}

/* test_timers() -- run_timers() while a test runs: the battery (for the
 * STAT lines and the run-down log), rundown-m's motor loop and the
 * Bluetooth switch-off of rundown-a / -m. */
static void test_timers(int64_t now)
{
    bool plays = tt_plays(U.test_kind);
    /* rundown-m: a running motor pulls the battery voltage down a little,
     * so the reading waits for a moment with both motors still (one comes
     * every few hundred ms; this runs at least every 250 ms). */
    if (now >= U.bat_next && !(plays && haptics_motors_on())) {
        U.bat_next = now + BATTERY_POLL_MS;
        int pct = battery_now();
        if (pct >= 0) {
            U.bat_pct = (U.bat_pct < 0) ? pct : (U.bat_pct * 3 + pct) / 4;
        }
        net_test_battery(pct >= 0 ? U.bat_mv : -1, pct >= 0 ? U.bat_pct : -1, U.bat_charging);
    }
    if (plays) {
        if (U.playing && !haptics_busy() && now - U.play_started > 100) {
            U.playing = false; /* the round ended but its done event was lost */
        }
        if (!U.playing) {
            test_motor_play(); /* the next round */
        }
    }
    if (tt_ble_off(U.test_kind) && !U.test_ble_off && now - U.test_since >= TEST_BLE_OFF_DELAY_MS) {
        ble_status_t bs;
        ble_get_status(&bs);
        if (bs.started) {
            ESP_LOGW(TAG, "%s: Bluetooth off for the run (on again when the test ends)", tt_test_name(U.test_kind));
            ble_stop();
            U.test_ble_off = true;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Bench tests (bench.h; docs/TEST_PLAN.md §14.1, §14.3)                      */
/* ------------------------------------------------------------------------- */

/* bench_skip() -- is BENCH_POWER[i] a step this board can't do (22 dBm on
 * an SX1276)? */
static bool bench_skip(int i)
{
    return bench_skips(i, radio_tx_dbm_max());
}

/* bench_phone() -- a BN line to the phone, if one is connected (phone.h):
 * what the meter session is doing. Lines while no phone is there are simply
 * not sent: the app runs the same timetable by itself. */
static void bench_phone(const char *what, const char *detail)
{
    char line[PHONE_LINE_MAX + 1];
    if (phone_fmt_bench(line, sizeof line, what, detail) > 0) {
        phone_send(line);
    }
}

/* bench_usb_in() -- is USB plugged in (the power chip sees VBUS)? Without a
 * power chip (the emulator) the answer is no. */
static bool bench_usb_in(void)
{
    pmic_status_t ps;
    return pmic_read_status(&ps) == ESP_OK && ps.vbus;
}

/* bench_begin() -- USB is out (or never was): the timetable starts. The
 * phone hears the total (BN start; the app counts the steps from here),
 * then BENCH_SETTLE_S for the meter, then step 1's signal. */
static void bench_begin(int64_t now)
{
    char d[24];
    snprintf(d, sizeof d, "%lu %d", (unsigned long)bench_power_secs(radio_tx_dbm_max()), radio_tx_dbm_max());
    bench_phone("start", d);
    ESP_LOGW(TAG, "bench power: USB out; step 1 in %d s", BENCH_SETTLE_S);
    U.bench_phase = BPH_SETTLE;
    U.bench_until = now + (int64_t)BENCH_SETTLE_S * 1000;
}

/*
 * bench_power_start() -- `bench power` (or BENCH POWER from the phone:
 * from_phone): print the plan (it is also the sheet to fill in), then wait
 * for USB to be unplugged, however long that takes (agreed 20 Oct: no
 * fallback time any more; a 5 s hold or `bench stop` cancels). With no USB
 * in (the phone's start: the board on its battery and the meter) it begins
 * at once. Refused during a test, during our SOS (step 6 switches off,
 * which would stop it), or while it already runs; the phone hears why (BN
 * refused).
 */
static void bench_power_start(int64_t now, bool from_phone)
{
    const char *no = U.test ? "test" : U.sos_active ? "sos" : U.bench ? "running" : NULL;
    if (no) {
        printf("bench power: not now (%s)\n", U.test ? "a test is running" : U.sos_active ? "your SOS is active"
                                                                                      : "already running");
        bench_phone("refused", no);
        return;
    }
    stop_playback();
    printf("bench power%s: the board steps through these states by itself; motor M1 buzzes each step's number before "
           "it (one short buzz between the parts of a step):\n",
           from_phone ? " (started from the phone)" : "");
    for (int i = 0; i < bench_power_n; i++) {
        const bench_step_t *st = &BENCH_POWER[i];
        printf("  %-3s %-72s %s\n", st->id, st->what,
               bench_skip(i) ? "(skipped: this chip's most is 20 dBm)"
               : st->secs  ? ""
                           : "(until you hold the key 2 s)");
        if (st->secs && !bench_skip(i)) {
            printf("      %u s\n", st->secs);
        }
    }
    U.bench = true;
    U.bench_i = -1;
    U.bench_last_step = 0;
    U.al_n = 0; /* nothing queued plays afterwards either */
    U.al_head = 0;
    if (bench_usb_in()) {
        printf("about %lu s in all. Unplug USB now (the meter in series with the battery): step 1 starts %d s after "
               "USB is out. A 5 s hold, or `bench stop`, ends it.\n",
               (unsigned long)bench_power_secs(radio_tx_dbm_max()), BENCH_SETTLE_S);
        U.bench_phase = BPH_WAIT_USB;
        bench_phone("wait-usb", NULL);
    } else {
        printf("about %lu s in all. A 5 s hold, or `bench stop`, ends it.\n",
               (unsigned long)bench_power_secs(radio_tx_dbm_max()));
        bench_begin(now);
    }
}

/* bench_undo() -- put back what bench power changed: motors off (and the
 * test LED dark), the radio receiving at its usual power and the stored
 * spreading factor (steps 2-4 used SF9 and SF12), advertising connectable
 * again, Bluetooth as the user has it (on if a phone is paired, unless W
 * switched it off). */
static void bench_undo(void)
{
    haptics_raw(0, 0);
    net_bench(NET_BENCH_OFF, 0);
    net_set_sf(U.set.sf);
    ble_set_connectable(true);
    if (ble_wanted()) {
        ble_start();
    } else {
        ble_stop();
    }
}

/* bench_power_stop() -- a 5 s hold ("hold"), `bench stop` ("stopped") or
 * the phone's BENCH STOP ("app"): end early, the phone hears how (BN end),
 * undo, and the cancel tone says it's over. */
static void bench_power_stop(const char *how)
{
    if (!U.bench) {
        return;
    }
    U.bench = false;
    bench_phone("end", how);
    bench_undo();
    U.last_key = now_ms();
    ESP_LOGW(TAG, "bench power stopped (%s)", how);
    play_tone(TONE_CANCEL);
}

/* bench_apply() -- the state of step i begins (its signal is over): the
 * phone hears which (BN step id secs) before anything changes, so it still
 * gets the line when the step switches Bluetooth off. */
static void bench_apply(int i)
{
    const bench_step_t *st = &BENCH_POWER[i];
    char d[16];
    snprintf(d, sizeof d, "%s %u", st->id, st->secs);
    bench_phone("step", d);
    ESP_LOGW(TAG, "bench power step %s: %s", st->id, st->what);
    switch (st->kind) {
    case BS_CPU:
        net_bench(NET_BENCH_STANDBY, 0);
        ble_flush(300); /* (the BN line out first) */
        ble_stop();
        break;
    case BS_KEYING:
        break; /* nothing changes on the board: the owner keys (1b) or holds the key (1c) */
    case BS_LISTEN:
        net_set_sf(RADIO_SF_MIN); /* SF9 for the table, whatever the stored level (put back at the end) */
        net_bench(NET_BENCH_LISTEN, 0);
        break;
    case BS_BLE_ADV:
        ble_set_connectable(false); /* advertising alone: no phone can connect */
        if (ble_start() != ESP_OK) {
            ESP_LOGW(TAG, "bench: Bluetooth didn't start");
        }
        break;
    case BS_BLE_CONN:
        ble_set_connectable(true); /* the paired phone reconnects by itself (the app tries at once) */
        if (!store_load_ble_on()) {
            ESP_LOGW(TAG, "bench: no phone is paired, so nobody will connect (O pairs one)");
        }
        break;
    case BS_RADIO_STBY:
        net_bench(NET_BENCH_STANDBY, 0);
        break;
    case BS_LISTEN_SF12:
        net_set_sf(RADIO_SF_MAX);
        net_bench(NET_BENCH_LISTEN, 0);
        break;
    case BS_CARRIER:
        net_set_sf(RADIO_SF_MIN); /* (4a: back to SF9; a carrier has no SF, but listening between them does) */
        net_bench(NET_BENCH_CARRIER, st->dbm);
        break;
    case BS_MOTOR:
        haptics_raw(st->m1, st->m2);
        break;
    case BS_OFF:
        U.bench = false;
        bench_phone("end", "off");
        ble_flush(300);
        net_bench(NET_BENCH_OFF, 0);
        vTaskDelay(pdMS_TO_TICKS(100)); /* (the net task gives the radio back first) */
        switch_off();                    /* (refused only during our SOS: ruled out at the start) */
        break;
    }
}

/* bench_end_step() -- the state of step i ends: carrier off (listening
 * again), motors off. */
static void bench_end_step(int i)
{
    const bench_step_t *st = &BENCH_POWER[i];
    if (st->kind == BS_CARRIER) {
        net_bench(NET_BENCH_LISTEN, 0);
    } else if (st->kind == BS_MOTOR) {
        haptics_raw(0, 0);
    }
}

/* bench_next() -- on to the next step this board can do: its signal first
 * (the step's number in buzzes, or one short buzz between parts of a step). */
static void bench_next(int64_t now)
{
    do {
        U.bench_i++;
    } while (U.bench_i < bench_power_n && bench_skip(U.bench_i));
    if (U.bench_i >= bench_power_n) {
        bench_power_stop("done"); /* (the last step switches off, so this only follows a skipped one) */
        return;
    }
    int step = BENCH_POWER[U.bench_i].step;
    U.bench_edges = step == U.bench_last_step ? 2 : 2 * step; /* each buzz is on + off */
    U.bench_phase = BPH_SIGNAL;
    U.bench_led = false;
    U.bench_until = now; /* the first edge at once */
}

/*
 * bench_timers() -- run_timers() while bench power runs: wait for USB to
 * go, settle, then for each step signal, pause, run. Timing is to the ms
 * (the UI loop wakes for U.bench_until, ui_task), and matches
 * bench_signal_ms() and the app's timetable. The signal is motor M1 at
 * full (agreed 20 Oct; the LED until v0.6.0): bench_led means "buzzing".
 */
static void bench_timers(int64_t now)
{
    switch (U.bench_phase) {
    case BPH_WAIT_USB:
        if (!bench_usb_in()) {
            bench_begin(now);
        }
        break;
    case BPH_SETTLE:
        if (now >= U.bench_until) {
            bench_next(now);
        }
        break;
    case BPH_SIGNAL:
        if (now < U.bench_until) {
            break;
        }
        if (U.bench_edges > 0) {
            bool between = BENCH_POWER[U.bench_i].step == U.bench_last_step;
            U.bench_led = !U.bench_led;
            haptics_raw(U.bench_led ? BENCH_SIGNAL_LEVEL : 0, 0);
            U.bench_edges--;
            int ms = between ? (U.bench_led ? BENCH_SUB_BLINK_MS : BENCH_SUB_PAUSE_MS)
                             : (U.bench_edges == 0 ? BENCH_PAUSE_MS : BENCH_BLINK_MS);
            U.bench_until = now + ms;
            break;
        }
        U.bench_last_step = BENCH_POWER[U.bench_i].step;
        U.bench_phase = BPH_RUN;
        U.bench_until = now + (int64_t)BENCH_POWER[U.bench_i].secs * 1000;
        bench_apply(U.bench_i);
        break;
    case BPH_RUN:
        if (now >= U.bench_until) {
            bench_end_step(U.bench_i);
            bench_next(now);
        }
        break;
    }
}

/* bench_key() -- the key during bench power: only a 5 s hold counts (it
 * stops the bench test); presses are tracked but not keyed (in steps 1b and
 * 1c the keying is what is measured), and no hold feedback plays. */
static void bench_key(const key_evt_t *k)
{
    if (k->kind == KEY_DOWN) {
        U.key_down = true;
    } else if (k->kind == KEY_UP) {
        U.key_down = false;
        U.last_key = k->t_us / 1000;
    } else if (k->kind == KEY_HOLD && k->level == HOLD_CANCEL) {
        ESP_LOGW(TAG, "5 s hold: stopping bench power");
        bench_power_stop("hold");
    }
}

/* bench_motor_go() -- `bench motor [m1|m2]`: play the phyphox pattern
 * (bench.h) on one motor, as an ordinary playback (a press stops it). */
static void bench_motor_go(int motor)
{
    if (U.test || U.bench) {
        printf("bench motor: not now (%s)\n", U.test ? "a test is running" : "bench power is running");
        return;
    }
    stop_playback();
    bench_motor_start(&U.bench_mot, motor);
    U.play_tag = ++U.next_tag;
    U.playing = true;
    U.play_started = now_ms();
    U.st = ST_PLAYBACK; /* (its end goes back to idle / compose, as any result) */
    haptics_play_gen(bench_motor_next, &U.bench_mot, U.play_tag);
    printf("bench motor: M%d plays the pattern for about %lu s (a 1 s buzz first; a press stops it)\n", motor == 2 ? 2 : 1,
           (unsigned long)((bench_motor_ms() + 999) / 1000));
}

/* print_phone() -- the phone part of `status` (also `phone` on its own):
 * BLE on or off, who is connected, the O window, lines and relayed packets. */
static void print_phone(void)
{
    ble_status_t bs;
    ble_get_status(&bs);
    net_status_t ns;
    net_get_status(&ns);
    char line[200], paired[40] = "";
    int n;
    if (bs.bonds == 0) {
        snprintf(paired, sizeof paired, ", no phone paired");
    } else if (bs.bonds > 0) {
        snprintf(paired, sizeof paired, ", %d phone%s paired", bs.bonds, bs.bonds == 1 ? "" : "s");
    }
    if (bs.sim) {
        n = snprintf(line, sizeof line, "phone: the console's pretend phone (phone sim off ends it)");
    } else if (!bs.started && store_load_ble_on() && store_load_ble_off()) {
        n = snprintf(line, sizeof line, "phone: BLE off (W)%s (W or `phone on` turns it on again)", paired);
    } else if (!bs.started) {
        n = snprintf(line, sizeof line, "phone: BLE off (O pairs a phone; it then stays on)");
    } else {
        n = snprintf(line, sizeof line, "phone: BLE on as \"%s\"%s | %s", bs.name, paired,
                     !bs.connected   ? (bs.advertising ? "waiting for the phone" : "not advertising")
                     : !bs.accepted  ? "a phone is connected, not accepted (yet)"
                     : !bs.subscribed ? "phone connected, not listening yet"
                                      : "phone connected");
        if (bs.connected && n < (int)sizeof line) {
            n += snprintf(line + n, sizeof line - (size_t)n, " (MTU %u)", bs.mtu);
        }
    }
    if (bs.window_s && n < (int)sizeof line) {
        snprintf(line + n, sizeof line - (size_t)n, " | pairing window open, %ld s left", (long)bs.window_s);
    }
    printf("%s\n", line);
    printf("phone lines: in %lu, out %lu, dropped %lu | packets through the phone: out %lu, in %lu\n",
           (unsigned long)bs.lines_in, (unsigned long)bs.lines_out, (unsigned long)bs.lines_dropped,
           (unsigned long)ns.phone_out, (unsigned long)ns.phone_in);
}

/* print_status() -- `status` in the console. Lines that are assembled from
 * several parts are built in a buffer first and printed with one call, so
 * the console's prompt can't land in the middle of them. */
static void print_status(void)
{
    char id[ID_MAX_LEN + 1], id2[ID_MAX_LEN + 1], line[200];
    int n;
    const cal_profile_t *p = cal_profile(&U.cal);
    static const char *health[] = {"not calibrated", "healthy", "marginal (boundary frozen)", "DEGRADED"};
    printf("\nMorseLink %s | state: %s | recipient: %s (%s)\n", U.own_id[0] ? U.own_id : "(no ID set)",
           ST_NAMES[U.st], U.link_addr == ADDR_NONE ? "-" : id_of(U.link_addr, id), link_state_name(U.link_state));
    net_status_t ns;
    net_get_status(&ns);
    n = snprintf(line, sizeof line, "link: ");
    if (ns.peer != ADDR_NONE) {
        n += snprintf(line + n, sizeof line - (size_t)n, "%s %s", id_of(ns.peer, id), link_state_name(ns.state));
        if (ns.heard_ago_s >= 0) {
            n += snprintf(line + n, sizeof line - (size_t)n, ", heard %ld s ago", (long)ns.heard_ago_s);
        }
        if (ns.peer_rssi) {
            n += snprintf(line + n, sizeof line - (size_t)n, " at %d dBm (SNR %.1f dB)", ns.peer_rssi,
                          ns.peer_snr_x4 / 4.0);
        }
        n += snprintf(line + n, sizeof line - (size_t)n, "%s", ns.busy ? ", busy (message in the last 10 min)" : "");
    } else {
        n += snprintf(line + n, sizeof line - (size_t)n, "none");
    }
    if (ns.req_phase && n < (int)sizeof line) {
        n += snprintf(line + n, sizeof line - (size_t)n, " | our request to %s: %s", id_of(ns.req_to, id2),
                      link_req_phase_name(ns.req_phase));
    }
    if (ns.peer != ADDR_NONE && (ns.state == LS_UP || ns.state == LS_LOST)) {
        /* The two paths (v0.6.0): what we heard, and what the partner reports. */
        char p_line[200];
        int k = snprintf(p_line, sizeof p_line, "paths:");
        for (uint8_t p = LP_LORA; p <= LP_RELAY; p++) {
            k += snprintf(p_line + k, sizeof p_line - (size_t)k, "%s %s %s", p ? " |" : "", link_path_name(p),
                          link_path_state_name(ns.path_st[p]));
            if (ns.path_st[p] != PS_OFF && k < (int)sizeof p_line) {
                if (ns.heard_s[p] >= 0) {
                    k += snprintf(p_line + k, sizeof p_line - (size_t)k, " (heard %ld s ago", (long)ns.heard_s[p]);
                } else {
                    k += snprintf(p_line + k, sizeof p_line - (size_t)k, " (not heard yet");
                }
                if (k < (int)sizeof p_line) {
                    k += ns.they_s[p] >= 0
                             ? snprintf(p_line + k, sizeof p_line - (size_t)k, ", they heard us %ld s ago)", (long)ns.they_s[p])
                             : snprintf(p_line + k, sizeof p_line - (size_t)k, ns.reports ? ", they haven't heard us)"
                                                                                           : ", no report: older firmware)");
                }
            }
        }
        if (ns.state == LS_LOST && k < (int)sizeof p_line) {
            snprintf(p_line + k, sizeof p_line - (size_t)k, " | lost: %s", link_lost_why_name(ns.lost_why));
        }
        printf("%s\n", p_line);
    }
    if (n < (int)sizeof line) {
        snprintf(line + n, sizeof line - (size_t)n, " | messages waiting for a receipt: %d", ns.n_msg);
    }
    printf("%s\n", line);
    n = snprintf(line, sizeof line, "requests waiting for you:");
    for (int i = 0; i < U.n_inreq; i++) {
        n += snprintf(line + n, sizeof line - (size_t)n, " %d=%s", i + 1, id_of(U.inreq[i], id));
    }
    printf("%s%s\n", line, U.n_inreq ? " (A [n] accepts, N [n] declines)" : " none");
    if (U.convo.addr != ADDR_NONE) {
        int64_t now = now_ms();
        printf("conversation: %s, last exchange %ld s ago: %s\n", id_of(U.convo.addr, id),
               (long)((now - U.convo.at_ms) / 1000),
               convo_active(&U.convo, U.convo.addr, now) ? "their messages play without attention + ID"
                                                          : "over (more than 5 min): messages are announced");
    } else {
        printf("conversation: none yet (messages are announced)\n");
    }
    if (ns.loopback) {
        printf("radio: LOOPBACK test network (a pretend partner, no radio)%s\n",
               ns.radio_ok ? "; the radio is idle" : "");
    } else if (ns.radio_ok) {
        printf("radio: %s %.3f MHz SF%d (range level %d) BW%lu, %d dBm (max %d) | duty cycle %.2f %% (limit %d %%) | "
               "noise %d dBm\n",
               radio_chip_name(), RADIO_FREQ_HZ / 1e6, ns.sf, ns.sf - RADIO_SF_MIN + 1, (unsigned long)(RADIO_BW_HZ / 1000),
               ns.tx_dbm, ns.tx_dbm_max, ns.duty_used_ms / 36000.0, RADIO_DUTY_PERMILLE / 10, ns.noise_dbm);
    } else {
        printf("radio: NOT FOUND: %s (loopback on to test on one board)\n", radio_problem());
    }
    printf("packets: sent %lu, received %lu (duplicates %lu, not ours %lu, damaged %lu)\n", (unsigned long)ns.pkts_tx,
           (unsigned long)ns.pkts_rx, (unsigned long)ns.pkts_dup, (unsigned long)ns.pkts_bad,
           (unsigned long)ns.pkts_crc);
    print_phone();
    printf("calibration: %s", health[cal_health(&U.cal)]);
    if (cal_valid(&U.cal)) {
        printf(" | dot %.0f ms (mad %.0f) dash %.0f ms (mad %.0f) ratio %.2f d'=%.1f alpha %.3f\n", p->dot_mean,
               p->dot_mad, p->dash_mean, p->dash_mad, p->ratio, cal_dprime(&U.cal), cal_alpha(&U.cal));
        printf("             boundary %.0f ms, letter gap > %.0f ms, word gap > %.0f ms (gaps: intra %.0f, letter %.0f)\n",
               cal_boundary(&U.cal), cal_letter_threshold(&U.cal), cal_word_threshold(&U.cal), p->gi_mean,
               p->gl_mean);
    } else {
        printf("\n");
    }
    printf("reading: Q%d (U=%d ms), farnsworth x%.1f, dash M1 %.2fU / M2 %.2fU (together %.2fU), intensity M1 %d%% "
           "M2 %d%%, rail gating %s\n",
           U.set.speed_q, hspeed_unit_ms(U.set.speed_q), U.set.farnsworth_x10 / 10.0,
           (U.set.dash_m1_cu + U.set.overlap_cu / 2) / 100.0, (300 - U.set.dash_m1_cu + U.set.overlap_cu / 2) / 100.0,
           U.set.overlap_cu / 100.0, U.set.m1_pct, U.set.m2_pct, U.set.rail_gating ? "on" : "off");
    printf("dashes: H%d (%s)\n", U.set.dash_style == HDASH_M2 ? 2 : 1,
           U.set.dash_style == HDASH_M2 ? "M2 only, dots on M1" : "sweep M1 -> M2");
    printf("strength: I%d (drive %d %%)%s, motor supply (DCDC1) %d mV\n",
           U.set.strength ? U.set.strength : HSTRENGTH_DEFAULT, hstrength_drive_pct(U.set.strength),
           U.set.strength ? "" : " (never set: as the board starts)", U.motor_mv);
    if (U.draft_len) {
        printf("draft kept (K): %s\n", U.draft_kept ? "yes, no time limit" : "no, discarded after 60 s untouched");
    }
    n = snprintf(line, sizeof line, "contacts:");
    int used = 0;
    for (int i = 0; i < PEER_SLOTS; i++) {
        if (U.peers.slot[i].used) {
            n += snprintf(line + n, sizeof line - (size_t)n, " %d=%s", i + 1, id_of(U.peers.slot[i].addr, id));
            used++;
        }
    }
    n += snprintf(line + n, sizeof line - (size_t)n, "%s | blocked:", used ? "" : " none");
    for (int i = 0; i < U.peers.n_blocked && n < (int)sizeof line - 6; i++) {
        n += snprintf(line + n, sizeof line - (size_t)n, " %s", id_of(U.peers.blocked[i], id));
    }
    printf("%s%s | silent mode %s\n", line, U.peers.n_blocked ? "" : " none", U.set.discoverable ? "off" : "on");
    n = snprintf(line, sizeof line, "discovery: %s; sweeps answered %lu, left unanswered %lu",
                 U.set.discoverable ? "answering other people's D" : "silent mode: not answering",
                 (unsigned long)ns.disc_answered, (unsigned long)ns.disc_refused);
    if (U.st == ST_DISC_SCAN) {
        snprintf(line + n, sizeof line - (size_t)n, " | D: waiting for answers");
    } else if (U.disc_n && (U.st == ST_DISC_WALK || U.st == ST_DISC_INVITE)) {
        snprintf(line + n, sizeof line - (size_t)n, " | D: list entry %d of %d (%s %s)", U.disc_pos + 1, U.disc_n,
                 id_of(U.disc[U.disc_pos].addr, id), disc_prox_name(U.disc[U.disc_pos].prox));
    }
    printf("%s\n", line);
    int pct = battery_now();
    if (pct >= 0) {
        printf("battery: %d mV, %d%%%s\n", U.bat_mv, pct, U.bat_charging ? " (charging)" : "");
    } else {
        printf("battery: no reading\n");
    }
    if (U.test) {
        printf("TEST MODE: %s, %ld s so far (nothing plays; hold the key 5 s, or `test stop`, to end it)\n",
               tt_test_name(U.test_kind), (long)((now_ms() - U.test_since) / 1000));
    }
    printf("draft: \"%s\" | command: \"%s\" | alerts queued: %d | history: %d | SOS: %s\n", U.draft, U.cmd, U.al_n,
           U.hist_n, U.sos_active ? "ACTIVE" : "off");
    printf("tasks: least free stack ever, ui %lu B, net %lu B | RAM free %u B (least ever %u B)\n\n",
           (unsigned long)uxTaskGetStackHighWaterMark(NULL), (unsigned long)ns.stack_free,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

/* restore_link_at_boot() -- give the network the stored link, unless it had
 * ended (the partner moved on: the ID is kept only for "error + ID"). */
static void restore_link_at_boot(void)
{
    if (U.link_addr != ADDR_NONE && U.link_state != LS_ENDED) {
        net_set_link(U.link_addr);
    }
}

/* on_console() -- run a console command inside the UI task. */
static void on_console(const ui_evt_t *e)
{
    char id[ID_MAX_LEN + 1];
    int32_t a = e->con.a, b = e->con.b;
    switch ((con_op_t)e->con.op) {
    case CON_STATUS:
        print_status();
        break;
    case CON_PLAY_TEXT: {
        hjob_t j;
        hjob_init(&j);
        hjob_morse(&j, e->con.text);
        if (U.st == ST_IDLE || U.st == ST_COMPOSE || U.st == ST_PLAYBACK) {
            play_result(&j);
        } else {
            printf("busy (%s)\n", ST_NAMES[U.st]);
        }
        break;
    }
    case CON_TONE:
        if (a >= 0 && a < TONE__COUNT) {
            play_tone((tone_t)a);
        }
        break;
    case CON_RX:
        on_net_msg((uint32_t)a, e->con.text);
        break;
    case CON_SIM_SOS:
        on_net_sos((uint32_t)a, b != 0);
        break;
    case CON_SIM_SOS_CANCEL:
        on_net_sos_cancel((uint32_t)a);
        break;
    case CON_SET_ID:
        if (store_save_id(e->con.text) == ESP_OK) {
            store_load_id(U.own_id);
            net_set_identity(own_addr()); /* the protocol engine restarts with the new address */
            phone_msg_result(PHONE_TX_ALL_FAILED, ADDR_NONE); /* ... and forgets any message still waiting */
            restore_link_at_boot();
            ble_set_name(U.own_id);       /* advertised as "MorseLink-<ID>" */
            phone_hello();                /* a connected phone learns the new ID */
            printf("device ID set to %s\n", U.own_id);
        } else {
            printf("invalid ID (3 characters from %s)\n", ID_ALPHABET);
        }
        break;
    case CON_PEER_ADD: {
        uint32_t addr = id_to_addr(e->con.text);
        int s = addr == ADDR_NONE ? -1 : peers_add(&U.peers, addr);
        if (s >= 0) {
            save_peers();
            printf("%s is contact %d\n", id_of(addr, id), s + 1);
        } else {
            printf(addr == ADDR_NONE ? "invalid ID\n" : "contact table full\n");
        }
        break;
    }
    case CON_PEER_DEL: {
        uint32_t addr = peers_addr(&U.peers, a - 1);
        if (peers_remove(&U.peers, a - 1)) {
            save_peers();
            if (U.link_addr == addr) {
                link_select(ADDR_NONE);
            }
            printf("slot %ld cleared\n", (long)a);
        } else {
            printf("slot %ld is empty\n", (long)a);
        }
        break;
    }
    case CON_LOOPBACK:
        /* Not saved (U.set.loopback stays 0): it ends at the next restart or
         * switch-off. While on, the real radio is ignored. */
        net_set_loopback(a != 0);
        printf("loopback %s\n", a ? "on until the next restart: a pretend partner answers instead of the radio, "
                                    "and real radio packets are ignored"
                                  : "off");
        break;
    case CON_CAL_LOG:
        U.cal_log = a != 0;
        printf("verbose log %s\n", a ? "on" : "off");
        break;
    case CON_SPEED:
        if (a >= 1 && a <= 9) {
            U.set.speed_q = (uint8_t)a;
            apply_haptic_params();
            save_settings();
            printf("reading speed Q%ld (U=%d ms)\n", (long)a, hspeed_unit_ms((int)a));
        }
        break;
    case CON_HAPTIC:
        switch (a) {
        case HP_OVERLAP:
            U.set.overlap_cu = (uint8_t)(b < 0 ? 0 : b > 100 ? 100 : b); /* (hundredths of U since v0.6.4) */
            break;
        case HP_SPLIT:
            U.set.dash_m1_cu = (uint8_t)(b < 25 ? 25 : b > 250 ? 250 : b);
            break;
        case HP_M1:
            U.set.m1_pct = (uint8_t)(b < 0 ? 0 : b > 100 ? 100 : b);
            break;
        case HP_M2:
            U.set.m2_pct = (uint8_t)(b < 0 ? 0 : b > 100 ? 100 : b);
            break;
        case HP_FARNSWORTH:
            U.set.farnsworth_x10 = (uint8_t)(b < 10 ? 10 : b > 40 ? 40 : b);
            break;
        case HP_GATING:
            U.set.rail_gating = b ? 1 : 0;
            break;
        case HP_DASH: /* as H (v0.6.3) */
            if (b == HDASH_SWEEP || b == HDASH_M2) {
                U.set.dash_style = (uint8_t)b;
            } else {
                printf("haptic dash 1 (sweep M1 -> M2) | 2 (M2 only)\n");
            }
            break;
        case HP_STRENGTH: /* as I (v0.6.2) */
            if (b >= HSTRENGTH_MIN && b <= HSTRENGTH_MAX) {
                uint8_t was = U.set.strength;
                U.set.strength = (uint8_t)b;
                if (!apply_strength()) {
                    U.set.strength = was; /* nothing changed on the chip: keep the old level */
                    apply_strength();
                    printf("vibration strength: no power chip to set it with\n");
                }
            } else {
                printf("haptic strength 1-%d (%d = full, as the board starts; 9 = full at 3.4 V)\n", HSTRENGTH_MAX,
                       HSTRENGTH_DEFAULT);
            }
            break;
        }
        apply_haptic_params();
        save_settings();
        break;
    case CON_CALIBRATE:
        stop_playback();
        calib_enter(!cal_valid(&U.cal));
        break;
    case CON_FACTORY:
        ble_forget_phones(); /* the phone has to pair again (O) */
        store_factory_reset();
        printf("settings, contacts, calibration and paired phone erased (device ID kept); restarting\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        break;
    case CON_RAW:
        U.playing = false;
        if (U.st == ST_PLAYBACK) {
            U.st = resume_state();
        }
        haptics_raw((uint8_t)a, (uint8_t)b);
        break;
    case CON_POWER_OFF:
        if (!switch_off()) {
            printf("not switched off: your SOS is active (cancel it first with a 5 s hold)\n");
        }
        break;
    case CON_PHONE_STATUS:
        print_phone();
        break;
    case CON_PHONE_PAIR: /* as O: OK or error plays when the window ends */
        store_save_ble_off(false);
        if (ble_open_window() != ESP_OK) {
            printf("BLE didn't start (the log says why)\n");
        }
        break;
    case CON_PHONE_FORGET:
        ble_forget_phones();
        printf("forgotten; on the phone, forget \"MorseLink%s%s\" too, then O pairs it again\n", U.own_id[0] ? "-" : "",
               U.own_id);
        break;
    case CON_TEST_START: /* a = 1: the phone asked (then loopback must be off) */
        if (U.sos_active) {
            net_test_refuse("sos"); /* cancel the SOS first (5 s hold) */
        } else {
            net_test_start(e->con.text, a != 0);
        }
        break;
    case CON_TEST_STOP:
        net_test_stop(a ? "app" : "console");
        break;
    case CON_TX_POWER: {
        if (U.test) {
            printf("not during a test (a point test sets the power itself and puts yours back afterwards)\n");
            break;
        }
        int max = radio_tx_dbm_max();
        int dbm = a < RADIO_TX_DBM_MIN ? RADIO_TX_DBM_MIN : a > max ? max : (int)a;
        U.set.tx_dbm = (int8_t)dbm;
        save_settings();
        net_set_tx_power(dbm);
        printf("transmit power %d dBm (this chip: %d-%d)%s\n", dbm, RADIO_TX_DBM_MIN, max,
               a > max ? ": that's this chip's most" : "");
        break;
    }
    case CON_BENCH: /* b = 1 with a = 1 / 0: the phone's BENCH POWER / STOP */
        if (a == 1) {
            bench_power_start(now_ms(), b != 0);
        } else if (a == 2) {
            bench_motor_go((int)b);
        } else if (U.bench) {
            bench_power_stop(b ? "app" : "stopped");
        } else if (!b) {
            stop_playback(); /* `bench stop` also ends bench motor's pattern */
        }
        break;
    case CON_PHONE_ONOFF: /* `phone on` / `phone off`: as W */
        switch (ble_switch(a ? 1 : 0)) {
        case 1:
            printf("Bluetooth on (stored): the paired phone reconnects by itself\n");
            break;
        case 0:
            printf("Bluetooth off (stored, restarts included): W or `phone on` turns it on again\n");
            break;
        default:
            printf("no phone is paired: O (or `phone pair`) pairs one, and Bluetooth then stays on\n");
            break;
        }
        break;
    case CON_SF: /* `radio sf 9-12`: as G 1-4 */
        if (U.test) {
            printf("not during a test (a point test sets the SF itself and puts yours back afterwards)\n");
            break;
        }
        if (a < RADIO_SF_MIN || a > RADIO_SF_MAX) {
            printf("radio sf %d-%d (G 1-4 on the device)\n", RADIO_SF_MIN, RADIO_SF_MAX);
            break;
        }
        U.set.sf = (uint8_t)a;
        save_settings();
        net_set_sf((int)a);
        printf("SF%ld (range level %ld). Set the same on the other device: on different settings the two can't hear "
               "each other at all, SOS included\n",
               (long)a, (long)(a - RADIO_SF_MIN + 1));
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Timers                                                                     */
/* ------------------------------------------------------------------------- */

/* battery_poll() -- every 30 s (not while playing: motors sag the voltage):
 * smooth the reading, warn once at 15 % and once at 5 % (spec §8). */
static void battery_poll(int64_t now)
{
    if (U.playing) {
        U.bat_next = now + 5000; /* motors sag the voltage; read when quiet */
        return;
    }
    U.bat_next = now + BATTERY_POLL_MS;
    int pct = battery_now();
    if (pct < 0) {
        return;
    }
    U.bat_pct = (U.bat_pct < 0) ? pct : (U.bat_pct * 3 + pct) / 4;
    if (U.bat_charging || U.bat_pct > BATTERY_WARN1_PCT + 5) {
        U.bat_warned1 = U.bat_warned2 = false;
        return;
    }
    if (U.bat_pct <= BATTERY_WARN2_PCT && !U.bat_warned2) {
        U.bat_warned1 = U.bat_warned2 = true;
    } else if (U.bat_pct <= BATTERY_WARN1_PCT && !U.bat_warned1) {
        U.bat_warned1 = true;
    } else {
        return;
    }
    alert_t a = {.kind = AL_BATTERY, .num = U.bat_pct};
    alert_push(&a, true);
    ESP_LOGW(TAG, "battery low: %d%% (%d mV)", U.bat_pct, U.bat_mv);
}

/*
 * run_timers() -- everything time-driven, after each event or every 250 ms:
 * decoder gap timeouts, the playback safety net, pending SOS alarms, SOS
 * confirmation, draft and command-mode timeouts, alerts, SOS resends,
 * battery and saving the calibration.
 */
static void run_timers(int64_t now)
{
    if (U.test) {
        test_timers(now); /* nothing else happens during a test */
        return;
    }
    if (U.bench) {
        bench_timers(now); /* ... nor during bench power */
        return;
    }

    /* Decoder timeouts (letter / word gaps). */
    if (U.st == ST_IDLE || U.st == ST_COMPOSE || U.st == ST_COMMAND || U.st == ST_SOS_ENTRY || disc_active()) {
        if (dec_deadline(&U.dec) <= now) {
            dec_poll(&U.dec, now);
            pump_decoder();
        }
    }

    /* Safety net for a lost "done" event. */
    if (U.playing && !haptics_busy() && now - U.play_started > 100) {
        on_play_done();
    }

    if (U.key_down) {
        return; /* nothing below happens while the key is held */
    }

    /* An SOS that arrived while another was being sent or alarmed. */
    if (U.alarm_pending != ADDR_NONE && !U.playing && U.st != ST_SOS_ENTRY && U.st != ST_SOS_ARMED &&
        U.st != ST_SOS_ALARM) {
        uint32_t src = U.alarm_pending;
        U.alarm_pending = ADDR_NONE;
        sos_start_alarm(src);
        return;
    }

    if (U.draft_len == 0) {
        U.draft_kept = false; /* K keeps one draft: once it is sent, cancelled or deleted, the next has the limit */
    }
    switch (U.st) {
    case ST_SOS_ENTRY: {
        int64_t wait = U.sos_arg_len ? SOS_CONFIRM_ARG_MS : SOS_CONFIRM_NOARG_MS;
        if (now - U.sos_ref >= wait && !dec_group_open(&U.dec)) {
            sos_arm();
        }
        break;
    }
    case ST_COMPOSE: {
        int64_t ref = U.last_key > U.last_play_end ? U.last_key : U.last_play_end;
        if (!U.draft_kept && !U.playing && now - ref >= DRAFT_ABANDON_MS) {
            ESP_LOGI(TAG, "draft \"%s\" discarded after %d s untouched", U.draft, DRAFT_ABANDON_MS / 1000);
            U.draft_len = 0;
            U.draft[0] = '\0';
            dec_reset(&U.dec);
            U.st = ST_IDLE;
            play_tone(TONE_CANCEL);
        }
        break;
    }
    case ST_COMMAND:
        if (!U.playing && now - U.last_key >= CMD_IDLE_EXIT_MS) {
            ESP_LOGI(TAG, "left command mode (nothing keyed)");
            phone_cm_end("timeout", U.cmd);
            cmd_clear();
            dec_reset(&U.dec);
            U.st = resume_state();
            play_tone(TONE_CANCEL);
        }
        break;
    case ST_CALIB:
        if (U.cal_capturing && U.cal_n > 0 && now - U.cal_last_up >= CALIB_END_SILENCE_MS) {
            calib_evaluate();
        }
        break;
    case ST_SOS_ALARM:
        if (!U.playing && now >= U.alarm_next) {
            play_alarm();
        }
        break;
    case ST_DISC_SCAN:
        /* The network always reports a sweep (in about 4 s at SF9, 22 s at
         * SF12); this only catches a lost report (e.g. the engine restarted
         * for a new ID). */
        if (now - U.disc_started >= disc_timeout_ms()) {
            ESP_LOGW(TAG, "discovery: no answer from the network");
            U.disc_n = 0;
            dec_reset(&U.dec);
            play_result_tone(TONE_ERROR);
        }
        break;
    case ST_DISC_WALK: {
        int64_t ref = U.last_key > U.last_play_end ? U.last_key : U.last_play_end;
        if (!U.playing && !dec_group_open(&U.dec) && now - ref >= DISC_WALK_IDLE_MS) {
            disc_end(true, "nothing keyed for 30 s");
        }
        break;
    }
    default:
        break;
    }

    /* Queued alerts play once the key has rested. */
    if (U.al_n && !U.playing && (U.st == ST_IDLE || U.st == ST_COMPOSE) && !dec_group_open(&U.dec)) {
        int64_t need = (U.st == ST_COMPOSE) ? ALERT_IDLE_COMPOSE_MS : ALERT_IDLE_MS;
        if (now - U.last_key >= need && now - U.last_play_end >= 800) {
            alert_t a = U.alerts[U.al_head];
            U.al_head = (U.al_head + 1) % ALERT_QUEUE_LEN;
            U.al_n--;
            play_alert(&a);
        }
    }

    /* SOS resends: every 30 s for 10 min, then every 2 min, until cancelled. */
    if (U.sos_active && now >= U.sos_next) {
        net_send_sos(U.sos_target);
        U.sos_next = now + ((now - U.sos_started < SOS_FAST_PHASE_MS) ? SOS_RESEND_FAST_MS : SOS_RESEND_SLOW_MS);
    }

    if (now >= U.bat_next) {
        battery_poll(now);
    }

    /* Persist the calibration while the key rests (flash writes stall both cores). */
    if (U.cal_dirty && cal_valid(&U.cal) && now - U.last_key >= CALIB_SAVE_IDLE_MS &&
        now - U.cal_last_save >= CALIB_SAVE_MIN_INTERVAL_MS && !U.playing) {
        store_save_calib(cal_profile(&U.cal));
        U.cal_dirty = false;
        U.cal_last_save = now;
    }
}

/* ------------------------------------------------------------------------- */
/* Task                                                                       */
/* ------------------------------------------------------------------------- */

/* handle() -- dispatch one event from the queue. */
static void handle(const ui_evt_t *e)
{
    switch (e->kind) {
    case UIE_KEY:
        if (U.test) {
            test_key(&e->key); /* only the 5 s hold counts */
            break;
        }
        if (U.bench) {
            bench_key(&e->key); /* ... here too */
            break;
        }
        switch (e->key.kind) {
        case KEY_DOWN:
            on_key_down(e->key.t_us / 1000);
            break;
        case KEY_UP:
            on_key_up(e->key.t_us / 1000, e->key.down_us / 1000);
            break;
        case KEY_HOLD:
            on_key_hold(e->key.level);
            break;
        }
        break;
    case UIE_PLAY_DONE:
        if (U.playing && e->tag == U.play_tag) {
            phone_play_end(); /* (before on_play_done(), which may start the next playback) */
            if (U.test) {
                U.playing = false;
                if (tt_plays(U.test_kind)) {
                    test_motor_play(); /* rundown-m: straight into the next round */
                }
            } else {
                on_play_done();
            }
        }
        break;
    case UIE_NET:
        on_net_event(&e->net);
        break;
    case UIE_CONSOLE:
        on_console(e);
        break;
    case UIE_PHONE:
        on_phone(e->phone);
        break;
    case UIE_TEST:
        on_test(e->test.on != 0, e->test.test);
        break;
    }
}

/*
 * ui_task() -- load everything, tell the network who we are, who our link
 * and contacts are, start BLE if a phone is paired, then loop: wait for an
 * event (or the decoder's next deadline, or 250 ms), handle it, run the
 * timers, bring the phone up to date.
 */
static void ui_task(void *arg)
{
    (void)arg;
    char id[ID_MAX_LEN + 1];

    store_load_settings(&U.set);
    store_load_peers(&U.peers);
    store_load_id(U.own_id);
    cal_reset(&U.cal);
    cal_profile_t prof;
    if (store_load_calib(&prof)) {
        cal_load(&U.cal, &prof);
    }
    dec_init(&U.dec, &U.cal);
    apply_haptic_params();
    apply_strength(); /* the stored vibration strength (I), once one was chosen */

    /* The network side: identity first (it restarts the protocol engine),
     * then power, blocklist, silent mode, loopback, the stored link and the
     * contacts. */
    net_set_identity(own_addr());
    net_set_tx_power(U.set.tx_dbm);
    net_set_sf(U.set.sf); /* the stored range level (G) */
    push_blocklist();
    net_set_discoverable(U.set.discoverable);
    /* Loopback is a bench tool and never survives a restart or a switch-off
     * (13 Oct): left on, the pretend partner takes the real partner's ID and
     * answers for it, so the real partner going away is never noticed.
     * Versions up to v0.3.4 saved it; clear such a saved "on" once. */
    if (U.set.loopback) {
        U.set.loopback = 0;
        save_settings();
        ESP_LOGW(TAG, "loopback was left on: switched off at start-up (`loopback on` in the console to use it again)");
    }
    net_set_loopback(false);
    U.link_addr = ADDR_NONE;
    U.link_state = LS_NONE;
    if (proto_addr_valid(U.set.link_addr)) {
        U.link_addr = U.set.link_addr;
        U.link_state = U.set.link_ended ? LS_ENDED : LS_UP;
    }
    restore_link_at_boot();
    U.req_addr = ADDR_NONE;
    convo_init(&U.convo); /* nothing remembered across a restart: the first message is announced */
    push_contacts();      /* a general SOS also goes to each of them through the phone */

    /* The phone: BLE stays off until the first O; once a phone has paired,
     * it starts at every boot and the phone reconnects by itself. */
    phone_tx_init(&U.ph_tx); /* TX numbers count from 1 again */
    ble_set_name(U.own_id);
    if (ble_wanted()) {
        ble_start();
    } else if (store_load_ble_on()) {
        ESP_LOGW(TAG, "Bluetooth is off (W): W turns it on again");
    }

    U.alarm_src = U.alarm_pending = ADDR_NONE;
#if VERBOSE_LOG
    U.cal_log = true; /* board.h: log keyed timings and everything played (`verbose off` stops it) */
#endif
    U.bat_pct = -1;
    U.bat_next = now_ms() + 2000;
    U.cal_last_save = now_ms();

    ESP_LOGI(TAG, "MorseLink %s, recipient %s%s, %d contact(s)", U.own_id[0] ? U.own_id : "(no ID)",
             U.link_addr == ADDR_NONE ? "none" : id_of(U.link_addr, id), U.link_state == LS_ENDED ? " (ended)" : "",
             peers_count(&U.peers));
    if (!U.own_id[0]) {
        ESP_LOGW(TAG, "no device ID yet: set one in the console with  id K7Q");
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    if (cal_valid(&U.cal)) {
        log_calibration("calibration loaded:");
        U.st = ST_IDLE;
        play_tone(TONE_OK); /* ready */
    } else {
        ESP_LOGI(TAG, "first use: keying calibration needed");
        calib_enter(true);
    }

    for (;;) {
        int64_t now = now_ms();
        int64_t dl = now + 250;
        if (U.st == ST_IDLE || U.st == ST_COMPOSE || U.st == ST_COMMAND || U.st == ST_SOS_ENTRY || disc_active()) {
            int64_t d = dec_deadline(&U.dec);
            if (d < dl) {
                dl = d;
            }
        }
        if (U.bench && U.bench_until < dl) {
            dl = U.bench_until; /* bench power's LED signal is timed to the ms */
        }
        TickType_t wait = (dl <= now) ? 0 : pdMS_TO_TICKS(dl - now);
        ui_evt_t e;
        if (xQueueReceive(U.q, &e, wait) == pdTRUE) {
            handle(&e);
        }
        run_timers(now_ms());
        phone_sync(false); /* the phone sees each change (draft, link, battery) within 250 ms */
    }
}

/* ui_start() -- create the UI's event queue and task (core 1), and route
 * "playback finished" from the haptics player to it. */
void ui_start(void)
{
    U.q = xQueueCreate(24, sizeof(ui_evt_t));
    haptics_set_done_cb(ui_post_play_done);
    xTaskCreatePinnedToCore(ui_task, "ui", 6144, NULL, 8, NULL, 1);
}

/* ------------------------------------------------------------------------- */
/* Posting (any task)                                                         */
/* ------------------------------------------------------------------------- */

/* ui_post_key() -- from the keyer task: key down / up / hold threshold.
 * Waits up to 20 ms if the queue is full. */
void ui_post_key(const key_evt_t *k)
{
    ui_evt_t e = {.kind = UIE_KEY, .key = *k};
    xQueueSend(U.q, &e, pdMS_TO_TICKS(20));
}

/* ui_post_play_done() -- from the haptics player (esp_timer task): the job
 * with `tag` finished. Never blocks. */
void ui_post_play_done(uint32_t tag)
{
    ui_evt_t e = {.kind = UIE_PLAY_DONE, .tag = tag};
    xQueueSend(U.q, &e, 0);
}

/* ui_post_net() -- from the net task: something arrived or an exchange
 * finished (see link.h for the event kinds). */
void ui_post_net(const net_evt_t *ev)
{
    ui_evt_t e;
    memset(&e, 0, sizeof(e));
    e.kind = UIE_NET;
    e.net = *ev;
    if (xQueueSend(U.q, &e, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "UI queue full: network event %s lost", link_event_name(ev->kind));
    }
}

/* ui_post_console() -- from the console task: run a command in the UI
 * task. False if the queue stayed full for 100 ms. */
bool ui_post_console(con_op_t op, int32_t a, int32_t b, const char *text)
{
    ui_evt_t e;
    memset(&e, 0, sizeof(e));
    e.kind = UIE_CONSOLE;
    e.con.op = (uint8_t)op;
    e.con.a = a;
    e.con.b = b;
    if (text) {
        strncpy(e.con.text, text, TT_ARGS_MAX);
    }
    return xQueueSend(U.q, &e, pdMS_TO_TICKS(100)) == pdTRUE;
}

/* ui_post_phone() -- from the BLE side (NimBLE's host task, the ble_tx task,
 * or the console's pretend phone): see uip_t. Never blocks: NimBLE's task
 * must not wait on us. If the queue is full the report is lost (logged). */
void ui_post_phone(uint8_t what)
{
    ui_evt_t e;
    memset(&e, 0, sizeof(e));
    e.kind = UIE_PHONE;
    e.phone = what;
    if (!U.q || xQueueSend(U.q, &e, 0) != pdTRUE) {
        ESP_LOGE(TAG, "UI queue full: phone report %d lost", what);
    }
}

/* ui_post_test_cmd() -- from the BLE side: the phone sent TEST START (with
 * its parameters) or TEST STOP. Never blocks (NimBLE's task must not wait). */
void ui_post_test_cmd(bool start, const char *args)
{
    ui_evt_t e;
    memset(&e, 0, sizeof(e));
    e.kind = UIE_CONSOLE;
    e.con.op = (uint8_t)(start ? CON_TEST_START : CON_TEST_STOP);
    e.con.a = 1; /* from the phone */
    if (args) {
        strncpy(e.con.text, args, TT_ARGS_MAX);
    }
    if (!U.q || xQueueSend(U.q, &e, 0) != pdTRUE) {
        ESP_LOGE(TAG, "UI queue full: the phone's TEST %s lost", start ? "START" : "STOP");
    }
}

/* ui_post_bench_cmd() -- from the BLE side: the phone sent BENCH POWER
 * (start) or BENCH STOP. Never blocks (NimBLE's task must not wait). */
void ui_post_bench_cmd(bool start)
{
    ui_evt_t e;
    memset(&e, 0, sizeof(e));
    e.kind = UIE_CONSOLE;
    e.con.op = (uint8_t)CON_BENCH;
    e.con.a = start ? 1 : 0;
    e.con.b = 1; /* from the phone */
    if (!U.q || xQueueSend(U.q, &e, 0) != pdTRUE) {
        ESP_LOGE(TAG, "UI queue full: the phone's BENCH %s lost", start ? "POWER" : "STOP");
    }
}

/* ui_post_test() -- from the net task: test mode is on / off (ntest.c). */
void ui_post_test(bool on, uint8_t test)
{
    ui_evt_t e;
    memset(&e, 0, sizeof(e));
    e.kind = UIE_TEST;
    e.test.on = on ? 1 : 0;
    e.test.test = test;
    if (xQueueSend(U.q, &e, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "UI queue full: test mode %s lost", on ? "on" : "off");
    }
}
