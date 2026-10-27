/*
 * MorseLink — the phone protocol (spec §12): what the device and the owner's
 * phone say to each other over BLE.
 *
 * The BLE link is a Nordic UART Service (ble.c): a stream of bytes each way.
 * On it, every message is one line of plain ASCII text ending in '\n', with
 * fields separated by single spaces; a text field (a message, the draft) is
 * always last and may contain spaces. Plain text keeps it readable in a
 * generic BLE terminal app while testing, and lines don't care how the
 * phone's MTU chops the stream up.
 *
 * Device -> phone:
 *   HI <own ID> <protocol> <firmware>   hello, when the phone is accepted (or says HI)
 *   ST <partner|-> <link> <battery%|?> <silent 0/1> <lora> <relay>
 *                                       status: after HI and whenever it changes;
 *                                       link = none / connected / lost / ended (any path);
 *                                       lora, relay = each path (protocol 2, v0.6.0):
 *                                       none / checking / connected / they-cant-hear /
 *                                       cant-hear-them / lost / off (link.h path_state_t)
 *   RD <sf> <bandwidth kHz> <dBm>       the radio setting: after HI and whenever it changes
 *                                       (protocol 2)
 *   DR <draft>                          the draft, whole, whenever it changes ("DR" = empty):
 *                                       the live compose stream
 *   CM <command>                        command mode: what has been keyed so far (letter +
 *                                       argument; "CM" = nothing yet), on entering it and on
 *                                       every change (protocol 2). The draft is kept meanwhile.
 *   CE <how> [<command>]                command mode ended or a command was given up: how =
 *                                       done / cancelled / timeout / sos (it ended), aborted /
 *                                       error (still in command mode: a CM line follows)
 *   TX <n> <to> <text>                  message n leaves for <to> (n counts from 1)
 *   OK <n>                              ... its receipt came back
 *   NO <n>                              ... it wasn't delivered (its text is back in the draft,
 *                                       unless its recipient was just forgotten with X)
 *   RX <from> <text>                    a message arrived
 *   EV <what> <ID|ALL|-> [<detail>]     something happened (PHONE_EV_* below); LOST has the
 *                                       reason (silence / no-receipt / one-way), LORA and
 *                                       RELAY the path's new state (protocol 2)
 *   RL <ID> <base64>                    relay: publish this LoRa packet to morselink/<ID>
 *   T,<n>,<ms>,<EVENT>,<key>=<value>,...
 *                                       test mode (ntest.c): one event of a test session;
 *                                       the events are listed in docs/TEST_PLAN.md §4.4
 *   BN <what> [<detail>]                the meter session, bench power (protocol 3, v0.6.1):
 *                                       BN refused <why>      not started (test / sos / running)
 *                                       BN wait-usb           accepted; waits for USB to be unplugged
 *                                       BN start <s> <dBm>    the timetable starts now: step 1 in
 *                                                             BENCH_SETTLE_S; <s> = its total from step 1's
 *                                                             signal to switching off, <dBm> = the chip's
 *                                                             maximum (no 22 dBm step below 22)
 *                                       BN step <id> <s>      step <id> (1, 1b ... 6, bench.h) begins, for <s>
 *                                                             (only while the phone is connected: the app runs
 *                                                             the same timetable by itself meanwhile)
 *                                       BN end <how>          over: off (step 6 switches off), stopped (bench
 *                                                             stop), hold (5 s hold), app (BENCH STOP)
 *   The learning view (protocol 4, v0.6.2): what the key does as it happens, the keying
 *   and reading timings, and what plays on the motors, so the app can show Morse as it is
 *   keyed and felt.
 *   KY <where> <pattern> <ms> <boundary>
 *                                       a press was released and counted as an element:
 *                                       where = key (the decoder: composing, command mode,
 *                                       SOS entry, the discovery list) or cal (the PARIS
 *                                       attempt); pattern = the letter keyed so far with this
 *                                       press last (cal: every press of the attempt so far);
 *                                       ms = this press, boundary = the dot / dash boundary it
 *                                       was judged by (ms; longer = dash)
 *   KY                                  nothing is open any more (the letter closed or was
 *                                       dropped, the PARIS attempt ended)
 *   KC <kind> <pattern> [<char>]        the decoder closed a letter: kind = char (char = the
 *                                       letter), unknown (char = ?), error (the error prosign:
 *                                       it deleted the letter before; no char). Before the DR
 *                                       line that shows it
 *   CB ok | CB retry <why>              a PARIS attempt was judged: why = count / range /
 *                                       elements / gaps (calib.h's cal_err_t)
 *   SP <valid> <calibrating> <dot> <dash> <gap_in> <gap_letter> <boundary> <letter> <word>
 *      <health> <q> <unit> <fx10> <strength> <strength_mV> <dashes>
 *                                       the speeds: after HI and when they change (at most
 *                                       about once a second). valid 0/1 = calibrated;
 *                                       calibrating 0/1 = PARIS is being asked for now; the
 *                                       learned averages (ms): dot, dash, gap inside a letter,
 *                                       gap between letters; the decoder's thresholds (ms):
 *                                       dot / dash boundary, silence that ends a letter,
 *                                       silence that adds a word space; health = none /
 *                                       healthy / marginal / collapsed; reading: Q digit, unit
 *                                       ms, Farnsworth x10; vibration strength: I level 1-5
 *                                       and the motor supply in mV (0 = no power chip);
 *                                       dashes = H 1 (sweep M1 -> M2) or 2 (M2 only), v0.6.3:
 *                                       a 16th field, so an app that reads 15 still works
 *   PL <i> <offset> <unit> <fx10> <text>
 *                                       a playback with Morse in it starts now: one line per
 *                                       Morse part (i = 0, 1, ...; i = 0 means a new playback),
 *                                       offset = ms from now to that part's first element,
 *                                       unit / fx10 = its timing (dot 1U, dash 3U, 1U inside a
 *                                       letter, 3U x fx10/10 between letters, 7U x fx10/10
 *                                       between words); the text is cut to fit the line
 *   PL end                              that playback finished or was stopped
 * Protocol 2 only adds fields at the ends of lines and new tags: an app made
 * for protocol 1 shows everything it showed before. Protocol 3 (v0.6.1) adds
 * the BN lines, EV BLE_OFF, the point test (TEST START test=point plan=...)
 * and BENCH POWER / STOP; nothing else changes. Protocol 4 (v0.6.2) adds the
 * learning view's lines (KY, KC, CB, SP, PL) and nothing else.
 *
 * Phone -> device:
 *   HI [anything]                       hello: the device answers HI, ST and DR
 *   RL <base64>                         relay: a packet that arrived on morselink/<own ID>
 *   TEST START test=<t> paths=<p> role=<r> session=<s> seed=<n>
 *                                       start a test session (ttest.h has the values);
 *                                       answered by T lines: START, or REFUSED with a reason
 *   TEST STOP                           end it (a STOP line follows)
 *   BENCH POWER                         start the meter session (protocol 3): answered by BN lines
 *   BENCH STOP                          end it early (BN end app)
 * Lines the device doesn't know are ignored, so a newer app can send more.
 *
 * The relay (spec §12): the phone forwards LoRa packets through MQTT, one
 * topic per device, without understanding them. While a phone is connected,
 * every packet this device originates for a particular device also goes to
 * that device's topic; general SOS packets go to every contact's. The
 * receiving device drops whichever copy arrives second (the duplicate ring,
 * proto.h), so a message gets through if either path works. Discovery and
 * ping packets never go through the phone: they measure the radio. While a
 * test runs (ntest.c) this mirroring stops: each test message then goes as
 * two separate copies, one per path, each with its own receipt.
 *
 * Pure C (no ESP-IDF): the host tests compile it too.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "proto.h"

#define PHONE_PROTOCOL 4      /* the <protocol> field of HI (2: v0.6.0, paths, RD, CM / CE; 3: v0.6.1, BN,
                                 BENCH, EV BLE_OFF, the point test; 4: v0.6.2, KY / KC / CB / SP / PL) */
#define PHONE_LINE_MAX 240    /* longest line either way, '\n' excluded (an RL line is at most ~170) */
#define PHONE_RELAY_MAX 10    /* topics one packet can go to: a general SOS to every contact + the partner */

/* What EV lines report. */
typedef enum {
    PHONE_EV_LINKED = 0,  /* connected to ID (our request accepted, or our accept confirmed) */
    PHONE_EV_BUSY,        /* ID is busy: our request waits in their queue */
    PHONE_EV_DECLINED,    /* ID said no */
    PHONE_EV_UNAVAILABLE, /* ID didn't answer */
    PHONE_EV_FAILED,      /* our accept of ID wasn't confirmed */
    PHONE_EV_REQUEST,     /* ID asks to connect (answer with A / N on the device) */
    PHONE_EV_LOST,        /* the link to ID is lost */
    PHONE_EV_RESTORED,    /* ... and back */
    PHONE_EV_ENDED,       /* ID linked with someone else */
    PHONE_EV_UNPAIRED,    /* ID forgot this device */
    PHONE_EV_SOS,         /* SOS from ID */
    PHONE_EV_SOS_ACK,     /* ID acknowledged our SOS */
    PHONE_EV_SOS_CANCEL,  /* ID cancelled their SOS */
    PHONE_EV_SOS_SENT,    /* our SOS went out (ID = target, or ALL) */
    PHONE_EV_SOS_STOPPED, /* our SOS was cancelled */
    PHONE_EV_LORA,        /* the LoRa path to ID changed: detail = its state (protocol 2) */
    PHONE_EV_RELAY,       /* the relay path to ID changed: detail = its state (protocol 2) */
    PHONE_EV_BLE_OFF,     /* W: this device (ID = our own) switches Bluetooth off now (protocol 3) */
    PHONE_EV__COUNT
} phone_ev_t;

/* The word an EV line uses for `ev` ("LINKED", "SOS", ...). */
const char *phone_ev_name(uint8_t ev);

/* ------------------------------------------------------------------------- */
/* Base64 (standard alphabet, '=' padding): how packets travel inside lines  */
/* ------------------------------------------------------------------------- */

/* Characters needed to encode n bytes (without the terminating NUL). */
#define PHONE_B64_LEN(n) ((((n) + 2) / 3) * 4)

/* Encode `len` bytes into `out` (NUL-terminated). Returns the length
 * written, or -1 if `out_size` is too small. */
int phone_b64_encode(const uint8_t *in, int len, char *out, int out_size);

/* Decode `s` (exactly `n` characters, a multiple of 4, padding allowed only
 * at the end) into `out`. Returns the number of bytes, or -1 if `s` isn't
 * valid base64 or doesn't fit in `out_size`. */
int phone_b64_decode(const char *s, int n, uint8_t *out, int out_size);

/* ------------------------------------------------------------------------- */
/* Building device -> phone lines (each NUL-terminated, without '\n')        */
/* ------------------------------------------------------------------------- */
/* Every function writes one line into out[size] and returns its length, or
 * -1 if it doesn't fit (out is then an empty string). IDs come as
 * addresses: ADDR_NONE prints "-", ADDR_BROADCAST "ALL". */

int phone_fmt_hi(char *out, int size, uint32_t own, const char *firmware);
int phone_fmt_status(char *out, int size, uint32_t partner, const char *link_state, int battery_pct, bool silent,
                     const char *lora_state, const char *relay_state);
int phone_fmt_radio(char *out, int size, int sf, int bw_khz, int tx_dbm); /* RD */
int phone_fmt_draft(char *out, int size, const char *draft);
int phone_fmt_cmd(char *out, int size, const char *command);                  /* CM */
int phone_fmt_cmd_end(char *out, int size, const char *how, const char *command); /* CE */
int phone_fmt_tx(char *out, int size, unsigned n, uint32_t to, const char *text);
int phone_fmt_result(char *out, int size, unsigned n, bool delivered); /* OK n / NO n */
int phone_fmt_rx(char *out, int size, uint32_t from, const char *text);
int phone_fmt_event(char *out, int size, uint8_t ev, uint32_t who);
/* EV with a third field (detail NULL or "": the same as phone_fmt_event). */
int phone_fmt_event_detail(char *out, int size, uint8_t ev, uint32_t who, const char *detail);
int phone_fmt_relay(char *out, int size, uint32_t topic, const uint8_t *pkt, int len);
/* BN <what> [<detail>]: the meter session (detail NULL or "": none). */
int phone_fmt_bench(char *out, int size, const char *what, const char *detail);

/* The learning view (protocol 4). */
/* KY <where> <pattern> <ms> <boundary>; pattern NULL or "": "KY" (nothing open). */
int phone_fmt_key(char *out, int size, const char *where, const char *pattern, int ms, int boundary_ms);
/* KC <kind> <pattern> [<char>]; c outside printable ASCII (e.g. 0 for error): no char field. */
int phone_fmt_key_char(char *out, int size, const char *kind, const char *pattern, char c);
/* CB ok / CB retry <why> (why NULL or "": "other"). */
int phone_fmt_cal_result(char *out, int size, bool ok, const char *why);

/* What an SP line says (all times in ms, rounded). */
typedef struct {
    bool valid;         /* calibrated (else the timings are the starting guesses) */
    bool calibrating;   /* PARIS is being asked for now */
    int dot_ms, dash_ms;          /* average dot and dash, as keyed */
    int gap_in_ms, gap_letter_ms; /* average gap inside a letter, between letters */
    int boundary_ms;    /* presses longer than this are dashes */
    int letter_ms;      /* this much silence ends a letter */
    int word_ms;        /* ... and this much adds a word space */
    const char *health; /* none / healthy / marginal / collapsed */
    int q, unit_ms, fx10;         /* reading speed: Q digit, unit, Farnsworth x10 */
    int strength, strength_mv;    /* vibration strength: I level, motor supply (0: unknown) */
    int dash_style;               /* how dashes feel: 1 = sweep M1 -> M2, 2 = M2 only (H; v0.6.3) */
} phone_speeds_t;
int phone_fmt_speeds(char *out, int size, const phone_speeds_t *s);

/* PL <i> <offset_ms> <unit_ms> <fx10> <text>: the text is cut to fit; -1 if
 * nothing of it fits (or it is empty). */
int phone_fmt_play(char *out, int size, int i, uint32_t offset_ms, int unit_ms, int fx10, const char *text);
/* PL end */
int phone_fmt_play_end(char *out, int size);

/* ------------------------------------------------------------------------- */
/* Reading phone -> device lines                                              */
/* ------------------------------------------------------------------------- */

typedef enum {
    PHONE_IN_UNKNOWN = 0, /* anything else: ignored */
    PHONE_IN_HI,          /* HI [...] */
    PHONE_IN_RELAY,       /* RL <base64>: pkt/len hold the decoded packet */
    PHONE_IN_BAD,         /* a known line that is malformed (e.g. bad base64): ignored, but logged */
    PHONE_IN_TEST_START,  /* TEST START <parameters>: args holds the parameters */
    PHONE_IN_TEST_STOP,   /* TEST STOP */
    PHONE_IN_BENCH_POWER, /* BENCH POWER: start the meter session (protocol 3) */
    PHONE_IN_BENCH_STOP,  /* BENCH STOP: end it */
} phone_in_kind_t;

#define PHONE_ARGS_MAX 160 /* longest TEST START parameter text (= TT_ARGS_MAX: a point test's plan) */

typedef struct {
    uint8_t kind; /* phone_in_kind_t */
    uint8_t pkt[PROTO_MAX_PKT];
    int len;
    char args[PHONE_ARGS_MAX + 1]; /* PHONE_IN_TEST_START */
} phone_in_t;

/* Parse one line (no '\n'; a trailing '\r' is ignored). No side effects. */
void phone_parse(const char *line, phone_in_t *out);

/* ------------------------------------------------------------------------- */
/* Line assembly: bytes in, whole lines out                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    char buf[PHONE_LINE_MAX + 1];
    int n;          /* characters collected for the current line */
    bool overflow;  /* the current line was too long: skip it up to its '\n' */
} phone_rx_t;

void phone_rx_init(phone_rx_t *r);

/* Feed the next byte. Returns true when it completed a line, which is then
 * in r->buf (NUL-terminated, without '\n' or '\r'). An over-long line is
 * dropped whole; empty lines are skipped. */
bool phone_rx_byte(phone_rx_t *r, uint8_t c);

/* ------------------------------------------------------------------------- */
/* The relay                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Which topics a packet this device is transmitting should also be published
 * to through the phone. In: the encoded packet; own = our address; contacts
 * (n_contacts of them) and partner (ADDR_NONE: none) for general SOS.
 * Out: up to `max` addresses in to[]. Returns how many (0: not through the
 * phone). Rules: only packets we originated (a relay of someone else's SOS
 * stays on the radio); never DISCOVER, PRESENCE or PING; a packet for one
 * device goes to that device's topic; a general SOS or SOS_CANCEL to every
 * contact's and the partner's (each once); other broadcasts nowhere.
 */
int phone_relay_targets(const uint8_t *pkt, int len, uint32_t own, const uint32_t *contacts, int n_contacts,
                        uint32_t partner, uint32_t *to, int max);

/* ------------------------------------------------------------------------- */
/* Messages announced with TX, until their OK / NO                            */
/* ------------------------------------------------------------------------- */

/* Room for every message the link engine can hold (MSG_QUEUE_MAX + 1), plus
 * a few the UI may have handed on that the net task hasn't taken in yet. */
#define PHONE_TX_MAX (MSG_QUEUE_MAX + 4)

/* What the network reported about messages sent (link.h, LEV_MSG_*). */
typedef enum {
    PHONE_TX_DELIVERED,  /* the oldest waiting for `to` got its receipt (receipts come in order) */
    PHONE_TX_ALL_FAILED, /* no receipt for the one being delivered: the engine gave up on every one waiting */
    PHONE_TX_REFUSED,    /* the newest for `to` was refused at once (link down, queue full) */
    PHONE_TX_DROPPED,    /* every one for `to` was dropped without a word: we, or they, forgot the other (X) */
} phone_tx_res_t;

typedef struct {
    unsigned last;          /* the last TX number handed out (they count from 1) */
    struct {
        unsigned n;         /* its TX number */
        uint32_t to;        /* who it is for */
    } pend[PHONE_TX_MAX];   /* waiting for OK / NO, oldest first */
    int n_pend;
} phone_tx_t;

void phone_tx_init(phone_tx_t *t);

/* A message leaves for `to`: returns its TX number. If PHONE_TX_MAX are
 * already waiting (can't happen), the oldest is forgotten. */
unsigned phone_tx_add(phone_tx_t *t, uint32_t to);

/* Settle the messages `r` concerns (`to` is ignored for ALL_FAILED): their
 * TX numbers go to done[] (at most `max`), oldest first, and they stop
 * waiting. Returns how many. They were delivered (OK) for
 * PHONE_TX_DELIVERED, and not (NO) otherwise. */
int phone_tx_settle(phone_tx_t *t, phone_tx_res_t r, uint32_t to, unsigned *done, int max);
