/*
 * MorseLink — the phone protocol (see phone.h for the lines and the relay
 * rules).
 *
 * Pure C: no ESP-IDF, no allocation, no globals. Every function either
 * transforms its arguments or updates the struct it is given.
 */
#include "phone.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "morse.h"

/* ------------------------------------------------------------------------- */
/* Names                                                                      */
/* ------------------------------------------------------------------------- */

/*
 * phone_ev_name() -- the word an EV line uses for an event.
 * In: ev, a phone_ev_t. Returns a static string ("?" for anything else).
 */
const char *phone_ev_name(uint8_t ev)
{
    static const char *const names[PHONE_EV__COUNT] = {
        "LINKED", "BUSY",     "DECLINED", "UNAVAILABLE", "FAILED",  "REQUEST",    "LOST",       "RESTORED",
        "ENDED",  "UNPAIRED", "SOS",      "SOS_ACK",     "SOS_CANCEL", "SOS_SENT", "SOS_STOPPED", "LORA",
        "RELAY",  "BLE_OFF",
    };
    return ev < PHONE_EV__COUNT ? names[ev] : "?";
}

/* id_word() -- an address as a line field: its ID, "ALL" or "-".
 * Writes into buf (ID_MAX_LEN + 1 bytes) and returns it. */
static const char *id_word(uint32_t addr, char buf[ID_MAX_LEN + 1])
{
    if (addr == ADDR_BROADCAST) {
        strcpy(buf, "ALL");
    } else if (!addr_to_id(addr, buf)) {
        strcpy(buf, "-"); /* ADDR_NONE, or anything that isn't an ID */
    }
    return buf;
}

/* ------------------------------------------------------------------------- */
/* Base64                                                                     */
/* ------------------------------------------------------------------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * phone_b64_encode() -- see phone.h. Every 3 bytes become 4 characters; the
 * last group is padded with '=' when the input isn't a multiple of 3.
 */
int phone_b64_encode(const uint8_t *in, int len, char *out, int out_size)
{
    int need = PHONE_B64_LEN(len);
    if (len < 0 || need + 1 > out_size) {
        return -1;
    }
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;      /* the group's 24 bits, missing bytes as 0 */
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < len) {
            v |= in[i + 2];
        }
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '='; /* 1 byte in the group: two '=' */
        out[o++] = i + 2 < len ? B64[v & 63] : '=';        /* 2 bytes: one '=' */
    }
    out[o] = '\0';
    return o;
}

/* b64_value() -- the 6-bit value of a base64 character, or -1. */
static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/*
 * phone_b64_decode() -- see phone.h. Strict: the length must be a multiple
 * of 4, '=' may only end the last group (at most two of them), and any
 * other character outside the alphabet rejects the whole string.
 */
int phone_b64_decode(const char *s, int n, uint8_t *out, int out_size)
{
    if (n < 0 || n % 4 != 0) {
        return -1;
    }
    int o = 0;
    for (int i = 0; i < n; i += 4) {
        bool last = i + 4 == n;
        int pad = 0;
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i + k];
            if (c == '=' && last && k >= 2) {
                pad++;
                v <<= 6;
                continue;
            }
            int d = b64_value(c);
            if (d < 0 || pad) { /* a stray character, or data after '=' */
                return -1;
            }
            v = (v << 6) | (uint32_t)d;
        }
        int bytes = 3 - pad;
        if (o + bytes > out_size) {
            return -1;
        }
        out[o++] = (uint8_t)(v >> 16);
        if (bytes > 1) {
            out[o++] = (uint8_t)(v >> 8);
        }
        if (bytes > 2) {
            out[o++] = (uint8_t)v;
        }
    }
    return o;
}

/* ------------------------------------------------------------------------- */
/* Building lines                                                             */
/* ------------------------------------------------------------------------- */

/* fmt() -- snprintf that reports "didn't fit" as -1 and then leaves an empty
 * string, so a cut-off line is never sent half. */
static int fmt(char *out, int size, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(out, (size_t)size, f, ap);
    va_end(ap);
    if (n < 0 || n >= size) {
        if (size > 0) {
            out[0] = '\0';
        }
        return -1;
    }
    return n;
}

/* phone_fmt_hi() -- "HI <own ID> <protocol> <firmware>". */
int phone_fmt_hi(char *out, int size, uint32_t own, const char *firmware)
{
    char id[ID_MAX_LEN + 1];
    return fmt(out, size, "HI %s %d %s", id_word(own, id), PHONE_PROTOCOL, firmware);
}

/* phone_fmt_status() -- "ST <partner|-> <link> <battery%|?> <silent 0/1>
 * <lora> <relay>"; battery_pct < 0 means no reading. */
int phone_fmt_status(char *out, int size, uint32_t partner, const char *link_state, int battery_pct, bool silent,
                     const char *lora_state, const char *relay_state)
{
    char id[ID_MAX_LEN + 1], bat[12] = "?"; /* room for any int */
    if (battery_pct >= 0) {
        snprintf(bat, sizeof bat, "%d", battery_pct);
    }
    return fmt(out, size, "ST %s %s %s %d %s %s", id_word(partner, id), link_state, bat, silent ? 1 : 0, lora_state,
               relay_state);
}

/* phone_fmt_radio() -- "RD <sf> <bandwidth kHz> <dBm>". */
int phone_fmt_radio(char *out, int size, int sf, int bw_khz, int tx_dbm)
{
    return fmt(out, size, "RD %d %d %d", sf, bw_khz, tx_dbm);
}

/* phone_fmt_cmd() -- "CM <command>", or just "CM" when nothing is keyed yet. */
int phone_fmt_cmd(char *out, int size, const char *command)
{
    return command[0] ? fmt(out, size, "CM %s", command) : fmt(out, size, "CM");
}

/* phone_fmt_cmd_end() -- "CE <how> <command>", or "CE <how>" without one. */
int phone_fmt_cmd_end(char *out, int size, const char *how, const char *command)
{
    return (command && command[0]) ? fmt(out, size, "CE %s %s", how, command) : fmt(out, size, "CE %s", how);
}

/* phone_fmt_draft() -- "DR <draft>", or just "DR" for an empty draft. */
int phone_fmt_draft(char *out, int size, const char *draft)
{
    return draft[0] ? fmt(out, size, "DR %s", draft) : fmt(out, size, "DR");
}

/* phone_fmt_tx() -- "TX <n> <to> <text>": message n leaves. */
int phone_fmt_tx(char *out, int size, unsigned n, uint32_t to, const char *text)
{
    char id[ID_MAX_LEN + 1];
    return fmt(out, size, "TX %u %s %s", n, id_word(to, id), text);
}

/* phone_fmt_result() -- "OK <n>" (delivered) or "NO <n>" (not). */
int phone_fmt_result(char *out, int size, unsigned n, bool delivered)
{
    return fmt(out, size, "%s %u", delivered ? "OK" : "NO", n);
}

/* phone_fmt_rx() -- "RX <from> <text>": a message arrived. */
int phone_fmt_rx(char *out, int size, uint32_t from, const char *text)
{
    char id[ID_MAX_LEN + 1];
    return fmt(out, size, "RX %s %s", id_word(from, id), text);
}

/* phone_fmt_event() -- "EV <what> <ID|ALL|->". */
int phone_fmt_event(char *out, int size, uint8_t ev, uint32_t who)
{
    return phone_fmt_event_detail(out, size, ev, who, NULL);
}

/* phone_fmt_event_detail() -- "EV <what> <ID|ALL|-> <detail>". */
int phone_fmt_event_detail(char *out, int size, uint8_t ev, uint32_t who, const char *detail)
{
    char id[ID_MAX_LEN + 1];
    if (detail && detail[0]) {
        return fmt(out, size, "EV %s %s %s", phone_ev_name(ev), id_word(who, id), detail);
    }
    return fmt(out, size, "EV %s %s", phone_ev_name(ev), id_word(who, id));
}

/* phone_fmt_relay() -- "RL <ID> <base64 of the packet>". */
int phone_fmt_relay(char *out, int size, uint32_t topic, const uint8_t *pkt, int len)
{
    char id[ID_MAX_LEN + 1];
    int n = fmt(out, size, "RL %s ", id_word(topic, id));
    if (n < 0 || phone_b64_encode(pkt, len, out + n, size - n) < 0) {
        out[0] = '\0';
        return -1;
    }
    return n + PHONE_B64_LEN(len);
}

/* phone_fmt_bench() -- see phone.h: "BN start 543 22", "BN wait-usb". */
int phone_fmt_bench(char *out, int size, const char *what, const char *detail)
{
    if (detail && detail[0]) {
        return fmt(out, size, "BN %s %s", what, detail);
    }
    return fmt(out, size, "BN %s", what);
}

/* ------------------------------------------------------------------------- */
/* The learning view (protocol 4, v0.6.2)                                     */
/* ------------------------------------------------------------------------- */

/* phone_fmt_key() -- see phone.h: "KY key .- 540 330"; with pattern NULL,
 * just "KY" (nothing open). An empty pattern is sent as "KY" too. */
int phone_fmt_key(char *out, int size, const char *where, const char *pattern, int ms, int boundary_ms)
{
    if (!pattern || !pattern[0]) {
        return fmt(out, size, "KY");
    }
    return fmt(out, size, "KY %s %s %d %d", where, pattern, ms, boundary_ms);
}

/* phone_fmt_key_char() -- see phone.h: "KC char .- A", "KC unknown .-.-- ?",
 * "KC error ........" (no character: it deleted one). */
int phone_fmt_key_char(char *out, int size, const char *kind, const char *pattern, char c)
{
    if (c > ' ' && c < 0x7F) {
        return fmt(out, size, "KC %s %s %c", kind, pattern, c);
    }
    return fmt(out, size, "KC %s %s", kind, pattern);
}

/* phone_fmt_cal_result() -- see phone.h: "CB ok" or "CB retry <why>". */
int phone_fmt_cal_result(char *out, int size, bool ok, const char *why)
{
    if (ok) {
        return fmt(out, size, "CB ok");
    }
    return fmt(out, size, "CB retry %s", (why && why[0]) ? why : "other");
}

/* phone_fmt_speeds() -- see phone.h: the SP line, every field a word. */
int phone_fmt_speeds(char *out, int size, const phone_speeds_t *s)
{
    return fmt(out, size, "SP %d %d %d %d %d %d %d %d %d %s %d %d %d %d %d %d", s->valid ? 1 : 0, s->calibrating ? 1 : 0,
               s->dot_ms, s->dash_ms, s->gap_in_ms, s->gap_letter_ms, s->boundary_ms, s->letter_ms, s->word_ms,
               (s->health && s->health[0]) ? s->health : "none", s->q, s->unit_ms, s->fx10, s->strength,
               s->strength_mv, s->dash_style);
}

/* phone_fmt_play() -- see phone.h: "PL 0 1250 200 10 DEA HELLO". The text
 * is the line's last field and is cut to fit; with nothing left to show the
 * line isn't made (-1). */
int phone_fmt_play(char *out, int size, int i, uint32_t offset_ms, int unit_ms, int fx10, const char *text)
{
    int n = fmt(out, size, "PL %d %lu %d %d ", i, (unsigned long)offset_ms, unit_ms, fx10);
    if (n < 0) {
        return -1;
    }
    int room = size - 1 - n; /* characters of text that still fit */
    int len = (int)strlen(text);
    if (len > room) {
        len = room;
    }
    while (len > 0 && text[len - 1] == ' ') {
        len--; /* no trailing spaces (the cut may leave one) */
    }
    if (len <= 0) {
        out[0] = '\0';
        return -1;
    }
    memcpy(out + n, text, (size_t)len);
    out[n + len] = '\0';
    return n + len;
}

/* phone_fmt_play_end() -- "PL end": the playback finished or was stopped. */
int phone_fmt_play_end(char *out, int size)
{
    return fmt(out, size, "PL end");
}

/* ------------------------------------------------------------------------- */
/* Reading lines                                                              */
/* ------------------------------------------------------------------------- */

/*
 * phone_parse() -- see phone.h. The line's first word picks the kind; for
 * RL the rest must be one base64 word that decodes to at most PROTO_MAX_PKT
 * bytes (whether those bytes are a valid packet is the link engine's
 * business, as for anything off the radio).
 */
void phone_parse(const char *line, phone_in_t *out)
{
    memset(out, 0, sizeof(*out));
    int n = (int)strlen(line);
    if (n && line[n - 1] == '\r') {
        n--; /* a CRLF terminal app */
    }
    if (n >= 2 && strncmp(line, "HI", 2) == 0 && (n == 2 || line[2] == ' ')) {
        out->kind = PHONE_IN_HI;
        return;
    }
    if (n >= 3 && strncmp(line, "RL ", 3) == 0) {
        int len = phone_b64_decode(line + 3, n - 3, out->pkt, (int)sizeof(out->pkt));
        out->kind = len > 0 ? PHONE_IN_RELAY : PHONE_IN_BAD;
        out->len = len > 0 ? len : 0;
        return;
    }
    if (n >= 10 && strncmp(line, "TEST START", 10) == 0 && (n == 10 || line[10] == ' ')) {
        int k = n > 11 ? n - 11 : 0; /* the parameters after "TEST START " */
        if (k > PHONE_ARGS_MAX) {
            out->kind = PHONE_IN_BAD;
            return;
        }
        memcpy(out->args, line + (n > 11 ? 11 : n), (size_t)k);
        out->args[k] = '\0';
        out->kind = PHONE_IN_TEST_START;
        return;
    }
    if (n == 9 && strncmp(line, "TEST STOP", 9) == 0) {
        out->kind = PHONE_IN_TEST_STOP;
        return;
    }
    if (n == 11 && strncmp(line, "BENCH POWER", 11) == 0) {
        out->kind = PHONE_IN_BENCH_POWER;
        return;
    }
    if (n == 10 && strncmp(line, "BENCH STOP", 10) == 0) {
        out->kind = PHONE_IN_BENCH_STOP;
        return;
    }
    out->kind = PHONE_IN_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* Line assembly                                                              */
/* ------------------------------------------------------------------------- */

/* phone_rx_init() -- nothing collected yet. */
void phone_rx_init(phone_rx_t *r)
{
    memset(r, 0, sizeof(*r));
}

/*
 * phone_rx_byte() -- see phone.h. '\r' is dropped wherever it appears (CRLF
 * and lone CR line ends both come from terminal apps); '\n' ends a line.
 */
bool phone_rx_byte(phone_rx_t *r, uint8_t c)
{
    if (c == '\r') {
        return false;
    }
    if (c == '\n') {
        bool complete = !r->overflow && r->n > 0; /* not empty, not too long */
        r->buf[r->n] = '\0';
        r->n = 0;            /* the next byte starts a new line (the caller reads buf first) */
        r->overflow = false;
        return complete;
    }
    if (r->overflow) {
        return false;        /* still inside a line that was too long */
    }
    if (r->n >= PHONE_LINE_MAX) {
        r->overflow = true;  /* drop the whole line, not just its end */
        return false;
    }
    r->buf[r->n++] = (char)c;
    return false;
}

/* ------------------------------------------------------------------------- */
/* The relay                                                                  */
/* ------------------------------------------------------------------------- */

/* add_target() -- append `a` to to[] unless it is already there, invalid,
 * ourselves, or the list is full. */
static void add_target(uint32_t *to, int *n, int max, uint32_t a, uint32_t own)
{
    if (a == own || !proto_addr_valid(a) || *n >= max) {
        return;
    }
    for (int i = 0; i < *n; i++) {
        if (to[i] == a) {
            return;
        }
    }
    to[(*n)++] = a;
}

/* phone_relay_targets() -- see phone.h for the rules. */
int phone_relay_targets(const uint8_t *pkt, int len, uint32_t own, const uint32_t *contacts, int n_contacts,
                        uint32_t partner, uint32_t *to, int max)
{
    pkt_t p;
    if (proto_decode(pkt, len, &p) != PROTO_OK || p.src != own) {
        return 0; /* not ours: a relayed SOS stays on the radio */
    }
    if (p.type == PT_DISCOVER || p.type == PT_PRESENCE || p.type == PT_PING) {
        return 0; /* these measure the radio itself */
    }
    int n = 0;
    if (p.dst != ADDR_BROADCAST) {
        add_target(to, &n, max, p.dst, own);
        return n;
    }
    if (p.type == PT_SOS || p.type == PT_SOS_CANCEL) {
        add_target(to, &n, max, partner, own);
        for (int i = 0; i < n_contacts; i++) {
            add_target(to, &n, max, contacts[i], own);
        }
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Messages announced with TX, until their OK / NO                            */
/* ------------------------------------------------------------------------- */

/* phone_tx_init() -- see phone.h: nothing sent yet, nothing waiting. */
void phone_tx_init(phone_tx_t *t)
{
    memset(t, 0, sizeof(*t));
}

/* phone_tx_add() -- see phone.h. */
unsigned phone_tx_add(phone_tx_t *t, uint32_t to)
{
    if (t->n_pend == PHONE_TX_MAX) { /* (can't happen: see PHONE_TX_MAX) keep the newest */
        memmove(&t->pend[0], &t->pend[1], (size_t)(t->n_pend - 1) * sizeof(t->pend[0]));
        t->n_pend--;
    }
    t->pend[t->n_pend].n = ++t->last;
    t->pend[t->n_pend].to = to;
    t->n_pend++;
    return t->last;
}

/*
 * phone_tx_settle() -- see phone.h. DELIVERED settles the oldest entry for
 * `to`, REFUSED the newest one, DROPPED every one for `to`, ALL_FAILED all
 * of them. The rest keep waiting, in order.
 */
int phone_tx_settle(phone_tx_t *t, phone_tx_res_t r, uint32_t to, unsigned *done, int max)
{
    int one = -1; /* DELIVERED / REFUSED: the one entry concerned */
    for (int i = 0; i < t->n_pend; i++) {
        if (t->pend[i].to == to && (r == PHONE_TX_REFUSED || one < 0)) {
            one = i; /* the first match (oldest), or for REFUSED the last (newest) */
        }
    }
    int w = 0, k = 0;
    for (int i = 0; i < t->n_pend; i++) {
        bool settle = r == PHONE_TX_ALL_FAILED ? true : r == PHONE_TX_DROPPED ? t->pend[i].to == to : i == one;
        if (!settle) {
            t->pend[w++] = t->pend[i]; /* still waiting */
        } else if (k < max) {
            done[k++] = t->pend[i].n;
        }
    }
    t->n_pend = w;
    return k;
}
