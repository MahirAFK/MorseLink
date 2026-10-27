/*
 * MorseLink — test mode in the net task (see ntest.h; the plan is
 * docs/TEST_PLAN.md).
 *
 * The board makes its share of the test traffic itself (ttest.h decides
 * when, how long and the text) and logs every event as one line,
 *   T,<n>,<board ms>,<EVENT>,<key>=<value>,...
 * printed on the console and sent to the phone, which adds the wall-clock
 * time and GPS and writes the session file. While the phone is away the
 * lines wait in a RAM ring (NT_RING_N); if that overflows, a GAP line says
 * how many were lost, and the line numbers show where.
 *
 * Paths. Each test message goes as up to two copies with the same sequence
 * number (ttest.h: the marker's L or R):
 *   - the L copy through the link engine over LoRa, with the engine's real
 *     retries and receipt (link_send_msg). Skipped while the link is lost,
 *     while the hour's airtime is above 8 %, or when 4 are already queued;
 *   - the R copy straight to the phone as a relay packet, flag
 *     PF_VIA_PHONE, its own message ID. The other board's test mode answers
 *     it with a receipt that also goes back through the phones; no answer
 *     within NT_RELAY_WAIT_MS counts as failed. Skipped without a phone.
 * While a test runs nothing else uses the phone path: the usual mirroring
 * of packets to the phone stops (net.c asks ntest_active()), and whatever
 * comes from the phone is handled (and logged) here and never reaches the
 * engine, so LoRa's link state and signal readings stay LoRa's alone.
 *
 * The UI hears test mode is on / off (ui_post_test): it then plays nothing
 * (except rundown-m's motor loop) and ignores the key except a 5 s hold,
 * which stops the test.
 *
 * Run-downs (rundown-a / -b / -m / -l) also log the battery to flash once a
 * minute (store.h): its voltage and the charging flag, and for rundown-m
 * each motor's on-time in that minute; "meta" holds the run's details for
 * the header of `test dump` (TEST_PLAN §11, §14).
 *
 * The link during a test (v0.6.0): the relay doesn't count as a path while
 * a test runs (net.c tells the engine), so the link's state is LoRa's. Every
 * loss is a LINK line with its reason, and every change of a path's status
 * a PATH line, so the analysis sees exactly when LoRa was up.
 *
 * The point test (v0.6.1, agreed 20 Oct; ttest.h has its timetable): both
 * boards still, a plan of radio settings. At each step this board switches
 * its spreading factor and power (set_radio: never saved; a STEP line says
 * what it uses), sends its 20 messages over LoRa, and goes on sending while
 * the link is lost (link_send_test_msg): at the edge of range the delivery
 * is what is being measured. A message due while the one before still waits
 * for its result is held until that one is settled (late_ms in its GEN
 * line), so each message is a try of its own; the gaps are long enough that
 * this hardly ever happens. When the plan is over (or the test stops for
 * any reason) the board's own setting comes back (STEP i=0 ... restored=1).
 */
#include "ntest.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "haptics.h"
#include "morse.h"
#include "phone.h"
#include "proto.h"
#include "store.h"
#include "ttest.h"
#include "ui.h"

static const char *TAG = "test";

#define NT_LINE_MAX 224            /* one log line, NUL included (a point test's START with an 8-step plan
                                      is about 210 characters; phone lines go up to PHONE_LINE_MAX 240) */
#define NT_RING_N 128              /* log lines kept while the phone is away (28 KB, only during a test) */
#define NT_FLUSH_KEEP 4            /* BLE queue slots left free for everything else */
#define NT_LQ_MAX (MSG_QUEUE_MAX + 1) /* L copies the engine can hold */
#define NT_RQ_MAX 8                /* R copies waiting for their receipt */
#define NT_RELAY_WAIT_MS 30000     /* an R copy without a receipt by then has failed */
#define NT_STAT_MS 60000           /* a STAT line every minute */
#define NT_DUTY_SKIP_PM 80         /* skip the L copy above 8 % airtime in the last hour (per mille) */
#define NT_DRAIN_MS 35000          /* after a stop: wait at most this long for copies still out */
#define NT_TXC_N 16                /* (type, ID) pairs remembered to number transmitted copies */
#define NT_RD_SAMPLE_MS 60000      /* run-down: one battery sample a minute */
#define NT_RD_SAVE_EVERY 10        /* ... written to flash every 10 samples */

/* An L copy in the engine, waiting for its receipt (FIFO, like the engine). */
typedef struct {
    uint16_t seq;
    uint8_t size;
    uint8_t copies;    /* transmitted so far */
    uint16_t id;       /* the engine's message ID (known from the first transmission) */
    int64_t gen_ms;    /* generated */
    int64_t first_tx;  /* first transmission (0: not yet) */
} nt_lmsg_t;

/* An R copy handed to the phone, waiting for its receipt. */
typedef struct {
    uint16_t seq;
    uint8_t size;
    uint16_t id;
    int64_t sent_ms;
} nt_rmsg_t;

static struct {
    link_t *eng;
    int (*noise_dbm)(void);
    int (*set_radio)(int sf, int dbm); /* net.c: switch the radio for a point test's step (ntest_init) */
    volatile bool on;        /* a test runs */
    volatile bool draining;  /* stopped; copies still out (their results are still ours) */
    bool from_phone;
    tt_params_t p;
    tt_sched_t sch;
    uint32_t peer;           /* our partner when the test started */
    uint32_t n;              /* log lines so far this session */
    int64_t next_stat;
    int64_t drain_until;
    char stop_reason[16];
    nt_lmsg_t lq[NT_LQ_MAX];
    int nlq;
    nt_rmsg_t rq[NT_RQ_MAX];
    int nrq;
    dedup_t rx_dd;           /* radio packets seen (dup= in RX lines) */
    dedup_t rin_dd;          /* phone packets seen (dup= in RIN lines) */
    struct {
        uint8_t type;
        uint16_t id;
        uint8_t n;
    } txc[NT_TXC_N];         /* copies transmitted per (type, ID) */
    int txc_next;
    char (*ring)[NT_LINE_MAX]; /* log lines for the phone (allocated at the first start) */
    int r_head, r_n;
    uint32_t r_lost;         /* lines dropped from a full ring, not yet reported */
    uint32_t gen, ack[2], fail[2], skip[2]; /* totals; [0] = L, [1] = R */
    int bat_mv, bat_pct;
    bool chg;
    /* run-down log */
    int64_t next_sample;
    uint16_t rd_buf[RD_BLOCK];          /* this hour's battery samples */
    uint8_t rd_mot[2 * RD_BLOCK];       /* rundown-m: this hour's motor on-time (M1, M2 per sample) */
    bool rd_motors;                     /* keep rd_mot (rundown-m) */
    uint32_t rd_on_last[2];             /* each motor's on-time total (haptics_on_time_ms) at the last sample */
    uint32_t rd_on_rest[2];             /* ms not yet counted (under one RD_MOTOR_UNIT_MS step): carried over */
    int rd_n, rd_block;
    uint32_t rd_total;
    /* point test (v0.6.1) */
    int pt_step;           /* the plan step whose setting is on (0-based); -1: none (not switched, or put back) */
    int pt_keep_sf;        /* this board's own setting at the start: put back at the end */
    int pt_keep_dbm;
    bool pt_hold;          /* a message came due while the one before still waited for its result */
    uint16_t pt_hold_seq;  /* ... its sequence number, */
    int pt_hold_step;      /* ... its step (1-based) */
    int64_t pt_hold_due;   /* ... and when it was due */
} T;

/* ------------------------------------------------------------------------- */
/* Log lines                                                                  */
/* ------------------------------------------------------------------------- */

/* idstr() -- printable ID for an address: "ALL", "-" or "?" when not an ID. */
static const char *idstr(uint32_t a, char buf[ID_MAX_LEN + 1])
{
    if (a == ADDR_BROADCAST) {
        return strcpy(buf, "ALL");
    }
    if (a == ADDR_NONE) {
        return strcpy(buf, "-");
    }
    if (!addr_to_id(a, buf)) {
        strcpy(buf, "?");
    }
    return buf;
}

/* ring_push() -- keep a line for the phone; a full ring drops its oldest
 * line (counted, for the GAP line). */
static void ring_push(const char *line)
{
    if (!T.ring) {
        return;
    }
    if (T.r_n == NT_RING_N) {
        T.r_head = (T.r_head + 1) % NT_RING_N;
        T.r_n--;
        T.r_lost++;
    }
    int slot = (T.r_head + T.r_n) % NT_RING_N;
    strncpy(T.ring[slot], line, NT_LINE_MAX - 1);
    T.ring[slot][NT_LINE_MAX - 1] = '\0';
    T.r_n++;
}

/*
 * emit() -- one log line: "T,<n>,<board ms>,<EVENT>" plus the fields
 * (printf-style, "k=v,k=v", may be empty). Printed on the console at once,
 * and queued for the phone (flush() sends it when there is room).
 */
static void emit(int64_t now, const char *event, const char *fmt, ...)
{
    char line[NT_LINE_MAX];
    int k = snprintf(line, sizeof line, "T,%lu,%lld,%s", (unsigned long)++T.n, (long long)now, event);
    if (fmt && fmt[0] && k > 0 && k < (int)sizeof line - 1) {
        line[k++] = ',';
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line + k, sizeof line - (size_t)k, fmt, ap);
        va_end(ap);
    }
    ESP_LOGI(TAG, "%s", line);
    ring_push(line);
}

/* emit_now() -- a line outside a session (REFUSED, n = 0): straight to the
 * console and, if one is there, to the phone. */
static void emit_now(int64_t now, const char *event, const char *fields)
{
    char line[NT_LINE_MAX];
    snprintf(line, sizeof line, "T,0,%lld,%s,%s", (long long)now, event, fields);
    ESP_LOGW(TAG, "%s", line);
    if (ble_phone_ready()) {
        ble_send_line(line);
    }
}

/* flush() -- send queued lines while the phone is there and BLE has room,
 * first a GAP line if some were lost. */
static void flush(int64_t now)
{
    if (!T.ring || !ble_phone_ready()) {
        return;
    }
    if (T.r_lost && ble_tx_space() > NT_FLUSH_KEEP) {
        char line[64];
        snprintf(line, sizeof line, "T,%lu,%lld,GAP,lost=%lu", (unsigned long)++T.n, (long long)now,
                 (unsigned long)T.r_lost);
        ESP_LOGW(TAG, "%s", line);
        ble_send_line(line);
        T.r_lost = 0;
    }
    while (T.r_n && ble_tx_space() > NT_FLUSH_KEEP) {
        ble_send_line(T.ring[T.r_head]);
        T.r_head = (T.r_head + 1) % NT_RING_N;
        T.r_n--;
    }
}

/* duty_pm() -- airtime used in the last hour, per mille of the hour. */
static unsigned long duty_pm(int64_t now)
{
    return (unsigned long)(link_duty_used_ms(T.eng, now) / 3600u);
}

/* ------------------------------------------------------------------------- */
/* Results                                                                    */
/* ------------------------------------------------------------------------- */

/* ack_lora() -- the oldest L copy got its receipt (the engine's receipts come
 * in order). rtt_ms counts from its first transmission, rtt_gen_ms from when
 * it was generated (queueing included). */
static void ack_lora(int64_t now)
{
    if (!T.nlq) {
        return;
    }
    const nt_lmsg_t *m = &T.lq[0];
    T.ack[0]++;
    emit(now, "ACK", "path=L,seq=%u,id=%u,size=%u,rtt_ms=%lld,rtt_gen_ms=%lld,tries=%u", m->seq, m->id, m->size,
         (long long)(m->first_tx ? now - m->first_tx : -1), (long long)(now - m->gen_ms), m->copies);
    memmove(&T.lq[0], &T.lq[1], (size_t)(T.nlq - 1) * sizeof(T.lq[0]));
    T.nlq--;
}

/* fail_lora_all() -- the engine gave up: every L copy waiting has failed. */
static void fail_lora_all(int64_t now, const char *why)
{
    for (int i = 0; i < T.nlq; i++) {
        const nt_lmsg_t *m = &T.lq[i];
        T.fail[0]++;
        emit(now, "FAIL", "path=L,seq=%u,id=%u,size=%u,tries=%u,why=%s", m->seq, m->id, m->size, m->copies, why);
    }
    T.nlq = 0;
}

/* settle_relay() -- R copy i got its receipt (ok) or timed out. */
static void settle_relay(int i, bool ok, int64_t now)
{
    const nt_rmsg_t *m = &T.rq[i];
    if (ok) {
        T.ack[1]++;
        emit(now, "ACK", "path=R,seq=%u,id=%u,size=%u,rtt_ms=%lld,rtt_gen_ms=%lld,tries=1", m->seq, m->id, m->size,
             (long long)(now - m->sent_ms), (long long)(now - m->sent_ms));
    } else {
        T.fail[1]++;
        emit(now, "FAIL", "path=R,seq=%u,id=%u,size=%u,tries=1,why=timeout", m->seq, m->id, m->size);
    }
    memmove(&T.rq[i], &T.rq[i + 1], (size_t)(T.nrq - i - 1) * sizeof(T.rq[0]));
    T.nrq--;
}

/* ------------------------------------------------------------------------- */
/* Sending                                                                    */
/* ------------------------------------------------------------------------- */

/* relay_send() -- hand a packet of ours to the phone for `p->dst`'s topic
 * (an RL line, phone.h), and log it (ROUT). */
static void relay_send(const pkt_t *p, int64_t now)
{
    uint8_t buf[PROTO_MAX_PKT];
    char line[PHONE_LINE_MAX + 1], d[ID_MAX_LEN + 1];
    int len = proto_encode(p, buf, sizeof buf);
    if (len <= 0 || phone_fmt_relay(line, sizeof line, p->dst, buf, len) <= 0) {
        return;
    }
    ble_send_line(line);
    emit(now, "ROUT", "type=%s,dst=%s,id=%u,bytes=%d", proto_type_name(p->type), idstr(p->dst, d), p->id, len);
}

/*
 * generate() -- message `seq` of `size` characters is due: the L copy into
 * the engine (unless a skip rule applies), the R copy to the phone (if one
 * is there), and a GEN line saying what went where. A point test's message
 * also carries its step (1-based), how late it went (late_ms: it was held,
 * see ntest_tick) and may come with a skip reason decided by the caller
 * (`force`: "late", "stopped"); its L copy goes even while the link is lost.
 */
static void generate(uint16_t seq, int size, int step, int64_t late_ms, const char *force, int64_t now)
{
    char lora[24], relay[24], text[TT_SIZE_MAX + 1], pt[40] = "";
    const bool point = T.p.test == TT_POINT;
    T.gen++;
    if (point) {
        snprintf(pt, sizeof pt, ",step=%d", step);
    }
    if (tt_uses_lora(T.p.paths)) {
        const char *skip = force;
        /* The user's rule: no sending while the link is lost. A point test
         * sends anyway (the edge of range is its measurement), but not to
         * someone who is no longer the partner. */
        const bool linked = T.eng->state == LS_UP || (point && T.eng->state == LS_LOST);
        if (skip) {
            /* decided by the caller */
        } else if (T.eng->peer != T.peer || !linked) {
            skip = "lost";
        } else if (duty_pm(now) >= NT_DUTY_SKIP_PM) {
            skip = "duty"; /* leave room for receipts and keepalives */
        } else if (T.nlq >= NT_LQ_MAX) {
            skip = "queue";
        } else if (tt_text(text, sizeof text, seq, 'L', size) < 0 ||
                   !(point ? link_send_test_msg : link_send_msg)(T.eng, T.peer, text, now)) {
            skip = "refused";
        } else {
            nt_lmsg_t *m = &T.lq[T.nlq++];
            memset(m, 0, sizeof(*m));
            m->seq = seq;
            m->size = (uint8_t)size;
            m->gen_ms = now;
        }
        if (skip) {
            T.skip[0]++;
            snprintf(lora, sizeof lora, "lora_skip=%s", skip);
        } else {
            strcpy(lora, "lora=queued");
        }
    } else {
        strcpy(lora, "lora=off");
    }
    pkt_t r;
    bool send_r = false;
    if (tt_uses_relay(T.p.paths)) {
        if (!ble_phone_ready()) {
            T.skip[1]++;
            strcpy(relay, "relay_skip=nophone");
        } else if (T.nrq >= NT_RQ_MAX) {
            T.skip[1]++;
            strcpy(relay, "relay_skip=queue");
        } else {
            proto_init_pkt(&r, PT_MSG, T.eng->own, T.peer, link_new_id(T.eng));
            r.flags = PF_VIA_PHONE;
            r.plen = (uint8_t)tt_text((char *)r.payload, sizeof r.payload, seq, 'R', size);
            send_r = true;
            snprintf(relay, sizeof relay, "relay_id=%u", r.id);
        }
    } else {
        strcpy(relay, "relay=off");
    }
    if (point && late_ms > 0) {
        size_t k = strlen(pt);
        snprintf(pt + k, sizeof pt - k, ",late_ms=%lld", (long long)late_ms);
    }
    emit(now, "GEN", "seq=%u,size=%d%s,%s,%s", seq, size, pt, lora, relay);
    if (send_r) {
        relay_send(&r, now);
        nt_rmsg_t *m = &T.rq[T.nrq++];
        m->seq = seq;
        m->size = (uint8_t)size;
        m->id = r.id;
        m->sent_ms = now;
    }
}

/* ------------------------------------------------------------------------- */
/* Start, stop, the clock                                                     */
/* ------------------------------------------------------------------------- */

/* ntest_init() -- see ntest.h. */
void ntest_init(link_t *eng, int (*noise_dbm)(void), int (*set_radio)(int sf, int dbm))
{
    T.eng = eng;
    T.noise_dbm = noise_dbm;
    T.set_radio = set_radio;
    T.bat_mv = T.bat_pct = -1;
    T.pt_step = -1;
}

/* ------------------------------------------------------------------------- */
/* The point test's steps                                                     */
/* ------------------------------------------------------------------------- */

/* pt_drop_held() -- a held message whose turn never came (its step ended,
 * or the test stopped): logged as generated and skipped, so the counts add
 * up. */
static void pt_drop_held(const char *why, int64_t now)
{
    if (T.pt_hold) {
        T.pt_hold = false;
        generate(T.pt_hold_seq, TT_POINT_SIZE, T.pt_hold_step, now - T.pt_hold_due, why, now);
    }
}

/*
 * pt_switch() -- plan step i (0-based) begins: the radio to its setting
 * (applied by the net task between packets; never saved) and a STEP line
 * with what this board uses (the power clamped to its chip) and how long
 * the step lasts. A message still held from the step before is dropped.
 */
static void pt_switch(int i, int64_t now)
{
    pt_drop_held("late", now);
    const tt_step_t *st = &T.p.plan[i];
    int dbm = T.set_radio ? T.set_radio(st->sf, st->dbm) : st->dbm;
    T.pt_step = i;
    emit(now, "STEP", "i=%d,sf=%u,dbm=%d,n=%d,step_ms=%ld", i + 1, st->sf, dbm, tt_point_n(&T.p),
         (long)tt_point_step_ms(st->sf, tt_point_n(&T.p)));
}

/* pt_restore() -- the point test is over (done, stopped, refused...): this
 * board's own setting back, once, with its STEP line (i=0). */
static void pt_restore(int64_t now)
{
    if (T.pt_step < 0) {
        return;
    }
    T.pt_step = -1;
    int dbm = T.set_radio ? T.set_radio(T.pt_keep_sf, T.pt_keep_dbm) : T.pt_keep_dbm;
    emit(now, "STEP", "i=0,sf=%d,dbm=%d,restored=1", T.pt_keep_sf, dbm);
}

/* ntest_refuse() -- see ntest.h. */
void ntest_refuse(const char *reason, int64_t now)
{
    char f[48];
    snprintf(f, sizeof f, "reason=%s", reason);
    emit_now(now, "REFUSED", f);
}

/* copy_text() -- copy a string into a fixed field, cut to fit, always
 * NUL-terminated. */
static void copy_text(char *dst, size_t size, const char *src)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

/*
 * rd_begin() -- a run-down starts: a fresh log in flash, with the run's
 * details (store.h rd_meta_t: the dump's header, with the spreading factor
 * since v0.6.0), and the motor on-time counters taken from where they stand
 * now.
 */
static void rd_begin(const tt_params_t *p, int tx_dbm)
{
    rd_meta_t m;
    memset(&m, 0, sizeof m);
    m.version = RD_META_VERSION;
    m.test = p->test;
    m.paths = p->paths;
    m.role = p->role;
    m.tx_dbm = (int8_t)tx_dbm;
    m.sf = p->sf;
    hparams_t hp;
    haptics_get_params(&hp); /* the reading speed the UI has set */
    m.unit_lo = (uint8_t)(hp.unit_ms & 0xFF);
    m.unit_hi = (uint8_t)(hp.unit_ms >> 8);
    haptics_get_intensity(&m.m1_pct, &m.m2_pct);
    m.motors = tt_plays(p->test) ? 1 : 0;
    char id[ID_MAX_LEN + 1];
    copy_text(m.own, sizeof m.own, idstr(T.eng->own, id));
    copy_text(m.peer, sizeof m.peer, idstr(T.eng->peer, id));
    copy_text(m.session, sizeof m.session, p->session[0] ? p->session : "-");
    copy_text(m.fw, sizeof m.fw, MORSELINK_VERSION);
    if (store_rd_begin(&m) != ESP_OK) {
        ESP_LOGE(TAG, "run-down log: flash write failed (the samples may be lost)");
    }
    T.rd_n = T.rd_block = 0;
    T.rd_total = 0;
    T.rd_motors = m.motors != 0;
    haptics_on_time_ms(&T.rd_on_last[0], &T.rd_on_last[1]);
    T.rd_on_rest[0] = T.rd_on_rest[1] = 0;
}

/* ntest_start() -- see ntest.h. A test whose messages go over LoRa needs
 * LoRa itself up (or still checking), not just the link: during the test
 * the relay doesn't count, so a link carried only by the phones would be
 * lost the moment it started. rundown-l runs with Bluetooth off, so it
 * uses LoRa only, whatever the phone asked. */
void ntest_start(const char *args, bool from_phone, bool loopback, int tx_dbm, int sf, int64_t now)
{
    tt_params_t p;
    const char *why = NULL;
    if (T.on || T.draining) {
        why = "running";
    } else if (!tt_parse_params(args, &p, &why)) {
        /* why set by the parser */
    } else if (T.eng->own == ADDR_NONE) {
        why = "no-id";
    } else if (loopback && from_phone) {
        why = "loopback"; /* a field test must use the radio */
    } else if (T.eng->peer == ADDR_NONE) {
        why = "no-partner";
    } else if (T.eng->state != LS_UP) {
        why = "link-lost";
    } else if (tt_sends(p.test) && (tt_uses_lora(p.paths) || p.test == TT_RUNDOWN_L) &&
               link_path_state(T.eng, LP_LORA, now) != PS_UP && link_path_state(T.eng, LP_LORA, now) != PS_CHECKING) {
        why = "lora-lost"; /* (the relay may carry the link, but this test needs LoRa) */
    } else if (T.eng->n_msg) {
        why = "busy"; /* a message of the user's still waits for its receipt: its result would be taken for the test's */
    }
    if (!why && !T.ring) {
        T.ring = malloc(sizeof(*T.ring) * NT_RING_N);
        if (!T.ring) {
            why = "no-memory";
        }
    }
    if (why) {
        ntest_refuse(why, now);
        return;
    }
    if (p.test == TT_RUNDOWN_L) {
        p.paths = TT_LORA; /* Bluetooth is off for the run: no relay copies */
    }
    /* (a point test is LoRa only too: tt_parse_params set its paths) */
    p.sf = (uint8_t)sf; /* the pace follows this board's setting (ttest.h) */
    T.p = p;
    T.from_phone = from_phone;
    T.peer = T.eng->peer;
    tt_init(&T.sch, &p, now);
    T.n = 0;
    T.r_head = T.r_n = 0;
    T.r_lost = 0;
    T.nlq = T.nrq = 0;
    dedup_init(&T.rx_dd);
    dedup_init(&T.rin_dd);
    memset(T.txc, 0, sizeof(T.txc));
    T.txc_next = 0;
    T.gen = 0;
    memset(T.ack, 0, sizeof(T.ack));
    memset(T.fail, 0, sizeof(T.fail));
    memset(T.skip, 0, sizeof(T.skip));
    T.next_stat = now + NT_STAT_MS;
    if (tt_is_rundown(p.test)) {
        rd_begin(&p, tx_dbm);
        T.next_sample = now + 5000; /* the first sample: the full battery (the UI's reading has arrived by then) */
    }
    T.on = true;
    T.draining = false;
    T.pt_step = -1;
    T.pt_hold = false;
    T.pt_keep_sf = sf;       /* (a point test puts these back at the end) */
    T.pt_keep_dbm = tx_dbm;
    char o[ID_MAX_LEN + 1], q[ID_MAX_LEN + 1], extra[80] = "";
    if (p.test == TT_POINT) {
        char plan[TT_PLAN_MAX * 6 + 1];
        snprintf(extra, sizeof extra, ",plan=%s,n=%d,size=%d", tt_plan_text(&p, '/', plan, sizeof plan),
                 tt_point_n(&p), TT_POINT_SIZE); /* ('/' between the steps: the line's fields are comma-separated) */
    }
    emit(now, "START", "session=%s,role=%s,test=%s,paths=%s,seed=%lu,tx_dbm=%d,sf=%d,fw=%s,own=%s,peer=%s,lb=%d%s",
         p.session[0] ? p.session : "-", tt_role_name(p.role), tt_test_name(p.test), tt_paths_name(p.paths),
         (unsigned long)p.seed, tx_dbm, sf, MORSELINK_VERSION, idstr(T.eng->own, o), idstr(T.peer, q), loopback ? 1 : 0,
         extra);
    if (p.test == TT_POINT) {
        pt_switch(0, now); /* the first step's setting from the start: its settle time covers the switch */
    }
    ui_post_test(true, p.test);
}

/* rd_save() -- write the current hour's run-down samples (and, in
 * rundown-m, its motor on-time) to flash. */
static void rd_save(void)
{
    if (T.rd_n) {
        store_rd_save(T.rd_block, T.rd_buf, T.rd_motors ? T.rd_mot : NULL, T.rd_n, T.rd_total);
    }
}

/* finish() -- the copies still out have their results (or ran out of
 * time): the STOP line with the totals. */
static void finish(int64_t now)
{
    if (T.nlq) {
        fail_lora_all(now, "stopped");
    }
    while (T.nrq) {
        settle_relay(0, false, now);
    }
    emit(now, "STOP", "reason=%s,gen=%lu,ack_l=%lu,fail_l=%lu,skip_l=%lu,ack_r=%lu,fail_r=%lu,skip_r=%lu",
         T.stop_reason, (unsigned long)T.gen, (unsigned long)T.ack[0], (unsigned long)T.fail[0],
         (unsigned long)T.skip[0], (unsigned long)T.ack[1], (unsigned long)T.fail[1], (unsigned long)T.skip[1]);
    if (tt_is_rundown(T.p.test)) {
        rd_save();
        store_rd_end(T.stop_reason); /* the dump's header shows how it ended */
    }
    T.draining = false;
}

/* ntest_stop() -- see ntest.h. */
void ntest_stop(const char *reason, int64_t now)
{
    if (!T.on) {
        if (!T.draining) {
            ESP_LOGW(TAG, "no test is running");
        }
        return;
    }
    T.on = false;
    T.draining = true;
    T.drain_until = now + NT_DRAIN_MS;
    strncpy(T.stop_reason, reason, sizeof(T.stop_reason) - 1);
    T.stop_reason[sizeof(T.stop_reason) - 1] = '\0';
    if (T.p.test == TT_POINT) {
        pt_drop_held("stopped", now);
        pt_restore(now); /* the board's own setting back at once, whatever stopped it */
    }
    ui_post_test(false, T.p.test);
    if (!T.nlq && !T.nrq) {
        finish(now);
    }
}

/* ntest_battery() -- see ntest.h. */
void ntest_battery(int mv, int pct, bool charging)
{
    T.bat_mv = mv;
    T.bat_pct = pct;
    T.chg = charging;
}

/* stat_line() -- the minute's STAT line. */
static void stat_line(int64_t now)
{
    emit(now, "STAT", "bat_mv=%d,bat_pct=%d,chg=%d,duty_pm=%lu,noise_dbm=%d,heap=%u,queue=%d,ble=%d", T.bat_mv,
         T.bat_pct, T.chg ? 1 : 0, duty_pm(now), T.noise_dbm ? T.noise_dbm() : 0,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), T.nlq, ble_phone_ready() ? 1 : 0);
}

/*
 * rd_motor_step() -- motor `k`'s on-time since the last sample, in
 * RD_MOTOR_UNIT_MS steps (one byte; a minute is 240). The part under one
 * step is carried into the next sample, so the run's total stays exact.
 */
static uint8_t rd_motor_step(int k, uint32_t total_now)
{
    uint32_t ms = (total_now - T.rd_on_last[k]) + T.rd_on_rest[k]; /* unsigned: survives the 49-day wrap */
    T.rd_on_last[k] = total_now;
    uint32_t steps = ms / RD_MOTOR_UNIT_MS;
    T.rd_on_rest[k] = ms % RD_MOTOR_UNIT_MS;
    if (steps > 255) {
        T.rd_on_rest[k] += (steps - 255) * RD_MOTOR_UNIT_MS; /* a late sample: the excess goes into the next one */
        steps = 255;
    }
    return (uint8_t)steps;
}

/* rd_sample() -- run-down: the minute's battery sample (and in rundown-m
 * the motors' on-time), to flash every 10. */
static void rd_sample(void)
{
    if (T.rd_motors) {
        uint32_t on[2];
        haptics_on_time_ms(&on[0], &on[1]);
        T.rd_mot[2 * T.rd_n] = rd_motor_step(0, on[0]);
        T.rd_mot[2 * T.rd_n + 1] = rd_motor_step(1, on[1]);
    }
    T.rd_buf[T.rd_n++] = RD_SAMPLE(T.bat_mv > 0 ? T.bat_mv : 0, T.chg);
    T.rd_total++;
    if (T.rd_total % NT_RD_SAVE_EVERY == 0 || T.rd_n == RD_BLOCK) {
        rd_save();
    }
    if (T.rd_n == RD_BLOCK) {
        T.rd_block++;
        T.rd_n = 0;
    }
    T.next_sample += NT_RD_SAMPLE_MS;
}

/* ntest_tick() -- see ntest.h. */
void ntest_tick(int64_t now)
{
    if (T.on && T.p.test == TT_POINT) {
        /* The point test's timetable: switch at each step's start; at the
         * end, the board's own setting back and the test is done. */
        int i = tt_point_step_at(&T.p, now - T.sch.start_ms);
        if (i >= T.p.n_steps) {
            ntest_stop("done", now); /* (puts the setting back first) */
        } else if (i != T.pt_step) {
            pt_switch(i, now);
        }
    }
    if (T.on) {
        uint16_t seq;
        int size;
        if (tt_next(&T.sch, now, &seq, &size)) {
            if (T.p.test == TT_POINT && T.nlq > 0) {
                /* The one before still waits for its result: hold this one
                 * until it is settled (a newer one replaces it). */
                pt_drop_held("late", now);
                T.pt_hold = true;
                T.pt_hold_seq = seq;
                T.pt_hold_step = T.sch.step;
                T.pt_hold_due = now;
            } else {
                generate(seq, size, T.sch.step, 0, NULL, now);
            }
        }
        if (T.pt_hold && T.nlq == 0) {
            T.pt_hold = false;
            generate(T.pt_hold_seq, TT_POINT_SIZE, T.pt_hold_step, now - T.pt_hold_due, NULL, now);
        }
        if (now >= T.next_stat) {
            stat_line(now);
            T.next_stat += NT_STAT_MS;
        }
        if (tt_is_rundown(T.p.test) && now >= T.next_sample) {
            rd_sample();
        }
        if (tt_finished(&T.sch) && !T.nlq && !T.nrq) {
            ntest_stop("done", now); /* baseline: all 120 sent and settled */
        }
    }
    for (int i = 0; i < T.nrq;) {
        if (now - T.rq[i].sent_ms >= NT_RELAY_WAIT_MS) {
            settle_relay(i, false, now);
        } else {
            i++;
        }
    }
    if (T.draining && ((!T.nlq && !T.nrq) || now >= T.drain_until)) {
        finish(now);
    }
    flush(now);
    if (!T.on && !T.draining && T.ring && T.r_n == 0) {
        free(T.ring); /* all sent: give the 28 KB back until the next test */
        T.ring = NULL;
    }
}

/* ntest_active() -- see ntest.h. */
bool ntest_active(void)
{
    return T.on || T.draining;
}

/* ------------------------------------------------------------------------- */
/* Hooks                                                                      */
/* ------------------------------------------------------------------------- */

/* tx_copy() -- this is the how-many-th transmission of (type, id)? */
static int tx_copy(uint8_t type, uint16_t id)
{
    for (int i = 0; i < NT_TXC_N; i++) {
        if (T.txc[i].n && T.txc[i].type == type && T.txc[i].id == id) {
            return ++T.txc[i].n;
        }
    }
    T.txc[T.txc_next].type = type;
    T.txc[T.txc_next].id = id;
    T.txc[T.txc_next].n = 1;
    T.txc_next = (T.txc_next + 1) % NT_TXC_N;
    return 1;
}

/* marker() -- ",seq=..,path=.." for a test text in a MSG, else "". */
static const char *marker(const pkt_t *p, char *buf, size_t size, uint16_t *seq, char *path)
{
    buf[0] = '\0';
    if (p->type == PT_MSG && tt_parse((const char *)p->payload, p->plen, seq, path)) {
        snprintf(buf, size, ",seq=%u,path=%c", *seq, *path);
    }
    return buf;
}

/* ntest_on_tx() -- see ntest.h. A transmitted L copy also notes its first
 * transmission, its copy count and its engine ID. */
void ntest_on_tx(const uint8_t *pkt, int len, uint32_t air_ms, int64_t lbt_ms, int tx_dbm, int64_t now)
{
    pkt_t p;
    if ((!T.on && !T.draining) || proto_decode(pkt, len, &p) != PROTO_OK) {
        return;
    }
    char d[ID_MAX_LEN + 1], mk[24];
    uint16_t seq = 0;
    char path = 0;
    int copy = tx_copy(p.type, p.id);
    marker(&p, mk, sizeof mk, &seq, &path);
    if (mk[0] && path == 'L') {
        for (int i = 0; i < T.nlq; i++) {
            if (T.lq[i].seq == seq) {
                if (!T.lq[i].first_tx) {
                    T.lq[i].first_tx = now;
                }
                T.lq[i].copies++;
                T.lq[i].id = p.id;
                break;
            }
        }
    }
    emit(now, "TX", "type=%s,dst=%s,id=%u,copy=%d,bytes=%d,air_ms=%lu,dbm=%d,lbt_ms=%lld,duty_pm=%lu%s",
         proto_type_name(p.type), idstr(p.dst, d), p.id, copy, len, (unsigned long)air_ms, tx_dbm, (long long)lbt_ms,
         duty_pm(now), mk);
}

/* ntest_on_rx() -- see ntest.h. dup=1 for a copy already heard. */
void ntest_on_rx(const uint8_t *pkt, int len, int16_t rssi, int8_t snr_x4, int64_t now)
{
    pkt_t p;
    if (!T.on && !T.draining) {
        return;
    }
    if (proto_decode(pkt, len, &p) != PROTO_OK) {
        emit(now, "RX", "type=?,bytes=%d,rssi=%d,snr=%.1f", len, rssi, snr_x4 / 4.0);
        return;
    }
    bool dup = p.type != PT_PING && p.type != PT_DISCOVER && p.type != PT_PRESENCE &&
               dedup_check(&T.rx_dd, p.src, p.id, now);
    char s[ID_MAX_LEN + 1], d[ID_MAX_LEN + 1], mk[24];
    uint16_t seq;
    char path;
    emit(now, "RX", "type=%s,src=%s,dst=%s,id=%u,bytes=%d,rssi=%d,snr=%.1f,dup=%d%s", proto_type_name(p.type),
         idstr(p.src, s), idstr(p.dst, d), p.id, len, rssi, snr_x4 / 4.0, dup ? 1 : 0,
         marker(&p, mk, sizeof mk, &seq, &path));
}

/* ntest_on_rx_bad() -- see ntest.h. */
void ntest_on_rx_bad(int64_t now)
{
    if (T.on || T.draining) {
        emit(now, "RXBAD", "");
    }
}

/*
 * ntest_on_phone_rx() -- see ntest.h. Logged (RIN) whatever it is; for us:
 * an R copy is answered with a receipt back through the phones (dup or not,
 * as the engine does over LoRa: our first receipt may have been lost); a
 * receipt for one of our R copies settles it. Nothing goes to the engine.
 */
bool ntest_on_phone_rx(const uint8_t *pkt, int len, int64_t now)
{
    if (!T.on && !T.draining) {
        return false;
    }
    pkt_t p;
    if (proto_decode(pkt, len, &p) != PROTO_OK) {
        emit(now, "RIN", "type=?,bytes=%d", len);
        return true;
    }
    bool dup = dedup_check(&T.rin_dd, p.src, p.id, now);
    char s[ID_MAX_LEN + 1], mk[24];
    uint16_t seq;
    char path;
    emit(now, "RIN", "type=%s,src=%s,id=%u,bytes=%d,dup=%d%s", proto_type_name(p.type), idstr(p.src, s), p.id, len,
         dup ? 1 : 0, marker(&p, mk, sizeof mk, &seq, &path));
    if (p.dst != T.eng->own || !(p.flags & PF_VIA_PHONE)) {
        return true; /* not a test packet for us: ignored */
    }
    if (p.type == PT_MSG) {
        pkt_t r;
        proto_init_pkt(&r, PT_MSG_RECEIPT, T.eng->own, p.src, link_new_id(T.eng));
        r.flags = PF_VIA_PHONE;
        proto_put16(r.payload, p.id);
        r.plen = 2;
        if (ble_phone_ready()) {
            relay_send(&r, now);
        }
    } else if (p.type == PT_MSG_RECEIPT && p.plen >= 2) {
        uint16_t ref = proto_get16(p.payload);
        for (int i = 0; i < T.nrq; i++) {
            if (T.rq[i].id == ref) {
                settle_relay(i, true, now);
                break;
            }
        }
    }
    return true;
}

/*
 * ntest_on_event() -- see ntest.h. While a test runs: our L copies'
 * results (receipt: ACK; the engine giving up: FAIL for all; a refusal was
 * already counted as a skip), and every message that arrives, are test
 * mode's; a normal message is logged and dropped (TEST_PLAN §4.2). Link
 * changes are logged and still passed on (the UI follows the link, quietly,
 * so the phone's status line says "lost" too: up to v0.5.1 it couldn't,
 * the 15 Oct bug): LINK with the reason of a loss, PATH for each change of
 * LoRa's (or the relay's) status. After a test, late test messages are
 * dropped too.
 */
bool ntest_on_event(const link_evt_t *e, int64_t now)
{
    bool live = T.on || T.draining;
    char id[ID_MAX_LEN + 1];
    switch (e->kind) {
    case LEV_MSG: {
        uint16_t seq;
        char path;
        bool test_text = tt_parse(e->text, (int)strlen(e->text), &seq, &path);
        if (live) {
            if (!test_text) {
                emit(now, "USERMSG", "src=%s,bytes=%d", idstr(e->addr, id), (int)strlen(e->text));
            }
            return true;
        }
        if (test_text) {
            ESP_LOGI(TAG, "a test message from %s arrived after the test: dropped", idstr(e->addr, id));
            return true;
        }
        return false;
    }
    case LEV_MSG_SENT:
        if (!live) {
            return false;
        }
        ack_lora(now);
        return true;
    case LEV_MSG_FAILED:
        if (!live) {
            return false;
        }
        if (e->arg == 0) {
            fail_lora_all(now, "no-receipt");
        }
        return true;
    case LEV_LINK_LOST:
        if (live) {
            emit(now, "LINK", "state=lost,why=%s,heard_ago_ms=%lld", link_lost_why_name(e->arg),
                 (long long)(now - T.eng->last_heard));
        }
        return false;
    case LEV_LINK_RESTORED:
    case LEV_LINK_ENDED:
        if (live) {
            emit(now, "LINK", "state=%s,heard_ago_ms=%lld", e->kind == LEV_LINK_RESTORED ? "restored" : "ended",
                 (long long)(now - T.eng->last_heard));
        }
        return false;
    case LEV_PATH:
        if (live) {
            uint8_t path = LINK_EV_PATH(e->arg);
            emit(now, "PATH", "path=%c,state=%s,heard_s=%ld,they_s=%ld", path == LP_LORA ? 'L' : 'R',
                 link_path_state_name(LINK_EV_STATE(e->arg)), (long)link_heard_ago_s(T.eng, path, now),
                 (long)link_they_heard_s(T.eng, path, now));
        }
        return false;
    default:
        return false;
    }
}
