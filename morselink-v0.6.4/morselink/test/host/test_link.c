/*
 * MorseLink — host tests for the packet layer (proto.c) and the link
 * protocol engine (link.c).
 *
 * The link tests run several engines against a simulated LoRa channel:
 *   - every packet occupies the air for its real airtime;
 *   - radios are half-duplex: a device transmitting hears nothing;
 *   - two packets overlapping in time at a receiver destroy each other;
 *   - hear[i][j] says whether j is in range of i (changeable mid-test);
 *   - each delivery can be lost at random (seeded, so runs repeat exactly);
 *   - before transmitting, a device waits while it can hear someone else
 *     (the device does the same with the SX1276's modem status).
 * A small "user" policy per device answers requests (accept / decline after
 * a delay) and acknowledges SOS, the way a person with the device would.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "phone.h"
#include "proto.h"
#include "ttest.h" /* the point test's gaps (test_point_gaps) */

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

static const lora_params_t LP = {RADIO_SF_DEFAULT, RADIO_BW_HZ, RADIO_CR};

/* Records what msgid_init / msgid_next ask to be saved to "flash". */
typedef struct {
    uint16_t value;
    int count;
} save_probe_t;

static void probe_save(void *ctx, uint16_t v)
{
    save_probe_t *p = ctx;
    p->value = v;
    p->count++;
}

/* ========================================================================= */
/* Packet layer                                                               */
/* ========================================================================= */

static void test_proto(void)
{
    printf("packet layer\n");
    uint32_t a = id_to_addr("K7Q"), b = id_to_addr("2345");
    uint8_t buf[PROTO_MAX_PKT + 4];
    pkt_t p, q;

    /* Round trip of a message. */
    proto_init_pkt(&p, PT_MSG, a, b, 0xBEEF);
    p.flags = PF_LONG_PREAMBLE | 2;
    memcpy(p.payload, "HELLO", 5);
    p.plen = 5;
    int n = proto_encode(&p, buf, sizeof buf);
    CHECK(n == PROTO_HDR_LEN + 5, "encoded length %d", n);
    CHECK(buf[0] == ((PROTO_VERSION << 4) | PT_MSG), "version/type byte 0x%02x", buf[0]);
    CHECK(proto_decode(buf, n, &q) == PROTO_OK, "decodes");
    CHECK(q.type == PT_MSG && q.src == a && q.dst == b && q.id == 0xBEEF && q.flags == p.flags && q.plen == 5 &&
              memcmp(q.payload, "HELLO", 5) == 0,
          "fields survive the round trip");

    /* Every type with its minimum payload. */
    static const uint8_t min_len[PT__COUNT] = {0, 2, 2, 0, 3, 2, 0, 1, 2, 0, 0, 0, 0, 4};
    for (int t = PT_DISCOVER; t < PT__COUNT; t++) {
        proto_init_pkt(&p, (uint8_t)t, a, t == PT_SOS ? ADDR_BROADCAST : b, (uint16_t)t);
        p.plen = min_len[t];
        memset(p.payload, 'A', p.plen);
        n = proto_encode(&p, buf, sizeof buf);
        CHECK(n > 0 && proto_decode(buf, n, &q) == PROTO_OK && q.type == t, "type %s round trip",
              proto_type_name((uint8_t)t));
    }

    /* Rejections. */
    proto_init_pkt(&p, PT_KEEPALIVE, a, b, 1);
    n = proto_encode(&p, buf, sizeof buf);
    CHECK(proto_decode(buf, n - 1, &q) == PROTO_ERR_SHORT, "short");
    CHECK(proto_decode(buf, PROTO_MAX_PKT + 1, &q) == PROTO_ERR_LONG, "long");
    uint8_t bad[PROTO_MAX_PKT];
    memcpy(bad, buf, (size_t)n);
    bad[0] = (uint8_t)((2 << 4) | PT_KEEPALIVE);
    CHECK(proto_decode(bad, n, &q) == PROTO_ERR_VERSION, "other version");
    for (int t = 14; t <= 16; t++) {
        bad[0] = (uint8_t)((PROTO_VERSION << 4) | (t & 0x0F));
        CHECK(proto_decode(bad, n, &q) == PROTO_ERR_TYPE, "type %d rejected", t & 0x0F);
    }
    memcpy(bad, buf, (size_t)n);
    bad[2] = bad[3] = bad[4] = 0xFF; /* source = broadcast */
    CHECK(proto_decode(bad, n, &q) == PROTO_ERR_ADDR, "broadcast source rejected");
    memcpy(bad, buf, (size_t)n);
    bad[5] = 0xFF, bad[6] = 0xFF, bad[7] = 0xFE; /* destination = "none" */
    CHECK(proto_decode(bad, n, &q) == PROTO_ERR_ADDR, "'none' destination rejected");
    proto_init_pkt(&p, PT_LINK_REPLY, a, b, 1);
    p.plen = 2; /* needs 3 */
    n = proto_encode(&p, buf, sizeof buf);
    CHECK(proto_decode(buf, n, &q) == PROTO_ERR_PAYLOAD, "short LINK_REPLY rejected");
    proto_init_pkt(&p, PT_MSG, a, b, 1);
    n = proto_encode(&p, buf, sizeof buf); /* empty message */
    CHECK(proto_decode(buf, n, &q) == PROTO_ERR_PAYLOAD, "empty MSG rejected");
    p.plen = MSG_MAX_CHARS + 1;
    memset(p.payload, 'E', p.plen);
    n = proto_encode(&p, buf, sizeof buf);
    CHECK(proto_decode(buf, n, &q) == PROTO_ERR_PAYLOAD, "101-character MSG rejected");
    proto_init_pkt(&p, PT_MSG, ADDR_NONE, b, 1);
    p.plen = 1;
    CHECK(proto_encode(&p, buf, sizeof buf) < 0, "encode refuses an invalid source");
    for (int t = PT_DISCOVER; t <= PT_PRESENCE; t++) {
        proto_init_pkt(&p, (uint8_t)t, a, ADDR_BROADCAST, 1);
        p.plen = 1; /* both need two bytes */
        n = proto_encode(&p, buf, sizeof buf);
        CHECK(proto_decode(buf, n, &q) == PROTO_ERR_PAYLOAD, "1-byte %s rejected", proto_type_name((uint8_t)t));
    }
    /* Discovery packets keep the 144 ms airtime of a bare header (disc.h). */
    CHECK(lora_airtime_us(PROTO_HDR_LEN + DISC_PRESENCE_LEN, &LP, 8) == lora_airtime_us(10, &LP, 8),
          "a PRESENCE takes as long as a 10-byte packet");
    CHECK(lora_airtime_us(PROTO_HDR_LEN + DISC_DISCOVER_LEN, &LP, 8) == lora_airtime_us(10, &LP, 8),
          "so does a DISCOVER");

    /* Airtime: the spec's figure for a 10-byte packet at SF9/125 kHz. */
    uint32_t t10 = lora_airtime_us(10, &LP, 8);
    CHECK(t10 == 144384, "10 bytes at SF9 = %u us (expected 144384)", t10);
    uint32_t t110 = lora_airtime_us(110, &LP, 8);
    CHECK(t110 > 580000 && t110 < 600000, "a full 100-character message = %u us", t110);
    lora_params_t sf12 = {12, 125000, 1};
    CHECK(lora_needs_ldro(&sf12) && !lora_needs_ldro(&LP), "low-data-rate optimisation only for slow settings");
    uint32_t t12 = lora_airtime_us(10, &sf12, 8);
    CHECK(t12 > 990000 && t12 < 992000, "10 bytes at SF12 = %u us (expected ~991 ms)", t12);

    /* Duty cycle: 10 % of a rolling hour. */
    dc_t dc;
    dc_init(&dc);
    CHECK(dc_budget_ms() == 360000, "budget %u ms", dc_budget_ms());
    dc_add(&dc, 0, 300000);
    CHECK(dc_used_ms(&dc, 1000) == 300000, "used %u", dc_used_ms(&dc, 1000));
    CHECK(dc_allow(&dc, 1000, 60000) && !dc_allow(&dc, 1000, 60001), "budget edge");
    dc_add(&dc, 30 * 60000, 50000);
    CHECK(dc_used_ms(&dc, 59 * 60000) == 350000, "both minutes inside the window");
    CHECK(dc_used_ms(&dc, 60 * 60000) == 50000, "minute 0 has left the window: %u", dc_used_ms(&dc, 60 * 60000));
    CHECK(dc_used_ms(&dc, 200 * 60000) == 0, "everything has left the window");

    /* Duplicate suppression. */
    dedup_t dd;
    dedup_init(&dd);
    CHECK(!dedup_check(&dd, a, 7, 0), "first sight is new");
    CHECK(dedup_check(&dd, a, 7, 1000), "second sight is a duplicate");
    CHECK(!dedup_check(&dd, b, 7, 1000), "same ID from another device is new");
    CHECK(!dedup_check(&dd, a, 7, 1000 + DEDUP_KEEP_MS + 1), "forgotten after DEDUP_KEEP_MS");
    for (int i = 0; i < DEDUP_N; i++) {
        dedup_check(&dd, a, (uint16_t)(1000 + i), 5000);
    }
    CHECK(!dedup_check(&dd, b, 7, 5000), "oldest entry dropped once the ring is full");

    /* Message IDs survive a reboot. */
    msgid_t m;
    msgid_init(&m, 5, NULL, NULL);
    CHECK(msgid_next(&m) == 5 && msgid_next(&m) == 6, "starts at the stored value");
    save_probe_t probe = {0, 0};
    msgid_init(&m, 100, probe_save, &probe);
    CHECK(probe.count == 1 && probe.value == 100 + MSGID_BLOCK, "first block reserved at boot (saved %u)", probe.value);
    uint16_t last = 0;
    for (int i = 0; i < MSGID_BLOCK + 10; i++) {
        last = msgid_next(&m);
    }
    CHECK(probe.count == 2 && probe.value == 100 + 2 * MSGID_BLOCK, "next block reserved when the first ran out");
    msgid_t reboot;
    msgid_init(&reboot, probe.value, NULL, NULL);
    uint16_t after = msgid_next(&reboot);
    CHECK((uint16_t)(after - last) > 0 && (uint16_t)(after - last) <= MSGID_BLOCK,
          "after a reboot IDs jump ahead (%u -> %u)", last, after);
    msgid_init(&m, 65535, NULL, NULL);
    CHECK(msgid_next(&m) == 65535 && msgid_next(&m) == 1, "0 is never handed out");
}

/* ========================================================================= */
/* Simulated radio channel                                                    */
/* ========================================================================= */

#define SIM_MAX 5
#define SIM_STEP_MS 5
#define AIR_MAX 64
#define LOG_MAX 512
#define ACT_MAX 32

typedef struct {
    link_evt_t e;
    int64_t t;
} logged_t;

typedef struct {
    link_t L;
    uint32_t addr;
    const char *name;
    bool txing;
    int air_idx;
    /* user policy */
    int accept_after_ms;  /* answer REQ_IN with A after this long (-1: never) */
    int decline_after_ms; /* ... or with N (-1: never) */
    bool ack_sos;         /* tap to acknowledge an SOS alarm */
    /* what happened */
    logged_t log[LOG_MAX];
    int n_log;
    uint16_t msgid_flash; /* "flash" for the message-ID block */
    int tx_count;
} sdev_t;

typedef struct {
    int from;
    int64_t start, end;
    uint8_t pkt[PROTO_MAX_PKT];
    int len;
    bool active; /* still in the air */
    bool used;   /* kept for collision checks until well after it ended */
} air_t;

typedef struct {
    int64_t at;
    int dev;
    int what; /* 1 accept, 2 decline, 3 sos ack */
    uint32_t addr;
} act_t;

/* A packet on its way through the phones and MQTT (phone.h, the relay). */
typedef struct {
    bool used;
    int to;      /* receiving device */
    int64_t at;  /* arrives then */
    uint8_t pkt[PROTO_MAX_PKT];
    int len;
} relay_t;

#define RELAY_N 64
#define SIM_RELAY_MS 400 /* phone -> broker -> phone: a typical delay */
#define SIM_DBM 17       /* the simulated devices' transmit power (the field data so far; the comments' dB sums) */

typedef struct sim_s sim_t;

struct sim_s {
    sdev_t d[SIM_MAX];
    int n;
    bool hear[SIM_MAX][SIM_MAX];
    int16_t rssi[SIM_MAX][SIM_MAX]; /* [from][to]: how strongly `to` hears `from`, dBm (default -90) */
    bool phone[SIM_MAX];            /* the device has its phone connected, relaying (phone.h) */
    relay_t relay[RELAY_N];         /* packets on their way through the phones */
    int relayed;                    /* how many went that way */
    double loss;
    lora_params_t lp; /* the radio setting all devices use (sim_set_sf) */
    int64_t now;
    air_t air[AIR_MAX];
    act_t act[ACT_MAX];
    int n_act;
    uint64_t rng;
    int collisions, losses, deliveries;
    bool random_policy; /* users answer requests at random (soak test) */
    void (*on_event)(sim_t *s, int dev, const link_evt_t *e); /* optional observer */
};

/* sim_rand() -- the channel's own random numbers (losses, user choices), 0..1. */
static double sim_rand(sim_t *s)
{
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 7;
    s->rng ^= s->rng << 17;
    return (double)(s->rng >> 11) / 9007199254740992.0;
}

/* sim_save_msgid() -- a device's "flash": where its next message-ID block starts. */
static void sim_save_msgid(void *ctx, uint16_t v)
{
    ((sdev_t *)ctx)->msgid_flash = v;
}

/* Create a world of n devices (IDs given), all in range of each other. */
static void sim_init(sim_t *s, int n, const char *const *ids, uint64_t seed)
{
    memset(s, 0, sizeof(*s));
    s->n = n;
    s->rng = seed ? seed : 1;
    s->lp = LP;
    s->now = 1000;
    for (int i = 0; i < n; i++) {
        sdev_t *d = &s->d[i];
        d->addr = id_to_addr(ids[i]);
        d->name = ids[i];
        d->accept_after_ms = -1;
        d->decline_after_ms = -1;
        d->msgid_flash = (uint16_t)(1000 * (i + 1));
        link_init(&d->L, d->addr, &LP, (uint32_t)(seed * 7919 + (uint64_t)i * 104729 + 1), d->msgid_flash, sim_save_msgid, d,
                  s->now);
        link_set_tx_dbm(&d->L, SIM_DBM);
        for (int j = 0; j < n; j++) {
            s->hear[i][j] = (i != j);
            s->rssi[i][j] = -90;
        }
    }
}

/* sim_set_sf() -- every device switches to spreading factor `sf` at once
 * (G on each device), and so does the simulated channel's airtime. */
static void sim_set_sf(sim_t *s, int sf)
{
    s->lp.sf = (uint8_t)sf;
    for (int i = 0; i < s->n; i++) {
        link_set_lora(&s->d[i].L, &s->lp, s->now);
    }
}

/* Simulate a reboot of device i: a fresh engine resuming from "flash". */
static void sim_reboot(sim_t *s, int i, uint32_t peer)
{
    sdev_t *d = &s->d[i];
    d->txing = false;
    for (int k = 0; k < AIR_MAX; k++) {
        if (s->air[k].used && s->air[k].active && s->air[k].from == i) {
            s->air[k].active = false; /* cut off mid-packet: nobody receives it */
            s->air[k].used = false;
        }
    }
    link_init(&d->L, d->addr, &s->lp, (uint32_t)(s->now * 31 + i), d->msgid_flash, sim_save_msgid, d, s->now);
    link_set_tx_dbm(&d->L, SIM_DBM);
    if (peer != ADDR_NONE) {
        link_set_peer(&d->L, peer, s->now);
    }
}

/* set_range() -- put devices i and j in or out of each other's range. */
static void set_range(sim_t *s, int i, int j, bool in_range)
{
    s->hear[i][j] = s->hear[j][i] = in_range;
}

/* Is a transmission audible at j overlapping [start, end] (other than k)? */
static bool sim_overlap(const sim_t *s, int j, int except, int64_t start, int64_t end)
{
    for (int k = 0; k < AIR_MAX; k++) {
        const air_t *a = &s->air[k];
        if (!a->used || k == except) {
            continue;
        }
        if (a->start < end && a->end > start && (a->from == j || s->hear[a->from][j])) {
            return true; /* someone else on air (or j itself transmitting) at the same time */
        }
    }
    return false;
}

/* sim_events() -- drain device i's events: log them (for the checks), print
 * them with DBG_LINK=1, and let its "user" react (accept / decline a request
 * after a pause, acknowledge an SOS). */
static void sim_events(sim_t *s, int i)
{
    sdev_t *d = &s->d[i];
    link_evt_t e;
    while (link_get_event(&d->L, &e)) {
        if (d->n_log < LOG_MAX) {
            d->log[d->n_log].e = e;
            d->log[d->n_log].t = s->now;
            d->n_log++;
        }
        if (getenv("DBG_LINK")) {
            char id[5] = "?";
            addr_to_id(e.addr, id);
            printf("    %7.1f s  %s: %-13s %s arg=%d %s\n", s->now / 1000.0, d->name, link_event_name(e.kind), id, e.arg,
                   e.kind == LEV_DISC_DONE ? "" : e.text); /* (a found list is not text) */
        }
        if (s->on_event) {
            s->on_event(s, i, &e);
        }
        int what = 0, delay = 0;
        if (e.kind == LEV_REQ_IN && s->random_policy) {
            double r = sim_rand(s);
            if (r < 0.65) {
                what = 1, delay = 1000 + (int)(sim_rand(s) * 19000); /* accept after 1-20 s */
            } else if (r < 0.85) {
                what = 2, delay = 1000 + (int)(sim_rand(s) * 9000); /* decline after 1-10 s */
            }                                                      /* else: ignore it */
        } else if (e.kind == LEV_REQ_IN && d->accept_after_ms >= 0) {
            what = 1, delay = d->accept_after_ms;
        } else if (e.kind == LEV_REQ_IN && d->decline_after_ms >= 0) {
            what = 2, delay = d->decline_after_ms;
        } else if (e.kind == LEV_SOS && d->ack_sos) {
            what = 3, delay = 2000;
        }
        if (what && s->n_act < ACT_MAX) {
            s->act[s->n_act++] = (act_t){s->now + delay, i, what, e.addr};
        }
    }
}

/* relay_send() -- device i's phone gets the packet it is transmitting: it
 * goes to the topic of each relay target (phone_relay_targets), and reaches
 * a target device SIM_RELAY_MS later if that device's phone is connected
 * too. (No loss on this path: it is TCP.) */
static void relay_send(sim_t *s, int i, const uint8_t *pkt, int len)
{
    if (!s->phone[i]) {
        return;
    }
    uint32_t to[PHONE_RELAY_MAX];
    int n = phone_relay_targets(pkt, len, s->d[i].addr, NULL, 0, s->d[i].L.peer, to, PHONE_RELAY_MAX);
    for (int t = 0; t < n; t++) {
        for (int j = 0; j < s->n; j++) {
            if (s->d[j].addr != to[t] || !s->phone[j]) {
                continue;
            }
            for (int k = 0; k < RELAY_N; k++) {
                if (!s->relay[k].used) {
                    s->relay[k] = (relay_t){.used = true, .to = j, .at = s->now + SIM_RELAY_MS, .len = len};
                    memcpy(s->relay[k].pkt, pkt, (size_t)len);
                    s->relayed++;
                    break;
                }
            }
        }
    }
}

/* Advance the world by one step. */
static void sim_step(sim_t *s)
{
    s->now += SIM_STEP_MS;
    /* 0. Packets arriving through the phones. */
    for (int k = 0; k < RELAY_N; k++) {
        relay_t *r = &s->relay[k];
        if (r->used && r->at <= s->now) {
            r->used = false;
            link_on_rx_via(&s->d[r->to].L, r->pkt, r->len, 0, 0, true, s->now);
        }
    }
    /* 1. Transmissions that have just finished reach whoever could hear them. */
    for (int k = 0; k < AIR_MAX; k++) {
        air_t *a = &s->air[k];
        if (!a->used || !a->active || a->end > s->now) {
            continue;
        }
        a->active = false;
        for (int j = 0; j < s->n; j++) {
            if (j == a->from || !s->hear[a->from][j]) {
                continue;
            }
            if (sim_overlap(s, j, k, a->start, a->end)) {
                s->collisions++;
                continue;
            }
            if (sim_rand(s) < s->loss) {
                s->losses++;
                continue;
            }
            s->deliveries++;
            link_on_rx(&s->d[j].L, a->pkt, a->len, s->rssi[a->from][j], 24, s->now);
        }
        s->d[a->from].txing = false;
        link_tx_done(&s->d[a->from].L, s->now, true);
    }
    /* Forget old transmissions (well past any packet they could overlap). */
    for (int k = 0; k < AIR_MAX; k++) {
        if (s->air[k].used && !s->air[k].active && s->now - s->air[k].end > 2000) {
            s->air[k].used = false;
        }
    }
    /* 2. The users' scheduled actions. */
    for (int k = 0; k < s->n_act;) {
        act_t *a = &s->act[k];
        if (a->at > s->now) {
            k++;
            continue;
        }
        link_t *L = &s->d[a->dev].L;
        if (a->what == 1) {
            link_accept(L, a->addr, s->now);
        } else if (a->what == 2) {
            link_decline(L, a->addr, s->now);
        } else if (a->what == 3) {
            link_send_sos_ack(L, a->addr, s->now);
        }
        s->act[k] = s->act[--s->n_act];
    }
    /* 3. Timers (and whether each device has its phone to relay: net.c
     * tells the engine the same, link_set_relay). */
    for (int i = 0; i < s->n; i++) {
        link_set_relay(&s->d[i].L, s->phone[i], s->now);
        link_poll(&s->d[i].L, s->now);
    }
    /* 4. Start transmissions, unless the device can hear someone already. */
    for (int i = 0; i < s->n; i++) {
        sdev_t *d = &s->d[i];
        if (d->txing) {
            continue;
        }
        bool busy = false;
        for (int k = 0; k < AIR_MAX; k++) {
            if (s->air[k].used && s->air[k].active && s->hear[s->air[k].from][i]) {
                busy = true;
            }
        }
        if (busy) {
            continue;
        }
        uint8_t pkt[PROTO_MAX_PKT];
        int len;
        bool lp;
        if (!link_take_tx(&d->L, s->now, pkt, &len, &lp)) {
            continue;
        }
        int k = 0;
        while (k < AIR_MAX && s->air[k].used) {
            k++;
        }
        if (k == AIR_MAX) {
            link_tx_done(&d->L, s->now, false);
            continue;
        }
        air_t *a = &s->air[k];
        a->used = a->active = true;
        a->from = i;
        a->start = s->now;
        a->end = s->now + (int64_t)(lora_airtime_us(len, &s->lp, lp ? RADIO_PREAMBLE_LONG : RADIO_PREAMBLE_SHORT) + 999) / 1000;
        memcpy(a->pkt, pkt, (size_t)len);
        a->len = len;
        d->txing = true;
        d->tx_count++;
        relay_send(s, i, pkt, len); /* and a copy through the phone, if one is connected */
    }
    /* 5. Events. */
    for (int i = 0; i < s->n; i++) {
        sim_events(s, i);
    }
}

/* sim_run() -- advance the world by `ms`. */
static void sim_run(sim_t *s, int64_t ms)
{
    int64_t end = s->now + ms;
    while (s->now < end) {
        sim_step(s);
    }
}

/* Run until device i logs an event of `kind` (any address if addr == ANY),
 * or `ms` pass. Returns the time it took, or -1. */
#define ANY 0xFFFFFFFFu
static int64_t sim_until(sim_t *s, int i, uint8_t kind, uint32_t addr, int64_t ms)
{
    int start_log = s->d[i].n_log;
    int64_t t0 = s->now, end = s->now + ms;
    while (s->now < end) {
        sim_step(s);
        for (int k = start_log; k < s->d[i].n_log; k++) {
            if (s->d[i].log[k].e.kind == kind && (addr == ANY || s->d[i].log[k].e.addr == addr)) {
                return s->now - t0;
            }
        }
    }
    return -1;
}

/* How many events of `kind` (from addr, with arg; ANY / -1 = any) device i logged since index `from`. */
static int count_ev(const sim_t *s, int i, int from, uint8_t kind, uint32_t addr, int arg)
{
    int n = 0;
    for (int k = from; k < s->d[i].n_log; k++) {
        const link_evt_t *e = &s->d[i].log[k].e;
        if (e->kind == kind && (addr == ANY || e->addr == addr) && (arg < 0 || e->arg == arg)) {
            n++;
        }
    }
    return n;
}

/* last_ev() -- device i's most recent event of `kind`, or NULL. */
static const logged_t *last_ev(const sim_t *s, int i, uint8_t kind)
{
    for (int k = s->d[i].n_log - 1; k >= 0; k--) {
        if (s->d[i].log[k].e.kind == kind) {
            return &s->d[i].log[k];
        }
    }
    return NULL;
}

/* path_now() -- device i's view of `path` right now. */
static uint8_t path_now(const sim_t *s, int i, uint8_t path)
{
    return link_path_state(&s->d[i].L, path, s->now);
}

/* count_path() -- LEV_PATH events of device i since `from` saying `path`
 * went to `state` (-1: any). */
static int count_path(const sim_t *s, int i, int from, uint8_t path, int state)
{
    int n = 0;
    for (int k = from; k < s->d[i].n_log; k++) {
        const link_evt_t *e = &s->d[i].log[k].e;
        if (e->kind == LEV_PATH && LINK_EV_PATH(e->arg) == path && (state < 0 || LINK_EV_STATE(e->arg) == state)) {
            n++;
        }
    }
    return n;
}

/* Connect a and b (a asks, b accepts after 1.5 s). */
static bool sim_connect(sim_t *s, int a, int b)
{
    int keep = s->d[b].accept_after_ms;
    s->d[b].accept_after_ms = 1500;
    link_request(&s->d[a].L, s->d[b].addr, s->now);
    int64_t t = sim_until(s, a, LEV_REQ_RESULT, s->d[b].addr, 20000);
    sim_run(s, 2000); /* let the confirm land */
    s->d[b].accept_after_ms = keep;
    return t >= 0 && s->d[a].L.peer == s->d[b].addr && s->d[b].L.peer == s->d[a].addr;
}

/* ========================================================================= */
/* Scenarios                                                                  */
/* ========================================================================= */

static const char *const IDS[] = {"K7Q", "ABC", "XYZ", "MNP", "2345"};
enum { A = 0, B, C, D, E };

/* A asks B, B's user accepts; messages both ways with receipts; three
 * queued messages arrive in order; then an hour of keepalives. */
static void test_connect_and_chat(void)
{
    printf("link: connect, chat, keepalives\n");
    sim_t s;
    sim_init(&s, 2, IDS, 1);
    s.d[B].accept_after_ms = 3000;
    link_request(&s.d[A].L, s.d[B].addr, s.now);
    int64_t t_in = sim_until(&s, B, LEV_REQ_IN, s.d[A].addr, 5000);
    CHECK(t_in >= 0 && t_in < 1000, "B is asked within a second (%lld ms)", (long long)t_in);
    sim_run(&s, 600); /* B's RECEIVED goes on air */
    CHECK(s.d[A].L.req_phase == REQ_WAITING, "A heard RECEIVED and stopped retrying (phase %s)",
          link_req_phase_name(s.d[A].L.req_phase));
    int64_t t_acc = sim_until(&s, A, LEV_REQ_RESULT, s.d[B].addr, 10000);
    CHECK(t_acc >= 0, "A gets an answer");
    const logged_t *r = last_ev(&s, A, LEV_REQ_RESULT);
    CHECK(r && r->e.arg == LRES_ACCEPTED, "... accepted");
    sim_run(&s, 1000);
    CHECK(count_ev(&s, B, 0, LEV_ACCEPT_RESULT, s.d[A].addr, 1) == 1, "B's accept confirmed");
    CHECK(s.d[A].L.peer == s.d[B].addr && s.d[B].L.peer == s.d[A].addr && s.d[A].L.state == LS_UP &&
              s.d[B].L.state == LS_UP,
          "both linked");

    /* Messages both ways, each confirmed by its silent receipt. */
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "HELLO", s.now);
    int64_t t_msg = sim_until(&s, A, LEV_MSG_SENT, s.d[B].addr, 5000);
    CHECK(t_msg >= 0 && t_msg < 1500, "receipt back within %lld ms", (long long)t_msg);
    const logged_t *m = last_ev(&s, B, LEV_MSG);
    CHECK(m && strcmp(m->e.text, "HELLO") == 0 && m->e.addr == s.d[A].addr, "B got HELLO");
    link_send_msg(&s.d[B].L, s.d[B].L.peer, "HI THERE", s.now);
    CHECK(sim_until(&s, B, LEV_MSG_SENT, s.d[A].addr, 5000) >= 0, "B's reply confirmed");
    m = last_ev(&s, A, LEV_MSG);
    CHECK(m && strcmp(m->e.text, "HI THERE") == 0, "A got the reply");
    CHECK(link_busy(&s.d[A].L, s.now) && link_busy(&s.d[B].L, s.now), "both busy after messages");

    /* Three messages queued at once arrive in order. */
    int base = s.d[B].n_log;
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "ONE", s.now);
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "TWO", s.now);
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "THREE", s.now);
    sim_run(&s, 8000);
    CHECK(count_ev(&s, A, 0, LEV_MSG_SENT, ANY, -1) == 4, "all receipted (%d)", count_ev(&s, A, 0, LEV_MSG_SENT, ANY, -1));
    int k = base, order_ok = 0;
    const char *want[] = {"ONE", "TWO", "THREE"};
    for (int w = 0; w < 3 && k < s.d[B].n_log; k++) {
        if (s.d[B].log[k].e.kind == LEV_MSG && strcmp(s.d[B].log[k].e.text, want[w]) == 0) {
            w++, order_ok++;
        }
    }
    CHECK(order_ok == 3, "in order (%d)", order_ok);

    /* An hour of keepalives: never lost, and well inside the duty cycle. */
    int tx0 = s.d[A].tx_count;
    sim_run(&s, 60 * 60 * 1000);
    CHECK(count_ev(&s, A, 0, LEV_LINK_LOST, ANY, -1) == 0 && count_ev(&s, B, 0, LEV_LINK_LOST, ANY, -1) == 0,
          "no false 'connection lost' in an hour");
    int per_hour = s.d[A].tx_count - tx0;
    CHECK(per_hour >= 110 && per_hour <= 145, "A sent %d keepalives in an hour (expected ~126)", per_hour);
    uint32_t used = link_duty_used_ms(&s.d[A].L, s.now);
    printf("  keepalive cost: %d packets/hour, %.2f %% duty cycle\n", per_hour, used / 36000.0);
    CHECK(used < 36000, "under 1 %% duty cycle (%u ms)", used);
    CHECK(s.collisions < 5, "keepalives rarely collide (%d)", s.collisions);
}

/* Out of range: both notice within 90 s; sending is refused; back in range:
 * restored within a keepalive period. */
static void test_lost_restored(void)
{
    printf("link: connection lost and restored\n");
    sim_t s;
    sim_init(&s, 2, IDS, 2);
    CHECK(sim_connect(&s, A, B), "connected");
    sim_run(&s, 60000);
    set_range(&s, A, B, false);
    int64_t tl = sim_until(&s, A, LEV_LINK_LOST, s.d[B].addr, 200000);
    /* Lost 90 s after the last proof in either direction: the older of "A
     * last heard B" and "B last heard A" (B's report). Both were at most a
     * keepalive period (~30 s) old when B walked away, so 60-90 s after it,
     * or a little earlier when a keepalive collided just before. */
    CHECK(tl >= 50000 && tl <= 95000, "A notices after %lld ms (90 s after the last proof both ways)", (long long)tl);
    const logged_t *lw = last_ev(&s, A, LEV_LINK_LOST);
    CHECK(lw && lw->e.arg == LOST_SILENCE, "reason: silence (%s)", lw ? link_lost_why_name(lw->e.arg) : "-");
    sim_run(&s, 35000);
    CHECK(s.d[B].L.state == LS_LOST, "B notices too");
    CHECK(link_path_state(&s.d[A].L, LP_LORA, s.now) == PS_LOST && link_path_state(&s.d[A].L, LP_RELAY, s.now) == PS_OFF,
          "A: LoRa lost, relay off (no phone)");
    /* Sending while lost is refused immediately (the draft stays with the user). */
    int f0 = count_ev(&s, A, 0, LEV_MSG_FAILED, ANY, -1);
    CHECK(!link_send_msg(&s.d[A].L, s.d[A].L.peer, "ANYONE", s.now), "send refused while lost");
    sim_run(&s, 10);
    const logged_t *f = last_ev(&s, A, LEV_MSG_FAILED);
    CHECK(count_ev(&s, A, 0, LEV_MSG_FAILED, ANY, -1) == f0 + 1 && f && strcmp(f->e.text, "ANYONE") == 0,
          "failure carries the text back");
    CHECK(f && f->e.arg == 1, "marked as only the refused message (arg %d)", f ? f->e.arg : -1);
    sim_run(&s, 120000); /* still apart: keepalives keep trying */
    set_range(&s, A, B, true);
    int64_t tr = sim_until(&s, A, LEV_LINK_RESTORED, s.d[B].addr, 60000);
    /* The first keepalive heard (from either side, within a period) proves
     * one way; an extra keepalive each way (ka_soon) proves the other
     * within a few seconds. */
    CHECK(tr >= 0 && tr <= 35000, "restored within a keepalive period and a few seconds (%lld ms)", (long long)tr);
    sim_run(&s, 31000);
    CHECK(s.d[A].L.state == LS_UP && s.d[B].L.state == LS_UP, "both up again");
    CHECK(count_ev(&s, A, 0, LEV_LINK_LOST, ANY, -1) == 1 && count_ev(&s, B, 0, LEV_LINK_LOST, ANY, -1) == 1,
          "lost reported once each");
}

/* No answer (out of range): unavailable after ~10 s; N: declined; a blocked
 * requester just sees unavailable. */
static void test_unavailable_declined(void)
{
    printf("link: unavailable, declined, out-of-range ID\n");
    sim_t s;
    sim_init(&s, 3, IDS, 3);
    set_range(&s, A, C, false);
    link_request(&s.d[A].L, s.d[C].addr, s.now);
    int64_t t = sim_until(&s, A, LEV_REQ_RESULT, s.d[C].addr, 30000);
    const logged_t *r = last_ev(&s, A, LEV_REQ_RESULT);
    CHECK(t >= 8000 && t <= 14000 && r && r->e.arg == LRES_UNAVAILABLE, "unavailable after %lld ms (~10 s)",
          (long long)t);
    CHECK(s.d[A].tx_count == LINK_TRIES, "%d copies sent", s.d[A].tx_count);

    s.d[B].decline_after_ms = 2000;
    link_request(&s.d[A].L, s.d[B].addr, s.now);
    t = sim_until(&s, A, LEV_REQ_RESULT, s.d[B].addr, 20000);
    r = last_ev(&s, A, LEV_REQ_RESULT);
    CHECK(t >= 2000 && r && r->e.arg == LRES_DECLINED, "declined after B's user said N (%lld ms)", (long long)t);
    sim_run(&s, 10000);
    CHECK(s.d[A].L.peer == ADDR_NONE && s.d[B].L.peer == ADDR_NONE, "nobody linked");
    CHECK(count_ev(&s, A, 0, LEV_REQ_RESULT, ANY, -1) == 2, "one result per request");

    /* Blocked: a request from a blocked device is ignored -> unavailable. */
    uint32_t bl = s.d[A].addr;
    link_set_blocklist(&s.d[B].L, &bl, 1);
    int n0 = count_ev(&s, B, 0, LEV_REQ_IN, ANY, -1);
    link_request(&s.d[A].L, s.d[B].addr, s.now);
    sim_until(&s, A, LEV_REQ_RESULT, s.d[B].addr, 30000);
    r = last_ev(&s, A, LEV_REQ_RESULT);
    CHECK(r && r->e.arg == LRES_UNAVAILABLE && count_ev(&s, B, 0, LEV_REQ_IN, ANY, -1) == n0,
          "blocked requester sees 'unavailable'; B is not disturbed");
}

/* B is busy with A: C hears BUSY; the request plays 10 minutes after B's
 * last message; B accepts; C connects late; A is told the link ended. */
static void test_busy_queue(void)
{
    printf("link: busy, queued request, late accept, old partner told\n");
    sim_t s;
    sim_init(&s, 3, IDS, 4);
    CHECK(sim_connect(&s, A, B), "A-B connected");
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "WHERE ARE YOU", s.now);
    sim_run(&s, 3000);
    CHECK(link_busy(&s.d[B].L, s.now), "B is busy");
    s.d[B].accept_after_ms = 5000; /* B's user will accept once told */
    int b_log = s.d[B].n_log;      /* B's events from here on */
    link_request(&s.d[C].L, s.d[B].addr, s.now);
    int64_t t = sim_until(&s, C, LEV_REQ_RESULT, s.d[B].addr, 20000);
    const logged_t *r = last_ev(&s, C, LEV_REQ_RESULT);
    CHECK(t >= 0 && r && r->e.arg == LRES_BUSY, "C hears BUSY");
    CHECK(count_ev(&s, B, b_log, LEV_REQ_IN, ANY, -1) == 0, "B's user not disturbed yet");
    CHECK(s.d[C].L.req_phase == REQ_QUEUED, "C's request stays open");
    /* B and A keep chatting for a while; the request waits. */
    sim_run(&s, 4 * 60000);
    link_send_msg(&s.d[B].L, s.d[B].L.peer, "ON MY WAY", s.now);
    int64_t t_msg = s.now;
    sim_run(&s, 5 * 60000);
    CHECK(count_ev(&s, B, b_log, LEV_REQ_IN, ANY, -1) == 0, "still queued 5 min after the last message");
    int64_t t_in = sim_until(&s, B, LEV_REQ_IN, s.d[C].addr, 10 * 60000);
    CHECK(t_in >= 0, "played once B is free");
    int64_t quiet = s.now - t_msg;
    CHECK(quiet >= LINK_BUSY_MS && quiet < LINK_BUSY_MS + 5000, "... 10 minutes after the last message (%lld s)",
          (long long)(quiet / 1000));
    t = sim_until(&s, C, LEV_REQ_RESULT, s.d[B].addr, 20000);
    r = last_ev(&s, C, LEV_REQ_RESULT);
    CHECK(t >= 0 && r && r->e.arg == LRES_ACCEPTED, "C connected by the late accept");
    sim_run(&s, 6000);
    CHECK(s.d[B].L.peer == s.d[C].addr && s.d[C].L.peer == s.d[B].addr, "B-C linked");
    CHECK(count_ev(&s, A, 0, LEV_LINK_ENDED, s.d[B].addr, -1) == 1, "A told the link ended");
    CHECK(s.d[A].L.state == LS_ENDED, "A's link is ended (sending refused)");
    sim_run(&s, 5 * 60000);
    CHECK(count_ev(&s, A, 0, LEV_LINK_LOST, ANY, -1) == 0 && count_ev(&s, A, 0, LEV_LINK_ENDED, ANY, -1) == 1,
          "nothing more for A");
}

/* A's queued request is accepted after A has connected elsewhere: A says
 * ENDED, B's accept fails at once, nobody's link moves. */
static void test_late_accept_unwanted(void)
{
    printf("link: a late accept after the requester moved on\n");
    sim_t s;
    sim_init(&s, 4, IDS, 5);
    CHECK(sim_connect(&s, B, C), "B-C connected");
    link_send_msg(&s.d[B].L, s.d[B].L.peer, "HI", s.now);
    sim_run(&s, 3000);
    link_request(&s.d[A].L, s.d[B].addr, s.now); /* B busy -> queued */
    sim_until(&s, A, LEV_REQ_RESULT, s.d[B].addr, 20000);
    CHECK(s.d[A].L.req_phase == REQ_QUEUED, "queued at B");
    CHECK(sim_connect(&s, A, D), "A connects to D meanwhile");
    s.d[B].accept_after_ms = 1000;
    sim_until(&s, B, LEV_REQ_IN, s.d[A].addr, 15 * 60000); /* B free: B's user is told, accepts */
    int64_t t = sim_until(&s, B, LEV_ACCEPT_RESULT, s.d[A].addr, 20000);
    const logged_t *r = last_ev(&s, B, LEV_ACCEPT_RESULT);
    CHECK(t >= 0 && t < 3000 && r && r->e.arg == 0, "B's accept answered 'not wanted' at once (%lld ms)", (long long)t);
    sim_run(&s, 5000);
    CHECK(s.d[A].L.peer == s.d[D].addr && s.d[B].L.peer == s.d[C].addr, "nobody's link moved");
    CHECK(count_ev(&s, C, 0, LEV_LINK_ENDED, ANY, -1) == 0, "C undisturbed");
}

/* B walks away unnoticed: A's two messages fail together after the retries,
 * texts returned in order, link counted as lost (no second alert). */
static void test_message_failure(void)
{
    printf("link: message without a receipt\n");
    sim_t s;
    sim_init(&s, 2, IDS, 6);
    CHECK(sim_connect(&s, A, B), "connected");
    sim_run(&s, 20000);
    set_range(&s, A, B, false); /* B walks away; A doesn't know yet */
    sim_run(&s, 5000);
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "ARE YOU THERE", s.now);
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "HELLO", s.now);
    int64_t t = sim_until(&s, A, LEV_MSG_FAILED, s.d[B].addr, 40000);
    const logged_t *f = last_ev(&s, A, LEV_MSG_FAILED);
    CHECK(t >= 8000 && t <= 16000, "failure reported after %lld ms", (long long)t);
    CHECK(f && strcmp(f->e.text, "ARE YOU THERE HELLO") == 0, "both texts returned, in order: '%s'", f ? f->e.text : "");
    CHECK(f && f->e.arg == 0, "marked as every waiting message (arg %d)", f ? f->e.arg : -1);
    CHECK(count_ev(&s, A, 0, LEV_MSG_FAILED, ANY, -1) == 1, "one failure for the user to hear");
    CHECK(s.d[A].L.state == LS_LOST, "link counts as lost");
    /* Since v0.6.0 the loss is an event of its own, right after the failure
     * (the UI plays no second alert for it: ui.c); up to v0.5.1 it was
     * silent, and a long silence afterwards was then never reported. */
    const logged_t *l = last_ev(&s, A, LEV_LINK_LOST);
    CHECK(count_ev(&s, A, 0, LEV_LINK_LOST, ANY, -1) == 1 && l && l->e.arg == LOST_NO_RECEIPT && l->t == f->t,
          "one 'lost' event, reason no-receipt, at the failure (%s)", l ? link_lost_why_name(l->e.arg) : "-");
    set_range(&s, A, B, true);
    CHECK(sim_until(&s, A, LEV_LINK_RESTORED, s.d[B].addr, 40000) >= 0, "restored when back in range");
}

/* 25 % loss: every message gets exactly one result, nothing is delivered
 * twice, and every receipted message really arrived. */
static void test_lossy_channel(void)
{
    printf("link: 25 %% packet loss, messages exactly once\n");
    sim_t s;
    sim_init(&s, 2, IDS, 7);
    CHECK(sim_connect(&s, A, B), "connected");
    s.loss = 0.25;
    char text[32];
    int sent = 0, failed = 0, delivered = 0, dupes = 0;
    for (int i = 0; i < 40; i++) {
        snprintf(text, sizeof text, "MSG %d", i);
        link_send_msg(&s.d[A].L, s.d[A].L.peer, text, s.now);
        sim_run(&s, 20000);
        /* the link may have been declared lost after a failure; carry on */
        if (s.d[A].L.state != LS_UP) {
            sim_until(&s, A, LEV_LINK_RESTORED, ANY, 60000);
        }
    }
    sent = count_ev(&s, A, 0, LEV_MSG_SENT, ANY, -1);
    failed = count_ev(&s, A, 0, LEV_MSG_FAILED, ANY, -1);
    for (int i = 0; i < 40; i++) {
        snprintf(text, sizeof text, "MSG %d", i);
        int n = 0;
        for (int k = 0; k < s.d[B].n_log; k++) {
            if (s.d[B].log[k].e.kind == LEV_MSG && strcmp(s.d[B].log[k].e.text, text) == 0) {
                n++;
            }
        }
        delivered += n > 0;
        dupes += n > 1;
    }
    printf("  40 messages: %d receipted, %d failed, %d delivered, %d duplicates (losses %d)\n", sent, failed, delivered,
           dupes, s.losses);
    CHECK(dupes == 0, "no message delivered twice");
    CHECK(sent + failed == 40, "every message gets exactly one result");
    CHECK(delivered >= sent, "every receipted message really arrived");
    CHECK(failed <= 4, "few failures at 25 %% loss (%d)", failed);
}

/* B restarts mid-conversation: its new messages are not mistaken for
 * duplicates (message IDs resume from flash), and the link survives. */
static void test_reboot_msgids(void)
{
    printf("link: message IDs after a reboot\n");
    sim_t s;
    sim_init(&s, 2, IDS, 8);
    CHECK(sim_connect(&s, A, B), "connected");
    link_send_msg(&s.d[B].L, s.d[B].L.peer, "BEFORE", s.now);
    sim_run(&s, 3000);
    sim_reboot(&s, B, s.d[A].addr);
    sim_run(&s, 6000);
    link_send_msg(&s.d[B].L, s.d[B].L.peer, "AFTER", s.now);
    CHECK(sim_until(&s, B, LEV_MSG_SENT, ANY, 15000) >= 0, "message after the reboot receipted");
    const logged_t *m = last_ev(&s, A, LEV_MSG);
    CHECK(m && strcmp(m->e.text, "AFTER") == 0, "... and delivered, not dropped as a duplicate");
    CHECK(count_ev(&s, A, 0, LEV_LINK_LOST, ANY, -1) == 0, "the link survived the reboot");
}

/* Both ask at once: both connect. Then A moves to C: B is told, falls
 * silent, and a message A's user heard announced for B can't reach C. */
static void test_mutual_and_switch(void)
{
    printf("link: both ask at once; switching partner tells the old one\n");
    sim_t s;
    sim_init(&s, 3, IDS, 9);
    link_request(&s.d[A].L, s.d[B].addr, s.now);
    link_request(&s.d[B].L, s.d[A].addr, s.now);
    sim_run(&s, 15000);
    const logged_t *ra = last_ev(&s, A, LEV_REQ_RESULT), *rb = last_ev(&s, B, LEV_REQ_RESULT);
    CHECK(ra && ra->e.arg == LRES_ACCEPTED && rb && rb->e.arg == LRES_ACCEPTED, "both hear OK");
    CHECK(s.d[A].L.peer == s.d[B].addr && s.d[B].L.peer == s.d[A].addr, "linked");
    CHECK(count_ev(&s, A, 0, LEV_REQ_IN, ANY, -1) + count_ev(&s, B, 0, LEV_REQ_IN, ANY, -1) <= 1,
          "nobody left with a request to answer that no longer applies");
    sim_run(&s, 5000);
    /* A picks C; C accepts; B is told. */
    CHECK(sim_connect(&s, A, C), "A switches to C");
    sim_run(&s, 8000);
    CHECK(count_ev(&s, B, 0, LEV_LINK_ENDED, s.d[A].addr, -1) == 1, "B told the link ended");
    /* B's keepalives don't resurrect anything: B stops sending them. */
    int tx0 = s.d[B].tx_count;
    sim_run(&s, 5 * 60000);
    CHECK(s.d[B].tx_count - tx0 == 0, "B quiet after the end (%d packets)", s.d[B].tx_count - tx0);
    /* A message announced for B (A's user heard "ABC") must not go to C. */
    int c_msgs = count_ev(&s, C, 0, LEV_MSG, ANY, -1);
    CHECK(!link_send_msg(&s.d[A].L, s.d[B].addr, "FOR ABC", s.now), "refused: the link is now with C");
    sim_run(&s, 5000);
    CHECK(count_ev(&s, C, 0, LEV_MSG, ANY, -1) == c_msgs, "C never got it");
}

/* B misses A's ENDED notice (out of range): its next keepalive is answered
 * ENDED, so B finds out. */
static void test_missed_end_notice(void)
{
    printf("link: a former partner who missed the 'ended' notice\n");
    sim_t s;
    sim_init(&s, 3, IDS, 10);
    CHECK(sim_connect(&s, A, B), "A-B connected");
    set_range(&s, A, B, false); /* B can't hear A's notices */
    CHECK(sim_connect(&s, A, C), "A moves to C while B is out of range");
    sim_run(&s, 10000);
    CHECK(s.d[B].L.peer == s.d[A].addr, "B still thinks it is linked to A");
    set_range(&s, A, B, true);
    int64_t t = sim_until(&s, B, LEV_LINK_ENDED, s.d[A].addr, 120000);
    CHECK(t >= 0, "B learns at its next keepalive (%lld ms)", (long long)t);
}

/* One-way radio path: A hears B's ACCEPT and links, B never gets the
 * confirm and gives up; A's next keepalive is answered ENDED: both agree. */
static void test_accept_confirm_lost(void)
{
    printf("link: accept whose confirm never arrives\n");
    sim_t s;
    sim_init(&s, 2, IDS, 11);
    s.d[B].accept_after_ms = 1000;
    link_request(&s.d[A].L, s.d[B].addr, s.now);
    sim_until(&s, B, LEV_REQ_IN, ANY, 5000);
    s.hear[A][B] = false; /* one-way: B hears nothing from A, A still hears B */
    int64_t t = sim_until(&s, B, LEV_ACCEPT_RESULT, s.d[A].addr, 30000);
    const logged_t *r = last_ev(&s, B, LEV_ACCEPT_RESULT);
    CHECK(t >= 0 && r && r->e.arg == 0, "B's accept fails");
    CHECK(s.d[A].L.peer == s.d[B].addr, "A heard the ACCEPT and linked");
    CHECK(s.d[B].L.peer == ADDR_NONE, "B did not move its link without a confirm");
    s.hear[A][B] = true;
    t = sim_until(&s, A, LEV_LINK_ENDED, s.d[B].addr, 120000);
    CHECK(t >= 0, "A told 'ended' at its next keepalive: both agree again");
}

/* Three requests wait at most; a fourth is ignored; unanswered ones expire
 * 15 min after being played; any waiting request can be accepted. */
static void test_pending_limits(void)
{
    printf("link: request queue limits and expiry\n");
    sim_t s;
    sim_init(&s, 5, IDS, 12);
    for (int i = B; i <= E; i++) {
        link_request(&s.d[i].L, s.d[A].addr, s.now);
        sim_run(&s, 1500);
    }
    sim_run(&s, 15000);
    CHECK(s.d[A].L.n_pend == LINK_PENDING_MAX, "%d waiting", s.d[A].L.n_pend);
    const logged_t *r = last_ev(&s, E, LEV_REQ_RESULT);
    CHECK(r && r->e.arg == LRES_UNAVAILABLE, "the fourth requester is ignored");
    CHECK(count_ev(&s, A, 0, LEV_REQ_IN, ANY, -1) == 3, "A's user told about three");
    int64_t t = sim_until(&s, A, LEV_REQ_GONE, ANY, LINK_PENDING_MS + 60000);
    const logged_t *in = NULL, *gone = last_ev(&s, A, LEV_REQ_GONE);
    for (int k = 0; k < s.d[A].n_log && !in; k++) {
        if (s.d[A].log[k].e.kind == LEV_REQ_IN && gone && s.d[A].log[k].e.addr == gone->e.addr) {
            in = &s.d[A].log[k];
        }
    }
    CHECK(t >= 0 && in && gone->t - in->t >= LINK_PENDING_MS && gone->t - in->t < LINK_PENDING_MS + 1000,
          "an unanswered request expires 15 min after it was played (%lld s)", in && gone ? (long long)((gone->t - in->t) / 1000) : -1LL);
    sim_run(&s, 10000);
    CHECK(s.d[A].L.n_pend == 0, "queue empty");
    /* Accept by address works for any of them, in any order. */
    for (int i = B; i <= D; i++) {
        link_request(&s.d[i].L, s.d[A].addr, s.now);
        sim_run(&s, 1500);
    }
    sim_run(&s, 3000);
    link_accept(&s.d[A].L, s.d[C].addr, s.now);
    sim_run(&s, 5000);
    CHECK(s.d[A].L.peer == s.d[C].addr && s.d[C].L.peer == s.d[A].addr, "accepted the second one");
    CHECK(s.d[A].L.n_pend == 2, "the others still wait");
}

/* X: the other side is told once (despite two copies) and both unlink. */
static void test_unpair(void)
{
    printf("link: UNPAIR\n");
    sim_t s;
    sim_init(&s, 2, IDS, 13);
    CHECK(sim_connect(&s, A, B), "connected");
    link_unpair(&s.d[A].L, s.d[B].addr, s.now);
    int64_t t = sim_until(&s, B, LEV_UNPAIRED, s.d[A].addr, 10000);
    const logged_t *u = last_ev(&s, B, LEV_UNPAIRED);
    CHECK(t >= 0 && u && u->e.arg == 1, "B told, and A was its link");
    CHECK(s.d[A].L.peer == ADDR_NONE && s.d[B].L.peer == ADDR_NONE, "both unlinked");
    sim_run(&s, 10000);
    CHECK(count_ev(&s, B, 0, LEV_UNPAIRED, ANY, -1) == 1, "once, despite two copies");
}

/* A - B - C in a line: A's SOS reaches C through B's relay, C's ACK comes
 * back the same way, each only once; resends, cancel, targeted SOS and the
 * blocklist exemption. */
static void test_sos_relay(void)
{
    printf("link: SOS relayed one hop, acknowledged and cancelled\n");
    sim_t s;
    sim_init(&s, 3, IDS, 14);
    set_range(&s, A, C, false); /* A - B - C in a line */
    s.d[B].ack_sos = false;
    s.d[C].ack_sos = true;
    link_send_sos(&s.d[A].L, ADDR_BROADCAST, s.now);
    int64_t tc = sim_until(&s, C, LEV_SOS, s.d[A].addr, 10000);
    CHECK(tc >= 0, "C, out of A's range, gets the SOS through B (%lld ms)", (long long)tc);
    sim_run(&s, 8000);
    CHECK(count_ev(&s, B, 0, LEV_SOS, ANY, -1) == 1 && count_ev(&s, C, 0, LEV_SOS, ANY, -1) == 1,
          "each alarm raised once despite copies and relays");
    CHECK(count_ev(&s, A, 0, LEV_SOS_ACK, s.d[C].addr, -1) == 1, "C's acknowledgement reaches A through B");
    /* A resend (a new SOS) is delivered again: the UI re-acknowledges silently. */
    link_send_sos(&s.d[A].L, ADDR_BROADCAST, s.now);
    sim_run(&s, 8000);
    CHECK(count_ev(&s, C, 0, LEV_SOS, ANY, -1) == 2, "resend arrives as a new SOS");
    link_send_sos_cancel(&s.d[A].L, ADDR_BROADCAST, s.now);
    CHECK(sim_until(&s, C, LEV_SOS_CANCEL, s.d[A].addr, 15000) >= 0, "the cancel reaches C too");
    sim_run(&s, 10000);
    CHECK(count_ev(&s, C, 0, LEV_SOS_CANCEL, ANY, -1) == 1, "once");
    /* A targeted SOS for C is relayed by B but only alarms C. */
    int b0 = count_ev(&s, B, 0, LEV_SOS, ANY, -1);
    link_send_sos(&s.d[A].L, s.d[C].addr, s.now);
    int64_t t2 = sim_until(&s, C, LEV_SOS, s.d[A].addr, 10000);
    const logged_t *e = last_ev(&s, C, LEV_SOS);
    CHECK(t2 >= 0 && e && e->e.arg == 1, "targeted SOS relayed to C, marked as for C");
    CHECK(count_ev(&s, B, 0, LEV_SOS, ANY, -1) == b0, "B relays it but no alarm on B");
    /* SOS ignores the blocklist. */
    uint32_t bl = s.d[A].addr;
    link_set_blocklist(&s.d[B].L, &bl, 1);
    link_send_sos(&s.d[A].L, ADDR_BROADCAST, s.now);
    CHECK(sim_until(&s, B, LEV_SOS, s.d[A].addr, 10000) >= 0, "SOS from a blocked device still arrives");
}

/* With the hour's budget used up, a message isn't transmitted (it fails),
 * but SOS still goes out. */
static void test_duty_cycle(void)
{
    printf("link: duty-cycle limit (SOS exempt)\n");
    sim_t s;
    sim_init(&s, 2, IDS, 15);
    CHECK(sim_connect(&s, A, B), "connected");
    dc_add(&s.d[A].L.dc, s.now, dc_budget_ms() - 50); /* almost the whole hour's budget used */
    int tx0 = s.d[A].tx_count;
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "TOO MUCH", s.now);
    int64_t t = sim_until(&s, A, LEV_MSG_FAILED, ANY, 60000);
    CHECK(t >= 0 && s.d[A].tx_count == tx0, "message not transmitted, reported failed (%lld ms)", (long long)t);
    link_send_sos(&s.d[A].L, ADDR_BROADCAST, s.now);
    CHECK(sim_until(&s, B, LEV_SOS, ANY, 10000) >= 0, "SOS still goes out");
}

/* PING: every device answers, reporting how it heard us. */
static void test_ping(void)
{
    printf("link: ping / pong\n");
    sim_t s;
    sim_init(&s, 3, IDS, 16);
    link_ping(&s.d[A].L, s.now);
    sim_run(&s, 4000);
    CHECK(count_ev(&s, A, 0, LEV_PONG, ANY, -1) == 2, "both devices answered (%d)", count_ev(&s, A, 0, LEV_PONG, ANY, -1));
    const logged_t *p = last_ev(&s, A, LEV_PONG);
    CHECK(p && p->e.far_rssi == -90 && p->e.far_dbm == SIM_DBM, "pong reports how they heard us");
}

/* ------------------------------------------------------------------------- */
/* Discovery (D)                                                              */
/* ------------------------------------------------------------------------- */

/* set_rssi() -- how strongly devices i and j hear each other (both ways), dBm. */
static void set_rssi(sim_t *s, int i, int j, int dbm)
{
    s->rssi[i][j] = s->rssi[j][i] = (int16_t)dbm;
}

/* disc_index() -- where `addr` is in a LEV_DISC_DONE's found list, or -1. */
static int disc_index(const link_evt_t *e, uint32_t addr)
{
    for (int k = 0; k < e->arg; k++) {
        if (e->found[k].addr == addr) {
            return k;
        }
    }
    return -1;
}

/* sweep() -- device i keys D: run until its LEV_DISC_DONE (at most 10 s).
 * Returns that event (NULL if none came) and, in *took, how long it took. */
static const logged_t *sweep(sim_t *s, int i, int64_t *took)
{
    int from = s->d[i].n_log;
    link_discover(&s->d[i].L, s->now);
    int64_t t = sim_until(s, i, LEV_DISC_DONE, ANY, 10000);
    if (took) {
        *took = t;
    }
    for (int k = s->d[i].n_log - 1; t >= 0 && k >= from; k--) {
        if (s->d[i].log[k].e.kind == LEV_DISC_DONE) {
            return &s->d[i].log[k];
        }
    }
    return NULL;
}

/* The pure parts: groups by path loss, the found list and its order. */
static void test_disc_list(void)
{
    printf("discovery: near / mid / far, the found list\n");
    CHECK(disc_prox_of(DISC_NEAR_LOSS_DB - 1) == DISC_NEAR && disc_prox_of(DISC_NEAR_LOSS_DB) == DISC_MID &&
              disc_prox_of(DISC_FAR_LOSS_DB - 1) == DISC_MID && disc_prox_of(DISC_FAR_LOSS_DB) == DISC_FAR,
          "group boundaries at %d and %d dB", DISC_NEAR_LOSS_DB, DISC_FAR_LOSS_DB);
    CHECK(disc_prox_letter(DISC_NEAR) == 'N' && disc_prox_letter(DISC_MID) == 'M' && disc_prox_letter(DISC_FAR) == 'F',
          "played as N, M, F");

    /* Answers in this order (power 17 dBm unless given), with the boundary
     * cases: 79 dB is still near, 80 already mid, 105 already far. */
    static const struct {
        const char *id;
        int rssi, dbm;
    } in[] = {
        {"F22", -100, 17}, /* 117 dB: far */
        {"N22", -50, 17},  /*  67 dB: near */
        {"M22", -80, 17},  /*  97 dB: mid */
        {"N33", -62, 17},  /*  79 dB: near */
        {"F33", -103, 2},  /* 105 dB: far, from a quieter transmitter */
        {"M33", -60, 20},  /*  80 dB: mid, from a louder one */
    };
    disc_list_t l;
    disc_list_clear(&l);
    for (size_t k = 0; k < sizeof in / sizeof in[0]; k++) {
        disc_list_add(&l, id_to_addr(in[k].id), (int16_t)in[k].rssi, (int8_t)in[k].dbm);
    }
    CHECK(!disc_list_add(&l, id_to_addr("N22"), -40, 17) && l.n == 6, "a second answer from the same device is ignored");
    CHECK(disc_loss_db(&l.e[4]) == 105 && disc_loss_db(&l.e[5]) == 80, "path loss = their power - RSSI");
    disc_list_sort(&l);
    static const char *const want[] = {"N22", "N33", "M22", "M33", "F22", "F33"};
    int ok = 0;
    for (int k = 0; k < 6; k++) {
        ok += l.e[k].addr == id_to_addr(want[k]);
    }
    CHECK(ok == 6, "near, mid, far; arrival order kept inside each group (%d of 6 in place)", ok);

    disc_list_clear(&l);
    char id[ID_MAX_LEN + 1] = "A22";
    for (int k = 0; k < DISC_MAX + 3; k++) {
        id[0] = ID_ALPHABET[k];
        disc_list_add(&l, id_to_addr(id), -70, 17);
    }
    CHECK(l.n == DISC_MAX, "the list holds at most %d (%d)", DISC_MAX, l.n);
}

/* The answering side's limits, on their own. */
static void test_disc_limit(void)
{
    printf("discovery: answer limits\n");
    const int64_t W = DISC_ANSWER_WINDOW_MS, MIN = 60000, t0 = 1000;
    uint32_t a = id_to_addr("K7Q"), b = id_to_addr("ABC");
    disc_limit_t lim;
    disc_limit_init(&lim);
    CHECK(disc_limit_allow(&lim, a, t0) && disc_limit_allow(&lim, a, t0 + MIN) && disc_limit_allow(&lim, a, t0 + 2 * MIN),
          "three answers to one searcher");
    CHECK(!disc_limit_allow(&lim, a, t0 + 9 * MIN), "a fourth within 10 minutes is refused");
    CHECK(disc_limit_allow(&lim, b, t0 + 9 * MIN), "another searcher is still answered");
    CHECK(disc_limit_allow(&lim, a, t0 + W), "10 minutes after the first answer, one more");
    CHECK(!disc_limit_allow(&lim, a, t0 + W + MIN / 2), "... but not two (the 1- and 2-minute answers still count)");
    CHECK(disc_limit_allow(&lim, a, t0 + W + MIN), "the next when the 1-minute answer has aged out");

    /* Refusals aren't counted: sweeping every second doesn't extend the block. */
    disc_limit_init(&lim);
    for (int k = 0; k < DISC_ANSWERS_PER_SRC; k++) {
        disc_limit_allow(&lim, a, t0);
    }
    int allowed = 0;
    for (int64_t t = t0; t < t0 + W; t += 1000) {
        allowed += disc_limit_allow(&lim, a, t);
    }
    CHECK(allowed == 0, "sweeping every second gets nothing more for 10 minutes (%d)", allowed);
    CHECK(disc_limit_allow(&lim, a, t0 + W), "... and is answered again after that");

    /* Everyone together. */
    disc_limit_init(&lim);
    char id[ID_MAX_LEN + 1] = "AXZ";
    int n_ok = 0;
    for (int k = 0; k < DISC_ANSWERS_TOTAL + 5; k++) {
        id[0] = ID_ALPHABET[k]; /* a different searcher each time */
        n_ok += disc_limit_allow(&lim, id_to_addr(id), t0 + k * 1000);
    }
    CHECK(n_ok == DISC_ANSWERS_TOTAL, "at most %d answers per 10 minutes in all (%d)", DISC_ANSWERS_TOTAL, n_ok);
    CHECK(disc_limit_allow(&lim, a, t0 + W), "the total frees up as answers age out");
}

/* A sweeps; four devices at different distances answer in their slots; the
 * list comes back after the window, in walking order. Nobody's user feels
 * anything. */
static void test_disc_sweep(void)
{
    printf("discovery: a sweep finds everyone, sorted near / mid / far\n");
    sim_t s;
    sim_init(&s, 5, IDS, 21);
    set_rssi(&s, A, B, -50);  /* 17 + 50  =  67 dB: near */
    set_rssi(&s, A, C, -100); /* 17 + 100 = 117 dB: far */
    set_rssi(&s, A, D, -85);  /* 102 dB: mid */
    set_rssi(&s, A, E, -60);  /*  77 dB: near */
    int tx0[SIM_MAX];
    for (int i = 0; i < s.n; i++) {
        tx0[i] = s.d[i].tx_count;
    }
    int64_t took;
    const logged_t *r = sweep(&s, A, &took);
    CHECK(r && r->e.arg == 4, "all four found (%d)", r ? r->e.arg : -1);
    CHECK(took >= 3500 && took <= 4000, "the list is ready %lld ms after D (about 3.7 s)", (long long)took);
    if (r && r->e.arg == 4) {
        const disc_entry_t *f = r->e.found;
        uint32_t b = s.d[B].addr, e = s.d[E].addr;
        bool near_first = (f[0].addr == b && f[1].addr == e) || (f[0].addr == e && f[1].addr == b);
        CHECK(near_first && f[2].addr == s.d[D].addr && f[3].addr == s.d[C].addr,
              "walking order: the two near ones, then the mid one, then the far one");
        CHECK(f[0].prox == DISC_NEAR && f[1].prox == DISC_NEAR && f[2].prox == DISC_MID && f[3].prox == DISC_FAR,
              "groups N N M F");
        int c = disc_index(&r->e, s.d[C].addr);
        CHECK(c >= 0 && f[c].rssi == -100 && f[c].dbm == SIM_DBM && disc_loss_db(&f[c]) == 117,
              "the far one: heard at -100 dBm, sent at %d dBm, 117 dB", SIM_DBM);
    }
    CHECK(s.d[A].tx_count - tx0[A] == 1, "the searcher sent one packet (%d)", s.d[A].tx_count - tx0[A]);
    int quiet = 0, once = 0;
    for (int i = B; i <= E; i++) {
        once += s.d[i].tx_count - tx0[i] == 1;
        quiet += s.d[i].n_log == 0;
    }
    CHECK(once == 4, "each answered exactly once (%d of 4)", once);
    CHECK(quiet == 4, "and none of their users felt anything (%d of 4)", quiet);
    CHECK(s.d[A].L.disc_phase == DISC_IDLE, "the sweep is over");
}

/* Silent mode and the blocklist, both ways. */
static void test_disc_silent_blocked(void)
{
    printf("discovery: silent mode and the blocklist\n");
    sim_t s;
    sim_init(&s, 4, IDS, 22);
    link_set_discoverable(&s.d[B].L, false); /* B's user keyed Z */
    uint32_t bl = s.d[C].addr;
    link_set_blocklist(&s.d[A].L, &bl, 1); /* A's user blocked C */
    const logged_t *r = sweep(&s, A, NULL);
    CHECK(r && r->e.arg == 1 && r->e.found[0].addr == s.d[D].addr, "only D is found: B is silent, C blocked (%d)",
          r ? r->e.arg : -1);
    CHECK(s.d[B].L.n_disc_refused == 1 && s.d[B].tx_count == 0, "B didn't transmit at all");
    r = sweep(&s, C, NULL);
    CHECK(r && disc_index(&r->e, s.d[A].addr) < 0 && disc_index(&r->e, s.d[D].addr) >= 0,
          "C can't find A, who blocked it (D answers C as usual)");
    r = sweep(&s, B, NULL);
    CHECK(r && r->e.arg == 3, "in silent mode B can still search: finds all three (%d)", r ? r->e.arg : -1);
    link_set_discoverable(&s.d[B].L, true);
    r = sweep(&s, A, NULL);
    CHECK(r && disc_index(&r->e, s.d[B].addr) >= 0, "silent mode off: B answers again");
}

/* Sweeping over and over: answers stop after DISC_ANSWERS_PER_SRC and come
 * back 10 minutes later. */
static void test_disc_limits_on_air(void)
{
    printf("discovery: sweeping over and over gets no more answers\n");
    sim_t s;
    sim_init(&s, 2, IDS, 23);
    int found = 0, sweeps = DISC_ANSWERS_PER_SRC + 2;
    for (int k = 0; k < sweeps; k++) {
        const logged_t *r = sweep(&s, A, NULL);
        found += r ? r->e.arg : 0;
        sim_run(&s, 5000);
    }
    CHECK(found == DISC_ANSWERS_PER_SRC, "B answered %d of %d sweeps (limit %d)", found, sweeps, DISC_ANSWERS_PER_SRC);
    CHECK(s.d[B].L.n_disc_refused == 2 && s.d[B].L.n_disc_answered == DISC_ANSWERS_PER_SRC,
          "the others went unanswered (%lu answered, %lu not)", (unsigned long)s.d[B].L.n_disc_answered,
          (unsigned long)s.d[B].L.n_disc_refused);
    sim_run(&s, DISC_ANSWER_WINDOW_MS);
    const logged_t *r = sweep(&s, A, NULL);
    CHECK(r && r->e.arg == 1, "10 minutes later B answers again");
}

/* Nobody around; a sweep replaced by a newer one; stray answers; no ID; the
 * duty cycle on either side. */
static void test_disc_edges(void)
{
    printf("discovery: nobody around, a replaced sweep, stray answers, duty cycle\n");
    sim_t s;
    sim_init(&s, 3, IDS, 24);
    set_range(&s, A, B, false);
    set_range(&s, A, C, false);
    int64_t took;
    const logged_t *r = sweep(&s, A, &took);
    CHECK(r && r->e.arg == 0 && took >= 3500 && took <= 4000, "nobody in range: an empty list after the window (%lld ms)",
          (long long)took);

    /* A second D while the first sweep listens: only the newer one reports. */
    set_range(&s, A, B, true);
    int n0 = count_ev(&s, A, 0, LEV_DISC_DONE, ANY, -1);
    link_discover(&s.d[A].L, s.now);
    sim_run(&s, 1000);
    CHECK(s.d[A].L.disc_phase == DISC_LISTENING, "the first sweep is listening");
    link_discover(&s.d[A].L, s.now);
    sim_run(&s, 8000);
    r = last_ev(&s, A, LEV_DISC_DONE);
    CHECK(count_ev(&s, A, 0, LEV_DISC_DONE, ANY, -1) == n0 + 1 && r && r->e.arg == 1,
          "one report, from the newer sweep, with B in it once");

    /* Stray answers: C (out of A's range) is faked straight into A's radio. */
    uint8_t buf[PROTO_MAX_PKT];
    pkt_t p;
    proto_init_pkt(&p, PT_PRESENCE, s.d[C].addr, s.d[A].addr, 999);
    p.payload[0] = 17;
    p.payload[1] = (uint8_t)s.d[A].L.disc_id;
    p.plen = DISC_PRESENCE_LEN;
    int len = proto_encode(&p, buf, sizeof buf);
    link_on_rx(&s.d[A].L, buf, len, -70, 20, s.now); /* no sweep listening */
    link_discover(&s.d[A].L, s.now);
    sim_run(&s, 500);
    p.payload[1] = (uint8_t)(s.d[A].L.disc_id + 1); /* listening, but tagged for another sweep */
    len = proto_encode(&p, buf, sizeof buf);
    link_on_rx(&s.d[A].L, buf, len, -70, 20, s.now);
    sim_until(&s, A, LEV_DISC_DONE, ANY, 8000);
    r = last_ev(&s, A, LEV_DISC_DONE);
    CHECK(r && disc_index(&r->e, s.d[C].addr) < 0, "an answer outside our sweep, or for another sweep, is ignored");

    /* No ID yet: refused at once, reported as nobody. */
    static link_t solo; /* (large: off the stack) */
    link_init(&solo, ADDR_NONE, &LP, 1, 0, NULL, NULL, 0);
    link_evt_t ev;
    CHECK(!link_discover(&solo, 0) && link_get_event(&solo, &ev) && ev.kind == LEV_DISC_DONE && ev.arg == 0,
          "without an ID: refused, reported at once as nobody found");

    /* The hour's budget used up: our DISCOVER can't go, so after
     * DISC_SEND_HARD_MS the sweep reports nobody. */
    sim_init(&s, 2, IDS, 25);
    dc_add(&s.d[A].L.dc, s.now, dc_budget_ms());
    r = sweep(&s, A, &took);
    CHECK(r && r->e.arg == 0 && took >= DISC_SEND_HARD_MS && took < DISC_SEND_HARD_MS + 500 && s.d[A].tx_count == 0,
          "duty cycle used up: nothing sent, nobody reported after %lld ms", (long long)took);

    /* An answerer without budget stays quiet, and doesn't answer late. */
    sim_init(&s, 2, IDS, 26);
    dc_add(&s.d[B].L.dc, s.now, dc_budget_ms());
    r = sweep(&s, A, NULL);
    sim_run(&s, 10000);
    CHECK(r && r->e.arg == 0 && s.d[B].tx_count == 0, "an answerer with no budget left stays quiet (no late answer)");
}

/* Two people sweep at the same moment: each list has the others, never
 * itself, and nobody twice. */
static void test_disc_two_at_once(void)
{
    printf("discovery: two sweeps at once\n");
    sim_t s;
    sim_init(&s, 4, IDS, 27);
    link_discover(&s.d[A].L, s.now);
    link_discover(&s.d[C].L, s.now);
    sim_run(&s, 8000);
    const logged_t *ra = last_ev(&s, A, LEV_DISC_DONE), *rc = last_ev(&s, C, LEV_DISC_DONE);
    CHECK(ra && rc, "both sweeps report");
    if (ra && rc) {
        CHECK(disc_index(&ra->e, s.d[A].addr) < 0 && disc_index(&rc->e, s.d[C].addr) < 0, "nobody finds themselves");
        CHECK(ra->e.arg >= 2 && rc->e.arg >= 2, "each finds most of the others despite answering too (A %d, C %d of 3)",
              ra->e.arg, rc->e.arg);
        int dupes = 0;
        for (int i = 0; i < ra->e.arg; i++) {
            dupes += disc_index(&ra->e, ra->e.found[i].addr) != i;
        }
        CHECK(dupes == 0, "nobody listed twice");
    }
}

/* Inviting someone from the list is an ordinary request. */
static void test_disc_invite(void)
{
    printf("discovery: invite someone found\n");
    sim_t s;
    sim_init(&s, 3, IDS, 28);
    set_rssi(&s, A, B, -50);  /* near (the channel's default, -90 dBm, would be far too) */
    set_rssi(&s, A, C, -100); /* far */
    s.d[C].accept_after_ms = 2000;
    const logged_t *r = sweep(&s, A, NULL);
    CHECK(r && r->e.arg == 2 && r->e.found[1].addr == s.d[C].addr && r->e.found[1].prox == DISC_FAR,
          "C found, listed last as far");
    if (!r || r->e.arg != 2) {
        return;
    }
    link_request(&s.d[A].L, r->e.found[1].addr, s.now);
    int64_t t = sim_until(&s, A, LEV_REQ_RESULT, s.d[C].addr, 20000);
    const logged_t *res = last_ev(&s, A, LEV_REQ_RESULT);
    sim_run(&s, 3000);
    CHECK(t >= 0 && res && res->e.arg == LRES_ACCEPTED && s.d[A].L.peer == s.d[C].addr && s.d[C].L.peer == s.d[A].addr,
          "C's user accepts: linked");
}

/* ------------------------------------------------------------------------- */
/* The phone relay (phone.h)                                                  */
/* ------------------------------------------------------------------------- */

/* Linked, then out of LoRa range with both phones connected: the link stays
 * up and messages get through both ways, receipts included. */
static void test_relay_out_of_range(void)
{
    printf("relay: out of LoRa range, both phones connected\n");
    sim_t s;
    sim_init(&s, 2, IDS, 31);
    CHECK(sim_connect(&s, A, B), "connected over LoRa");
    sim_run(&s, 60000); /* keepalives over LoRa give A a reading of B's signal (-90 dBm here) */
    CHECK(s.d[A].L.peer_rssi == -90, "B heard over LoRa at %d dBm", s.d[A].L.peer_rssi);
    s.phone[A] = s.phone[B] = true;
    set_range(&s, A, B, false);
    sim_run(&s, 5 * 60000);
    CHECK(count_ev(&s, A, 0, LEV_LINK_LOST, ANY, -1) == 0 && count_ev(&s, B, 0, LEV_LINK_LOST, ANY, -1) == 0,
          "no 'connection lost' in 5 minutes: the keepalives go through the phones");
    CHECK(path_now(&s, A, LP_LORA) == PS_LOST && path_now(&s, B, LP_LORA) == PS_LOST && path_now(&s, A, LP_RELAY) == PS_UP &&
              path_now(&s, B, LP_RELAY) == PS_UP,
          "but LoRa shows lost on both, the relay connected (15 Oct: it said 'connected' for LoRa too)");
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "VIA PHONE", s.now);
    int64_t t = sim_until(&s, A, LEV_MSG_SENT, ANY, 15000);
    const logged_t *m = last_ev(&s, B, LEV_MSG);
    CHECK(t >= 0 && t < 3000 && m && strcmp(m->e.text, "VIA PHONE") == 0,
          "a message gets through, and its receipt comes back (%lld ms)", (long long)t);
    link_send_msg(&s.d[B].L, s.d[B].L.peer, "GOT IT", s.now);
    CHECK(sim_until(&s, B, LEV_MSG_SENT, ANY, 15000) >= 0, "and the reply");
    CHECK(s.d[A].L.peer_rssi == -90 && s.d[B].L.n_rx_phone > 0,
          "the partner's signal reading is still the last one heard over LoRa (%d dBm)", s.d[A].L.peer_rssi);
}

/* In range with both phones: every packet arrives twice, everything happens
 * once. */
static void test_relay_both_paths(void)
{
    printf("relay: in range with phones too: everything happens once\n");
    sim_t s;
    sim_init(&s, 2, IDS, 32);
    s.phone[A] = s.phone[B] = true;
    CHECK(sim_connect(&s, A, B), "connected (the request went both ways)");
    char text[16];
    for (int i = 0; i < 10; i++) {
        snprintf(text, sizeof text, "MSG %d", i);
        link_send_msg(&s.d[A].L, s.d[A].L.peer, text, s.now);
        sim_run(&s, 3000);
    }
    CHECK(count_ev(&s, B, 0, LEV_MSG, ANY, -1) == 10, "10 messages delivered once each (%d)",
          count_ev(&s, B, 0, LEV_MSG, ANY, -1));
    CHECK(count_ev(&s, A, 0, LEV_MSG_SENT, ANY, -1) == 10, "10 OKs");
    CHECK(count_ev(&s, A, 0, LEV_REQ_RESULT, ANY, -1) == 1 && count_ev(&s, B, 0, LEV_REQ_IN, ANY, -1) == 1,
          "one request, one result");
    CHECK(s.d[B].L.n_dup >= 10 && s.relayed > 20, "the second copies were recognised (%lu duplicates, %d relayed)",
          (unsigned long)s.d[B].L.n_dup, s.relayed);
}

/* Only one side has its phone: out of range nothing can get through. */
static void test_relay_one_phone(void)
{
    printf("relay: only one phone connected\n");
    sim_t s;
    sim_init(&s, 2, IDS, 33);
    CHECK(sim_connect(&s, A, B), "connected");
    s.phone[A] = true;
    set_range(&s, A, B, false);
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "NOBODY HOME", s.now);
    CHECK(sim_until(&s, A, LEV_MSG_FAILED, ANY, 30000) >= 0 && count_ev(&s, B, 0, LEV_MSG, ANY, -1) == 0,
          "the message fails as before: B's phone isn't there to receive it");
}

/* Out of LoRa range from the start: connecting, an SOS and its
 * acknowledgement all work through the phones. */
static void test_relay_connect_sos(void)
{
    printf("relay: connect and SOS through the phones only\n");
    sim_t s;
    sim_init(&s, 2, IDS, 34);
    set_range(&s, A, B, false);
    s.phone[A] = s.phone[B] = true;
    s.d[B].ack_sos = true;
    CHECK(sim_connect(&s, A, B), "A asks B, B accepts: linked through the phones");
    link_send_sos(&s.d[A].L, ADDR_BROADCAST, s.now);
    CHECK(sim_until(&s, B, LEV_SOS, s.d[A].addr, 10000) >= 0, "A's general SOS reaches its partner B");
    CHECK(sim_until(&s, A, LEV_SOS_ACK, s.d[B].addr, 10000) >= 0, "B's acknowledgement comes back");
}

/* Discovery is about the radio: the phones never carry it. */
static void test_relay_no_discovery(void)
{
    printf("relay: discovery stays on the radio\n");
    sim_t s;
    sim_init(&s, 2, IDS, 35);
    s.phone[A] = s.phone[B] = true;
    set_range(&s, A, B, false);
    const logged_t *r = sweep(&s, A, NULL);
    CHECK(r && r->e.arg == 0, "out of LoRa range, B isn't found through the phones");
    link_discover(&s.d[A].L, s.now);
    sim_run(&s, 500);
    pkt_t p;
    uint8_t buf[PROTO_MAX_PKT];
    proto_init_pkt(&p, PT_PRESENCE, s.d[B].addr, s.d[A].addr, 77);
    p.payload[0] = 17;
    p.payload[1] = (uint8_t)s.d[A].L.disc_id; /* the right tag, while listening */
    p.plen = DISC_PRESENCE_LEN;
    int len = proto_encode(&p, buf, sizeof buf);
    link_on_rx_via(&s.d[A].L, buf, len, 0, 0, true, s.now);
    sim_until(&s, A, LEV_DISC_DONE, ANY, 8000);
    r = last_ev(&s, A, LEV_DISC_DONE);
    CHECK(r && r->e.arg == 0, "an answer that comes through the phone doesn't count");
}

/* ------------------------------------------------------------------------- */
/* Paths and the connection-lost fix (agreed 15 Oct, v0.6.0)                  */
/* ------------------------------------------------------------------------- */

/* The timing at SF9 is exactly the constants every earlier version used,
 * and the slow settings stretch it (HANDOVER §5.2). */
static void test_timing(void)
{
    printf("paths: timing per spreading factor\n");
    link_timing_t t;
    link_timing(&LP, &t);
    CHECK(t.keepalive_ms == KEEPALIVE_MS && t.ka_jitter_ms == KEEPALIVE_JITTER_MS && t.lost_ms == LINK_LOST_MS,
          "SF9: keepalive %u, jitter %u, lost %u ms", t.keepalive_ms, t.ka_jitter_ms, t.lost_ms);
    CHECK(t.retry_gap_ms == RETRY_GAP_MS && t.retry_jitter_ms == RETRY_JITTER_MS && t.reply_wait_ms == REPLY_WAIT_MS,
          "SF9: retries %u + %u ms, wait %u ms", t.retry_gap_ms, t.retry_jitter_ms, t.reply_wait_ms);
    CHECK(t.hard_ms == LINK_HARD_TIMEOUT_MS && t.reply_hard_ms == 5000 && t.lbt_max_ms == 1500 &&
              t.disc_send_hard_ms == DISC_SEND_HARD_MS && t.disc_late_ms == DISC_LATE_MS && t.pong_spread_ms == 1000 &&
              t.sos_relay_max_ms == SOS_RELAY_MAX_MS && t.relay_hard_ms == 10000,
          "SF9: the other limits as before");
    uint32_t w9 = link_disc_window_ms(&LP);
    CHECK(w9 > 3600 && w9 < 3800, "SF9: a sweep takes %u ms (about 3.7 s)", w9);
    uint32_t prev_gap = 0, prev_lost = 0;
    for (int sf = 10; sf <= 12; sf++) {
        lora_params_t lp = {(uint8_t)sf, RADIO_BW_HZ, RADIO_CR};
        link_timing(&lp, &t);
        uint32_t rcpt = (lora_airtime_us(PROTO_HDR_LEN + 2, &lp, 8) + 999) / 1000;
        uint32_t big = (lora_airtime_us(PROTO_HDR_LEN + MSG_MAX_CHARS, &lp, 8) + 999) / 1000;
        printf("  SF%d: keepalive %u s, lost %u s, retry gap %u ms (receipt %u ms), give up after %u s, LBT %u ms, "
               "sweep %u ms\n",
               sf, t.keepalive_ms / 1000, t.lost_ms / 1000, t.retry_gap_ms, rcpt, t.hard_ms / 1000, t.lbt_max_ms,
               link_disc_window_ms(&lp));
        CHECK(t.retry_gap_ms >= 2 * rcpt + 600, "SF%d: a receipt fits twice into the retry gap", sf);
        CHECK(t.hard_ms >= LINK_TRIES * (big + t.retry_gap_ms + t.retry_jitter_ms) + t.reply_wait_ms,
              "SF%d: all copies of the longest message fit before giving up", sf);
        CHECK(t.lbt_max_ms > big, "SF%d: listen-before-talk can wait out the longest packet", sf);
        CHECK(t.lost_ms == 3 * t.keepalive_ms && t.retry_gap_ms >= prev_gap && t.lost_ms >= prev_lost,
              "SF%d: three keepalives per lost time; nothing shrinks", sf);
        prev_gap = t.retry_gap_ms;
        prev_lost = t.lost_ms;
    }
    lora_params_t sf12 = {12, RADIO_BW_HZ, RADIO_CR};
    link_timing(&sf12, &t);
    CHECK(t.keepalive_ms == RADIO_KA_SF12_MS && t.lost_ms == 3 * RADIO_KA_SF12_MS, "SF12: keepalive 60 s, lost 180 s");
    uint32_t ka12 = (lora_airtime_us(PROTO_HDR_LEN + KA_REPORT_LEN, &sf12, 8) + 999) / 1000;
    CHECK(ka12 * 1000 / t.keepalive_ms < 25, "SF12: keepalives cost %.1f %% of the airtime (under 2.5 %%)",
          ka12 * 100.0 / t.keepalive_ms);
    CHECK(link_disc_window_ms(&sf12) + 5000 < 30000, "SF12: a sweep (%u ms) fits the UI's wait with room to spare",
          link_disc_window_ms(&sf12));
    /* The report makes a keepalive 12 bytes: still 144 ms at SF9. */
    CHECK(lora_airtime_us(PROTO_HDR_LEN + KA_REPORT_LEN, &LP, 8) == lora_airtime_us(PROTO_HDR_LEN, &LP, 8),
          "SF9: the report costs no airtime");
}

/* The bug of 15 Oct (mall session): a message fails because B has walked
 * away, and B then stays silent for half an hour. Up to v0.5.1 the link
 * went down silently at the failure and nothing was reported ever after.
 * Now: one 'lost' (no-receipt) at the failure, nothing more during the
 * silence, the paths say lost, and B (which heard nothing) reports
 * silence once. */
static void test_fail_then_silence(void)
{
    printf("paths: a failed message, then half an hour of silence (the 15 Oct bug)\n");
    sim_t s;
    sim_init(&s, 2, IDS, 41);
    CHECK(sim_connect(&s, A, B), "connected");
    sim_run(&s, 40000);
    CHECK(path_now(&s, A, LP_LORA) == PS_UP && path_now(&s, B, LP_LORA) == PS_UP, "LoRa proven both ways on both");
    set_range(&s, A, B, false);
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "HELLO", s.now);
    int64_t t = sim_until(&s, A, LEV_LINK_LOST, ANY, 40000);
    const logged_t *l = last_ev(&s, A, LEV_LINK_LOST), *f = last_ev(&s, A, LEV_MSG_FAILED);
    CHECK(t >= 0 && l && l->e.arg == LOST_NO_RECEIPT && f && f->t == l->t,
          "A: lost (no-receipt) at the failure, %lld ms after sending", (long long)t);
    CHECK(path_now(&s, A, LP_LORA) == PS_THEY_CANT || path_now(&s, A, LP_LORA) == PS_LOST,
          "A's LoRa is no longer 'connected' (%s)", link_path_state_name(path_now(&s, A, LP_LORA)));
    sim_run(&s, 30 * 60000);
    CHECK(count_ev(&s, A, 0, LEV_LINK_LOST, ANY, -1) == 1 && count_ev(&s, A, 0, LEV_LINK_RESTORED, ANY, -1) == 0,
          "A: nothing more in 30 minutes of silence (one loss, one event)");
    CHECK(s.d[A].L.state == LS_LOST && path_now(&s, A, LP_LORA) == PS_LOST, "A: still lost, LoRa lost");
    const logged_t *lb = last_ev(&s, B, LEV_LINK_LOST);
    CHECK(count_ev(&s, B, 0, LEV_LINK_LOST, ANY, -1) == 1 && lb && lb->e.arg == LOST_SILENCE,
          "B: lost once, reason silence");
    CHECK(count_path(&s, A, 0, LP_LORA, PS_LOST) >= 1, "A's LoRa path reported going lost (LEV_PATH)");
    set_range(&s, A, B, true);
    CHECK(sim_until(&s, A, LEV_LINK_RESTORED, ANY, 40000) >= 0, "restored once back in range");
    sim_run(&s, 5000);
    CHECK(s.d[B].L.state == LS_UP && path_now(&s, A, LP_LORA) == PS_UP && path_now(&s, B, LP_LORA) == PS_UP,
          "both up, LoRa proven again");
}

/* One-way: A still hears B, B can't hear A (the owner's "heartbeat back"
 * case, e.g. noise next to B). Up to v0.5.1 A thought the link was up as
 * long as it heard B. Now A notices from B's reports, says why, never
 * shows the link as connected while it lasts, sends no extra traffic, and
 * both ends come back together when it ends. */
static void test_one_way(void)
{
    printf("paths: one-way (B can't hear A)\n");
    sim_t s;
    sim_init(&s, 2, IDS, 42);
    CHECK(sim_connect(&s, A, B), "connected");
    sim_run(&s, 40000);
    s.hear[A][B] = false; /* B hears nothing from A; A still hears B */
    int64_t ta = sim_until(&s, A, LEV_LINK_LOST, ANY, 200000);
    const logged_t *la = last_ev(&s, A, LEV_LINK_LOST);
    /* B's reports say how long since it heard A: once that passes the lost
     * time (90 s after the last A packet B heard), the next report tells A.
     * So 60-120 s after it starts. */
    CHECK(ta >= 0 && ta <= 125000 && la && la->e.arg == LOST_ONE_WAY, "A: lost after %lld ms, reason %s", (long long)ta,
          la ? link_lost_why_name(la->e.arg) : "-");
    CHECK(path_now(&s, A, LP_LORA) == PS_THEY_CANT, "A's LoRa: they can't hear us (%s)",
          link_path_state_name(path_now(&s, A, LP_LORA)));
    sim_run(&s, 20000);
    const logged_t *lb = last_ev(&s, B, LEV_LINK_LOST);
    CHECK(s.d[B].L.state == LS_LOST && lb && lb->e.arg == LOST_SILENCE, "B: lost, reason silence (it hears nothing)");
    CHECK(!link_send_msg(&s.d[A].L, s.d[A].L.peer, "CAN YOU HEAR ME", s.now), "A can't send: B wouldn't get it");
    int tx0 = s.d[A].tx_count, l0 = s.d[A].n_log;
    sim_run(&s, 10 * 60000);
    CHECK(count_ev(&s, A, l0, LEV_LINK_RESTORED, ANY, -1) == 0 && count_ev(&s, A, l0, LEV_LINK_LOST, ANY, -1) == 0,
          "10 minutes one-way: no flapping between lost and restored");
    int kas = s.d[A].tx_count - tx0;
    CHECK(kas >= 18 && kas <= 24, "A keeps to its keepalive period: %d packets in 10 min (20 expected)", kas);
    s.hear[A][B] = true;
    int64_t tr = sim_until(&s, A, LEV_LINK_RESTORED, ANY, 45000);
    sim_run(&s, 3000);
    CHECK(tr >= 0 && s.d[B].L.state == LS_UP, "both restored when it ends (A after %lld ms)", (long long)tr);
}

/* The same one-way LoRa, with both phones relaying: the link stays up
 * through the relay, each side's LoRa says which way it fails, and
 * messages get through. */
static void test_one_way_with_relay(void)
{
    printf("paths: one-way LoRa while the phones relay\n");
    sim_t s;
    sim_init(&s, 2, IDS, 43);
    s.phone[A] = s.phone[B] = true;
    CHECK(sim_connect(&s, A, B), "connected (both paths)");
    sim_run(&s, 60000);
    CHECK(path_now(&s, A, LP_LORA) == PS_UP && path_now(&s, A, LP_RELAY) == PS_UP, "A: LoRa and relay connected");
    s.hear[A][B] = false;
    sim_run(&s, 3 * 60000);
    CHECK(s.d[A].L.state == LS_UP && s.d[B].L.state == LS_UP, "both still connected: the relay carries");
    CHECK(count_ev(&s, A, 0, LEV_LINK_LOST, ANY, -1) == 0 && count_ev(&s, B, 0, LEV_LINK_LOST, ANY, -1) == 0,
          "no 'lost' anywhere");
    CHECK(path_now(&s, A, LP_LORA) == PS_THEY_CANT && path_now(&s, B, LP_LORA) == PS_WE_CANT,
          "A's LoRa: they can't hear us (%s); B's: can't hear them (%s)", link_path_state_name(path_now(&s, A, LP_LORA)),
          link_path_state_name(path_now(&s, B, LP_LORA)));
    CHECK(path_now(&s, A, LP_RELAY) == PS_UP && path_now(&s, B, LP_RELAY) == PS_UP, "the relay connected on both");
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "VIA THE PHONES", s.now);
    CHECK(sim_until(&s, A, LEV_MSG_SENT, ANY, 15000) >= 0, "A's message gets through (and its receipt back)");
    int tx0 = s.d[A].tx_count + s.d[B].tx_count;
    sim_run(&s, 10 * 60000);
    int kas = s.d[A].tx_count + s.d[B].tx_count - tx0;
    CHECK(kas <= 2 * 24, "no keepalive storm: %d packets from both in 10 min (40 expected)", kas);
}

/* LoRa dead (out of range), the relay carrying; then A's phone goes away:
 * the link is lost at once (nothing left), and comes back only once the
 * relay is proven again, not merely because the phone is back. */
static void test_relay_drop(void)
{
    printf("paths: the relay carrying, then the phone goes away and comes back\n");
    sim_t s;
    sim_init(&s, 2, IDS, 44);
    s.phone[A] = s.phone[B] = true;
    CHECK(sim_connect(&s, A, B), "connected");
    set_range(&s, A, B, false);
    sim_run(&s, 3 * 60000);
    CHECK(s.d[A].L.state == LS_UP && path_now(&s, A, LP_LORA) == PS_LOST && path_now(&s, A, LP_RELAY) == PS_UP,
          "LoRa lost, relay connected, link up");
    CHECK(count_path(&s, A, 0, LP_LORA, PS_LOST) == 1, "A's LoRa went lost once (an event for the phone)");
    s.phone[A] = false;
    int64_t t = sim_until(&s, A, LEV_LINK_LOST, ANY, 2000);
    CHECK(t >= 0 && path_now(&s, A, LP_RELAY) == PS_OFF, "A: lost as soon as its phone went (%lld ms); relay off",
          (long long)t);
    int64_t tb = sim_until(&s, B, LEV_LINK_LOST, ANY, 120000);
    CHECK(tb >= 0, "B: lost too, once nothing came through for the lost time (%lld ms)", (long long)tb);
    sim_run(&s, 2 * 60000); /* long enough for what came through the relay before to be old news */
    int l0 = s.d[A].n_log;
    s.phone[A] = true;
    sim_run(&s, 100);
    CHECK(s.d[A].L.state == LS_LOST && count_ev(&s, A, l0, LEV_LINK_RESTORED, ANY, -1) == 0,
          "the phone back is not yet proof: still lost (relay %s)", link_path_state_name(path_now(&s, A, LP_RELAY)));
    int64_t tr = sim_until(&s, A, LEV_LINK_RESTORED, ANY, 45000);
    sim_run(&s, 3000);
    CHECK(tr >= 0 && s.d[B].L.state == LS_UP, "restored through the relay (A after %lld ms)", (long long)tr);
}

/* feed_ka() -- hand engine L a KEEPALIVE from `from` (to L's owner), as
 * older firmware sends it (report < 0: no payload) or with a report. */
static void feed_ka(link_t *L, uint32_t from, int lora_s, int relay_s, bool via_phone, int64_t now)
{
    pkt_t p;
    uint8_t buf[PROTO_MAX_PKT];
    static uint16_t id = 100;
    proto_init_pkt(&p, PT_KEEPALIVE, from, L->own, id++);
    if (lora_s >= 0) {
        p.payload[0] = (uint8_t)lora_s;
        p.payload[1] = (uint8_t)relay_s;
        p.plen = KA_REPORT_LEN;
    }
    int len = proto_encode(&p, buf, sizeof buf);
    link_on_rx_via(L, buf, len, -80, 20, via_phone, now);
}

/* drain() -- take L's events; returns how many of `kind` there were (and
 * the last one's arg in *arg, if any). */
static int drain(link_t *L, uint8_t kind, int *arg)
{
    link_evt_t e;
    int n = 0;
    while (link_get_event(L, &e)) {
        if (e.kind == kind) {
            n++;
            if (arg) {
                *arg = e.arg;
            }
        }
    }
    return n;
}

/* poll_to() -- run L's timers every 100 ms up to `until`. */
static void poll_to(link_t *L, int64_t *now, int64_t until)
{
    while (*now < until) {
        *now += 100;
        link_poll(L, *now);
    }
}

/* A partner on firmware before v0.6.0 sends keepalives without a report:
 * hearing it counts both ways, as before; a failed message brings the link
 * down (once) and the next packet heard brings it back. A partner with
 * reports is believed: "I haven't heard you" makes the path one-way, and
 * a report goes stale with age. */
static void test_reports_by_hand(void)
{
    printf("paths: keepalive reports, and partners without them (older firmware)\n");
    static link_t L; /* (large: off the stack) */
    uint32_t a = id_to_addr("K7Q"), b = id_to_addr("ABC");
    int64_t now = 1000;
    int arg = -1;
    link_init(&L, a, &LP, 7, 0, NULL, NULL, now);
    link_set_peer(&L, b, now);
    CHECK(link_path_state(&L, LP_LORA, now) == PS_CHECKING && link_path_state(&L, LP_RELAY, now) == PS_OFF,
          "a new link: LoRa checking, relay off");
    /* Older firmware. */
    now += 5000;
    feed_ka(&L, b, -1, 0, false, now);
    CHECK(link_path_state(&L, LP_LORA, now) == PS_UP, "a keepalive without a report: LoRa connected (as before)");
    poll_to(&L, &now, now + 89000);
    CHECK(L.state == LS_UP && link_path_state(&L, LP_LORA, now) == PS_UP, "89 s later still up");
    poll_to(&L, &now, now + 2000);
    CHECK(L.state == LS_LOST && drain(&L, LEV_LINK_LOST, &arg) == 1 && arg == LOST_SILENCE, "91 s: lost, silence");
    now += 1000;
    feed_ka(&L, b, -1, 0, false, now);
    CHECK(L.state == LS_UP && drain(&L, LEV_LINK_RESTORED, NULL) == 1, "the next keepalive restores it");
    /* Our keepalives carry our report: the first one soon (the path came
     * back: ka_soon), the next a period later. */
    int64_t heard_at = now;
    uint8_t buf[PROTO_MAX_PKT];
    int len;
    bool lp;
    pkt_t p;
    int reports[2] = {-1, -1}, n_rep = 0;
    int64_t sent_at[2] = {0, 0};
    for (int k = 0; k < 400 && n_rep < 2; k++) {
        now += 100;
        link_poll(&L, now);
        if (link_take_tx(&L, now, buf, &len, &lp)) {
            if (proto_decode(buf, len, &p) == PROTO_OK && p.type == PT_KEEPALIVE && p.plen == KA_REPORT_LEN &&
                p.payload[1] == KA_NEVER) {
                sent_at[n_rep] = now;
                reports[n_rep++] = p.payload[0];
            }
            link_tx_done(&L, now, true);
        }
    }
    CHECK(n_rep == 2 && sent_at[0] - heard_at <= 1500 && reports[0] <= 1, "a keepalive within 1.5 s: heard you %d s ago",
          reports[0]);
    CHECK(n_rep == 2 && sent_at[1] - sent_at[0] >= 26000 && sent_at[1] - sent_at[0] <= 30100 &&
              reports[1] == (int)((sent_at[1] - heard_at) / 1000),
          "the next a period later: heard you %d s ago over LoRa, never through the phones", reports[1]);
    /* A failed message (by hand: queued, never answered). */
    feed_ka(&L, b, -1, 0, false, now);
    drain(&L, 0, NULL);
    CHECK(link_send_msg(&L, b, "HELLO", now), "a message queued");
    for (int k = 0; k < 400 && L.state == LS_UP; k++) { /* send its copies, no receipt comes */
        now += 100;
        link_poll(&L, now);
        if (link_take_tx(&L, now, buf, &len, &lp)) {
            link_tx_done(&L, now, true);
        }
    }
    int lost = drain(&L, LEV_LINK_LOST, &arg);
    CHECK(L.state == LS_LOST && lost == 1 && arg == LOST_NO_RECEIPT, "no receipt: lost once, reason no-receipt");
    CHECK(link_path_state(&L, LP_LORA, now) == PS_THEY_CANT, "LoRa: they can't hear us (%s)",
          link_path_state_name(link_path_state(&L, LP_LORA, now)));
    now += 2000;
    feed_ka(&L, b, -1, 0, false, now);
    CHECK(L.state == LS_UP, "older firmware: a packet heard after the failure restores it");

    /* A partner with reports. */
    link_init(&L, a, &LP, 8, 0, NULL, NULL, now);
    link_set_peer(&L, b, now);
    now += 3000;
    feed_ka(&L, b, KA_NEVER, KA_NEVER, false, now);
    CHECK(link_path_state(&L, LP_LORA, now) == PS_CHECKING, "they haven't heard us yet: still checking");
    for (int k = 0; k < 4; k++) { /* their keepalives keep coming, every 25 s, still saying "never heard you" */
        poll_to(&L, &now, now + 25000);
        feed_ka(&L, b, KA_NEVER, KA_NEVER, false, now);
    }
    CHECK(L.state == LS_LOST && link_path_state(&L, LP_LORA, now) == PS_THEY_CANT && drain(&L, LEV_LINK_LOST, &arg) == 1 &&
              arg == LOST_ONE_WAY,
          "the checking time is over, they still can't hear us: lost, one-way");
    /* Losing the path to one-way asks for our keepalive at once (ka_soon). */
    bool got = false;
    for (int k = 0; k < 20 && !got; k++) {
        now += 100;
        link_poll(&L, now);
        got = link_take_tx(&L, now, buf, &len, &lp) && proto_decode(buf, len, &p) == PROTO_OK && p.type == PT_KEEPALIVE;
        if (got) {
            link_tx_done(&L, now, true);
        }
    }
    CHECK(got, "and our keepalive goes out within 2 s (they learn we hear them)");
    now += 500;
    feed_ka(&L, b, 1, KA_NEVER, false, now);
    CHECK(L.state == LS_UP && link_path_state(&L, LP_LORA, now) == PS_UP && drain(&L, LEV_LINK_RESTORED, NULL) == 1,
          "their next report says they heard us 1 s ago: restored, LoRa connected");
    poll_to(&L, &now, now + 20000);
    feed_ka(&L, b, 80, KA_NEVER, false, now);
    CHECK(link_path_state(&L, LP_LORA, now) == PS_UP, "a report of 80 s ago still counts ...");
    poll_to(&L, &now, now + 20000);
    feed_ka(&L, b, 100, KA_NEVER, false, now);
    CHECK(link_path_state(&L, LP_LORA, now) == PS_THEY_CANT && L.state == LS_LOST,
          "... the next one says 100 s: they can't hear us, the link is lost (%s)",
          link_path_state_name(link_path_state(&L, LP_LORA, now)));
    drain(&L, 0, NULL);
    poll_to(&L, &now, now + 2000);
    feed_ka(&L, b, 1, KA_NEVER, false, now);
    CHECK(L.state == LS_UP, "and one saying 1 s brings it back");
    /* The relay: their report arrives through the phones. */
    link_set_relay(&L, true, now);
    now += 500;
    feed_ka(&L, b, 2, 1, true, now);
    CHECK(link_path_state(&L, LP_RELAY, now) == PS_UP && link_path_state(&L, LP_LORA, now) == PS_UP,
          "a report through the phones proves both paths (LoRa: they heard us 2 s ago; we heard them over LoRa)");
    link_set_relay(&L, false, now);
    CHECK(link_path_state(&L, LP_RELAY, now) == PS_OFF, "phone gone: relay off");
    /* A new radio setting: LoRa starts checking again. */
    link_set_lora(&L, &LP, now);
    CHECK(link_path_state(&L, LP_LORA, now) == PS_CHECKING && L.state == LS_UP,
          "after a new radio setting: LoRa checking, the link still up");
    poll_to(&L, &now, now + LINK_LOST_MS + 1000);
    CHECK(L.state == LS_LOST && link_path_state(&L, LP_LORA, now) == PS_LOST,
          "nobody heard on the new setting within the lost time: lost");
}

/* At SF12 (G 4): connecting, a message, keepalives and losing the link
 * all work with the stretched timing. */
static void test_sf12(void)
{
    printf("paths: everything at SF12\n");
    sim_t s;
    sim_init(&s, 2, IDS, 45);
    sim_set_sf(&s, 12);
    CHECK(sim_connect(&s, A, B), "connected at SF12");
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "LONG RANGE IS SLOW BUT IT GETS THERE", s.now);
    int64_t t = sim_until(&s, A, LEV_MSG_SENT, ANY, 30000);
    CHECK(t > 2000 && t < 8000, "delivered and receipted in %lld ms (about 3.4 s of airtime)", (long long)t);
    int tx0 = s.d[A].tx_count;
    sim_run(&s, 60 * 60000);
    int per_hour = s.d[A].tx_count - tx0;
    CHECK(count_ev(&s, A, 0, LEV_LINK_LOST, ANY, -1) == 0, "no false 'lost' in an hour");
    CHECK(per_hour >= 55 && per_hour <= 75, "%d keepalives an hour (one a minute)", per_hour);
    CHECK(link_duty_used_ms(&s.d[A].L, s.now) < 90000, "under 2.5 %% airtime (%u ms)", link_duty_used_ms(&s.d[A].L, s.now));
    set_range(&s, A, B, false);
    int tx1 = s.d[A].tx_count;
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "ANYONE", s.now);
    t = sim_until(&s, A, LEV_MSG_FAILED, ANY, 120000);
    CHECK(t > 20000 && t < 60000 && s.d[A].tx_count - tx1 >= LINK_TRIES,
          "out of range: all %d copies sent, failed after %lld ms", LINK_TRIES, (long long)t);
    t = sim_until(&s, B, LEV_LINK_LOST, ANY, 240000);
    CHECK(t >= 0, "B: lost within the SF12 lost time (%lld ms)", (long long)t);
}

/* The point test (v0.6.1): its messages go even while the link is lost
 * (link_send_test_msg); the user's still don't. An ended link refuses both. */
static void test_send_while_lost(void)
{
    printf("point test: sending while the link is lost\n");
    sim_t s;
    sim_init(&s, 2, IDS, 46);
    CHECK(sim_connect(&s, A, B), "connected");
    sim_run(&s, 40000);
    set_range(&s, A, B, false);
    link_send_msg(&s.d[A].L, s.d[A].L.peer, "FIRST", s.now);
    CHECK(sim_until(&s, A, LEV_LINK_LOST, ANY, 40000) >= 0 && s.d[A].L.state == LS_LOST, "a failed message: lost");
    int l0 = s.d[A].n_log;
    bool refused = !link_send_msg(&s.d[A].L, s.d[A].L.peer, "USER", s.now);
    sim_run(&s, 100); /* (the engine's events reach the log as the world moves on) */
    CHECK(refused && count_ev(&s, A, l0, LEV_MSG_FAILED, ANY, 1) == 1,
          "the user's message is refused while lost (LEV_MSG_FAILED, just this one)");
    CHECK(link_send_test_msg(&s.d[A].L, s.d[A].L.peer, "TEST ONE", s.now) && s.d[A].L.n_msg == 1,
          "a point test's message is queued while lost");
    int tx0 = s.d[A].tx_count;
    l0 = s.d[A].n_log;
    sim_run(&s, 30000);
    CHECK(count_ev(&s, A, l0, LEV_MSG_FAILED, ANY, 0) == 1 && s.d[A].tx_count - tx0 >= LINK_TRIES,
          "out of range it is really sent (%d copies) and then fails", s.d[A].tx_count - tx0);
    set_range(&s, A, B, true);
    l0 = s.d[A].n_log;
    CHECK(link_send_test_msg(&s.d[A].L, s.d[A].L.peer, "TEST TWO", s.now), "queued again, still lost");
    CHECK(sim_until(&s, A, LEV_MSG_SENT, ANY, 30000) >= 0, "back in range: delivered with its receipt");
    CHECK(count_ev(&s, B, 0, LEV_MSG, ANY, -1) >= 1, "B got it");
    sim_run(&s, 60000);
    CHECK(s.d[A].L.state == LS_UP, "and the link comes back as usual");
    /* B links to someone else: A's link ends; a point test can't send either. */
    s.d[A].L.state = LS_ENDED;
    CHECK(!link_send_test_msg(&s.d[A].L, s.d[A].L.peer, "TEST THREE", s.now), "an ended link refuses it");
}

/* The point test's gaps (ttest.c) must be longer than a 25-character
 * message takes to fail, on the simulated channel with the real retry
 * timing, at every SF: otherwise the next message would wait behind a
 * failing one and fail with it. */
static void test_point_gaps(void)
{
    printf("point test: each gap outlasts a failing message\n");
    for (int sf = 9; sf <= 12; sf++) {
        sim_t s;
        sim_init(&s, 2, IDS, 47 + (uint64_t)sf);
        sim_set_sf(&s, sf);
        CHECK(sim_connect(&s, A, B), "SF%d: connected", sf);
        sim_run(&s, 20000);
        set_range(&s, A, B, false);
        int64_t worst = 0;
        for (int k = 0; k < 6; k++) {
            char text[TT_SIZE_MAX + 1];
            tt_text(text, sizeof text, (uint16_t)(k + 1), 'L', TT_POINT_SIZE);
            link_send_test_msg(&s.d[A].L, s.d[A].L.peer, text, s.now);
            int64_t t = sim_until(&s, A, LEV_MSG_FAILED, ANY, 120000);
            if (t > worst) {
                worst = t;
            }
            sim_run(&s, 1000);
        }
        printf("  SF%d: a failing message gives up after %.1f s at most; the gap is %ld s\n", sf, worst / 1000.0,
               (long)(tt_point_gap_ms(sf) / 1000));
        CHECK(worst > 0 && worst + 2000 < tt_point_gap_ms(sf),
              "SF%d: a failing 25-character message gives up after %lld ms at most, the gap is %ld ms", sf,
              (long long)worst, (long)tt_point_gap_ms(sf));
    }
}

/* ------------------------------------------------------------------------- */
/* Soak: random use for hours, then check the invariants                      */
/* ------------------------------------------------------------------------- */

#define SOAK_TEXTS 8192

static struct {
    uint16_t delivered_times[SOAK_TEXTS];  /* how often text "M<n>" was delivered */
    int delivered[SIM_MAX][SIM_MAX];       /* [receiver][sender] */
    int receipted[SIM_MAX][SIM_MAX];       /* [sender][recipient] MSG_SENT events */
    int64_t rebooted_at[SIM_MAX];
    int dupes;           /* delivered twice with no excuse */
    int reboot_dupes;    /* delivered twice because the receiver restarted in between */
    int failed;
    int results[4];
    int disc_done;       /* sweeps that reported (LEV_DISC_DONE) */
    int disc_found;      /* devices those sweeps found, in all */
} g_soak;

/* A receiver that restarts a moment after a message arrived, before its
 * receipt got through, gets the sender's next copy and (its memory of
 * recent packets being gone) plays it again. That is accepted: it needs a
 * restart within the ~10 s retry window. */
#define SOAK_REBOOT_EXCUSE_MS 20000

/* dev_index() -- which simulated device has this address (-1: none). */
static int dev_index(const sim_t *s, uint32_t addr)
{
    for (int i = 0; i < s->n; i++) {
        if (s->d[i].addr == addr) {
            return i;
        }
    }
    return -1;
}

/* soak_event() -- the soak's observer: counts deliveries (and duplicates),
 * receipts, failures and request results. */
static void soak_event(sim_t *s, int dev, const link_evt_t *e)
{
    int other = dev_index(s, e->addr);
    switch (e->kind) {
    case LEV_MSG:
        if (e->text[0] == 'M') {
            int n = atoi(e->text + 1);
            if (n >= 0 && n < SOAK_TEXTS) {
                if (g_soak.delivered_times[n]++ > 0) {
                    if (s->now - g_soak.rebooted_at[dev] < SOAK_REBOOT_EXCUSE_MS) {
                        g_soak.reboot_dupes++;
                    } else {
                        g_soak.dupes++;
                    }
                }
            }
        }
        if (other >= 0) {
            g_soak.delivered[dev][other]++;
        }
        break;
    case LEV_MSG_SENT:
        if (other >= 0) {
            g_soak.receipted[dev][other]++;
        }
        break;
    case LEV_MSG_FAILED:
        g_soak.failed++;
        break;
    case LEV_REQ_RESULT:
        g_soak.results[e->arg & 3]++;
        break;
    case LEV_DISC_DONE:
        g_soak.disc_done++;
        g_soak.disc_found += e->arg;
        break;
    default:
        break;
    }
}

/*
 * test_soak_seed() -- four devices used at random for `hours`: messages,
 * requests answered at random, unpairs, SOS, restarts, 10 % loss and people
 * walking out of range. Then a quiet quarter of an hour in full range, and
 * the invariants: nothing delivered twice (except right after a restart),
 * no receipt without a delivery, every "connected" link agreed by both
 * ends, nothing stuck, no event lost.
 */
static void test_soak_seed(uint64_t seed, int hours)
{
    static sim_t s; /* large: keep it off the stack */
    memset(&g_soak, 0, sizeof(g_soak));
    for (int i = 0; i < SIM_MAX; i++) {
        g_soak.rebooted_at[i] = -SOAK_REBOOT_EXCUSE_MS;
    }
    sim_init(&s, 4, IDS, seed);
    s.loss = 0.10;
    s.random_policy = true;
    s.on_event = soak_event;
    for (int i = 0; i < s.n; i++) {
        s.d[i].ack_sos = true;
    }
    struct {
        int i, j;
        int64_t until;
    } cuts[8];
    int n_cuts = 0, texts = 0, reboots = 0, unpairs = 0, requests = 0, soses = 0, sweeps = 0;
    int64_t next_act[SIM_MAX];
    for (int i = 0; i < s.n; i++) {
        next_act[i] = s.now + 2000 + (int64_t)(sim_rand(&s) * 20000);
    }
    int64_t end = s.now + (int64_t)hours * 3600000;
    while (s.now < end) {
        sim_step(&s);
        /* Range: every couple of minutes someone walks out of range of
         * someone else for 30 s .. 20 min. */
        if (n_cuts < 8 && sim_rand(&s) < SIM_STEP_MS / 120000.0) {
            int i = (int)(sim_rand(&s) * s.n), j = (int)(sim_rand(&s) * s.n);
            if (i != j && s.hear[i][j]) {
                set_range(&s, i, j, false);
                cuts[n_cuts].i = i, cuts[n_cuts].j = j;
                cuts[n_cuts].until = s.now + 30000 + (int64_t)(sim_rand(&s) * 19.5 * 60000);
                n_cuts++;
            }
        }
        for (int k = 0; k < n_cuts;) {
            if (s.now >= cuts[k].until) {
                set_range(&s, cuts[k].i, cuts[k].j, true);
                cuts[k] = cuts[--n_cuts];
            } else {
                k++;
            }
        }
        /* The users. */
        for (int i = 0; i < s.n; i++) {
            if (s.now < next_act[i]) {
                continue;
            }
            next_act[i] = s.now + 5000 + (int64_t)(sim_rand(&s) * 55000);
            link_t *L = &s.d[i].L;
            double r = sim_rand(&s);
            int j = (int)(sim_rand(&s) * s.n);
            if (r < 0.45) {
                if (L->peer != ADDR_NONE && L->state == LS_UP && texts < SOAK_TEXTS) {
                    char t[16];
                    snprintf(t, sizeof t, "M%d", texts++);
                    link_send_msg(L, L->peer, t, s.now);
                }
            } else if (r < 0.60) {
                if (j != i && s.d[j].addr != L->peer) {
                    link_request(L, s.d[j].addr, s.now);
                    requests++;
                }
            } else if (r < 0.62) {
                if (j != i) {
                    link_unpair(L, s.d[j].addr, s.now);
                    unpairs++;
                }
            } else if (r < 0.63) {
                link_send_sos(L, ADDR_BROADCAST, s.now);
                soses++;
            } else if (r < 0.64) {
                link_send_sos_cancel(L, ADDR_BROADCAST, s.now);
            } else if (r < 0.66) {
                /* Restart: the UI restores the stored link unless it had ended. */
                uint32_t keep = (L->state == LS_UP || L->state == LS_LOST) ? L->peer : ADDR_NONE;
                sim_reboot(&s, i, keep);
                g_soak.rebooted_at[i] = s.now;
                reboots++;
            } else if (r < 0.69) {
                link_discover(L, s.now); /* D: a sweep among everything else going on */
                sweeps++;
            }
        }
    }
    /* Quiet: everyone in range, no loss, nobody touches anything. */
    for (int k = 0; k < n_cuts; k++) {
        set_range(&s, cuts[k].i, cuts[k].j, true);
    }
    s.loss = 0;
    s.random_policy = false;
    s.n_act = 0;
    sim_run(&s, 15 * 60000);

    int dupes = g_soak.dupes, asym = 0, stuck = 0, receipt_without_delivery = 0, dropped = 0;
    for (int i = 0; i < s.n; i++) {
        const link_t *L = &s.d[i].L;
        if (L->state == LS_UP) {
            int j = dev_index(&s, L->peer);
            if (j < 0 || s.d[j].L.peer != L->own || s.d[j].L.state != LS_UP) {
                asym++;
                char a[5], b[5];
                addr_to_id(L->own, a);
                addr_to_id(L->peer, b);
                printf("    seed %llu: %s is linked to %s, which has peer %06x state %s\n", (unsigned long long)seed, a, b,
                       j >= 0 ? (unsigned)s.d[j].L.peer : 0u, j >= 0 ? link_state_name(s.d[j].L.state) : "-");
            }
        }
        stuck += L->n_msg + L->acc_active + L->msg_inflight + (L->disc_phase != DISC_IDLE);
        dropped += (int)L->n_ev_dropped;
        for (int j = 0; j < s.n; j++) {
            if (g_soak.receipted[i][j] > g_soak.delivered[j][i]) {
                receipt_without_delivery++;
            }
        }
    }
    int links = 0;
    for (int i = 0; i < s.n; i++) {
        links += s.d[i].L.state == LS_UP;
    }
    printf("  seed %llu, %d h: %d msgs (%d failed), %d requests (acc %d busy %d no %d unavail %d), %d unpairs, %d SOS, "
           "%d sweeps (%d reported, %d found), %d reboots (%d replays); %d collisions; at the end %d devices linked\n",
           (unsigned long long)seed, hours, texts, g_soak.failed, requests, g_soak.results[0], g_soak.results[1],
           g_soak.results[2], g_soak.results[3], unpairs, soses, sweeps, g_soak.disc_done, g_soak.disc_found, reboots,
           g_soak.reboot_dupes, s.collisions, links);
    CHECK(dupes == 0, "seed %llu: %d texts delivered twice", (unsigned long long)seed, dupes);
    CHECK(receipt_without_delivery == 0, "seed %llu: receipt without delivery", (unsigned long long)seed);
    CHECK(asym == 0, "seed %llu: %d one-sided links after the quiet period", (unsigned long long)seed, asym);
    CHECK(stuck == 0, "seed %llu: %d exchanges stuck", (unsigned long long)seed, stuck);
    CHECK(dropped == 0, "seed %llu: %d events dropped", (unsigned long long)seed, dropped);
}

/* test_soak() -- the soak over several seeds (SOAK_SEEDS=n for more). */
static void test_soak(void)
{
    /* SOAK_SEEDS=n runs more of them (each is 4 simulated hours, ~1 s). */
    const char *env = getenv("SOAK_SEEDS");
    int seeds = env ? atoi(env) : 6;
    printf("link: soak (4 devices, random use and sweeps, 10 %% loss, range changes, restarts)\n");
    for (uint64_t seed = 1; seed <= (uint64_t)seeds; seed++) {
        test_soak_seed(seed, 4);
    }
}

/* test_link_all() -- every packet-layer and link test, called from main(). */
void test_link_all(void)
{
    test_proto();
    test_connect_and_chat();
    test_lost_restored();
    test_unavailable_declined();
    test_busy_queue();
    test_late_accept_unwanted();
    test_message_failure();
    test_lossy_channel();
    test_reboot_msgids();
    test_mutual_and_switch();
    test_missed_end_notice();
    test_accept_confirm_lost();
    test_pending_limits();
    test_unpair();
    test_sos_relay();
    test_duty_cycle();
    test_ping();
    test_disc_list();
    test_disc_limit();
    test_disc_sweep();
    test_disc_silent_blocked();
    test_disc_limits_on_air();
    test_disc_edges();
    test_disc_two_at_once();
    test_disc_invite();
    test_relay_out_of_range();
    test_relay_both_paths();
    test_relay_one_phone();
    test_relay_connect_sos();
    test_relay_no_discovery();
    test_timing();
    test_fail_then_silence();
    test_one_way();
    test_one_way_with_relay();
    test_relay_drop();
    test_reports_by_hand();
    test_sf12();
    test_send_while_lost();
    test_point_gaps();
    test_soak();
}
