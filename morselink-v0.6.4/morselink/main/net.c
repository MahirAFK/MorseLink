/*
 * MorseLink — network task: the link engine, the LoRa radio and the loopback
 * test network (see net.h).
 *
 * One FreeRTOS task ("net", core 0) owns all of it, so none of the engine or
 * radio code needs locks. It sleeps until one of these wakes it:
 *   - NB_RADIO: the radio's DIO0 interrupt (a packet was sent or received);
 *   - NB_CMD:   a command from the UI or console (net_* functions below);
 *   - a timeout: the engine's next deadline, at most 200 ms away.
 * Each time round it: runs the queued commands, services the radio, runs
 * the engine's timers, hands the engine's events to the UI, and if the
 * radio is free, transmits the next packet the engine has due.
 *
 * The phone relay (phone.h): whenever the engine hands out a packet to
 * transmit, a copy also goes to the phone (if one is ready) for each topic
 * phone_relay_targets() names; packets coming back from the phone are fed
 * to the engine marked "via phone" (link_on_rx_via). The relay works the
 * same with the radio or with loopback.
 *
 * Test mode (ntest.c, docs/TEST_PLAN.md) runs inside this task too: it is
 * told about every packet sent and received and every engine event, it
 * makes the test traffic, and while it runs the mirroring to the phone
 * stops and packets from the phone go to it instead of the engine.
 *
 * Loopback (one board): the engine's packets go to a second engine, "the
 * pretend partner", instead of the radio, after a delay equal to their
 * airtime; its packets come back the same way. A tiny auto-responder plays
 * the partner's user: it accepts requests, echoes messages and acknowledges
 * SOS. The partner takes on whatever ID the device talks to, so `S K7Q`
 * reaches "K7Q", and it follows the device's link when that changes. Our
 * sweeps (D) are answered by the partner (heard strongly: near) and by two
 * more pretend devices further away (LB_EXTRA: mid and far), so the whole
 * discovery list can be tried on one board.
 */
#include "net.h"

#include <stdio.h>
#include <string.h>

#include "ble.h"
#include "board.h"
#include "config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "morse.h"
#include "ntest.h"
#include "phone.h"
#include "radio.h"
#include "store.h"
#include "ttest.h" /* TT_ARGS_MAX */
#include "ui.h"

static const char *TAG = "net";

#define NB_RADIO (1u << 0) /* notification bit: DIO0 interrupt */
#define NB_CMD (1u << 1)   /* notification bit: a command was queued */

#define CMDQ_LEN 12
#define MAX_SLEEP_MS 200          /* wake at least this often */
/* (the longest we hold a packet back for someone else's grows with the
 * airtime: the engine's tm.lbt_max_ms, 1.5 s at SF9) */
#define TX_TIMEOUT_EXTRA_MS 500   /* TX_DONE later than airtime + this: the radio is stuck */
#define HEALTH_CHECK_MS 10000     /* check the radio is still listening */
#define LB_N 8                    /* loopback packets in flight */
#define ACT_N 6                   /* the pretend partner's pending actions */
#define LB_PARTNER_RSSI (-40)     /* loopback: how strongly the pretend partner is "heard" (near) */

/* ------------------------------------------------------------------------- */
/* Commands from other tasks                                                  */
/* ------------------------------------------------------------------------- */

typedef enum {
    NC_IDENTITY = 0,
    NC_LOOPBACK,
    NC_LINK,
    NC_BLOCKLIST,
    NC_POWER,
    NC_REQUEST,
    NC_ACCEPT,
    NC_DECLINE,
    NC_UNPAIR,
    NC_MSG,
    NC_SOS,
    NC_SOS_CANCEL,
    NC_SOS_ACK,
    NC_PING,
    NC_LOG,
    NC_SIM_REQUEST,
    NC_SIM_DEAF,
    NC_DISCOVER,
    NC_DISCOVERABLE,
    NC_SIM_DISCOVER,
    NC_CONTACTS,
    NC_PHONE_RX,
    NC_TEST_START,   /* text: the parameters; arg: 1 = from the phone */
    NC_TEST_REFUSE,  /* text: why the UI refused a start */
    NC_TEST_STOP,    /* text: why */
    NC_TEST_BATTERY, /* addr: mV (or -1); arg: percentage (or -1) | charging << 8 */
    NC_SF,           /* arg: the new spreading factor */
    NC_BENCH,        /* arg: net_bench_t; addr: dBm for the carrier */
} net_cmd_op_t;

typedef struct {
    uint8_t op;      /* net_cmd_op_t */
    uint32_t addr;   /* the device concerned */
    int32_t arg;     /* on/off, power, list or packet length */
    union {
        char text[MSG_MAX_CHARS + 1];
        char args[TT_ARGS_MAX + 1];  /* NC_TEST_START: the parameters (a point test's plan makes them long) */
        uint32_t list[BLOCK_MAX];    /* blocklist or contacts (PEER_SLOTS <= BLOCK_MAX) */
        uint8_t pkt[PROTO_MAX_PKT];  /* NC_PHONE_RX: the packet */
    };
} net_cmd_t;

/* ------------------------------------------------------------------------- */
/* Loopback                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool used;
    bool to_sim;    /* towards the pretend partner (else towards us) */
    int64_t at;     /* arrives at this time */
    int16_t rssi;   /* towards us: how strongly it is "heard", dBm */
    uint8_t len;
    uint8_t pkt[PROTO_MAX_PKT];
} lb_pkt_t;

/* Loopback: more pretend devices, which only answer our sweeps (D), always,
 * from a random slot like real ones. Heard at these strengths, they sort as
 * mid (22 + 78 = 100 dB path loss) and far (22 + 108 = 130 dB); the pretend
 * partner, at LB_PARTNER_RSSI, is near. Inviting one works like `S` with
 * any ID: the pretend partner becomes that device and accepts. */
static const struct {
    const char *id;
    int16_t rssi;
} LB_EXTRA[] = {{"M4X", -78}, {"F9Z", -108}};

typedef enum { ACT_ACCEPT = 1, ACT_ECHO, ACT_SOS_ACK } act_kind_t;

typedef struct {
    uint8_t what;   /* act_kind_t; 0 = free */
    int64_t at;
    uint32_t addr;
    char text[MSG_MAX_CHARS + 1];
} sim_act_t;

/* ------------------------------------------------------------------------- */
/* State (owned by the net task, except `st` and the flags noted)            */
/* ------------------------------------------------------------------------- */

static struct {
    TaskHandle_t task;
    QueueHandle_t cmdq;
    SemaphoreHandle_t lock; /* protects st */
    net_status_t st;        /* snapshot for net_get_status() */

    link_t eng;             /* this device's protocol engine */
    lora_params_t lp;
    uint32_t own;
    int tx_dbm;
    int want_dbm;           /* power to apply once the radio is free (-1: none) */
    int want_sf;            /* spreading factor to apply once the radio is free (-1: none) */
    int bench;              /* net_bench_t: bench power holds the radio */
    int want_bench;         /* ... the next hold, once the radio is free (-1: none) */
    int want_bench_dbm;
    int64_t carrier_since;  /* bench carrier on since (0: off): its airtime counts */
    uint32_t blocked[BLOCK_MAX];
    uint8_t n_blocked;
    uint32_t contacts[PEER_SLOTS]; /* for a general SOS through the phone */
    uint8_t n_contacts;
    uint32_t phone_out;     /* packet copies handed to the phone */
    volatile bool radio_ok; /* read by other tasks (net_has_transport) */
    volatile bool loopback; /* idem */
    bool discoverable;      /* silent mode off: answer sweeps (kept across engine restarts) */
    bool log;

    /* radio transmit in progress */
    bool txing;
    int64_t tx_deadline;
    int64_t lbt_since;      /* holding back for someone else's packet since (0: not) */
    int64_t next_check;

    /* loopback */
    link_t sim;             /* the pretend partner */
    bool sim_valid;
    bool sim_deaf;          /* the pretend partner is "out of range": packets both ways are lost */
    lb_pkt_t lb[LB_N];
    sim_act_t act[ACT_N];
    bool eng_lb_tx, sim_lb_tx;      /* each "on air" until its end time */
    int64_t eng_lb_end, sim_lb_end;
    uint32_t last_peer;     /* to notice when our link changes */
} N;

/* now_ms() -- milliseconds since boot (the engine's clock). */
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* id_or() -- printable ID for an address ("ALL", "-" or "?" when not an ID). */
static const char *id_or(uint32_t a, char buf[ID_MAX_LEN + 1])
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

/* airtime_ms() -- airtime of a packet of `len` bytes, rounded up. */
static uint32_t airtime_ms(int len, bool long_preamble)
{
    return (lora_airtime_us(len, &N.lp, long_preamble ? RADIO_PREAMBLE_LONG : RADIO_PREAMBLE_SHORT) + 999) / 1000;
}

/* ------------------------------------------------------------------------- */
/* Logging                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * log_pkt() -- one line per packet when `radio log on` (or the test setup):
 *   tx MSG K7Q->ABC #4211 "HELLO"  15 B, 165 ms
 *   rx RECEIPT ABC->K7Q #977  -61 dBm, SNR 9.5 dB
 * `rx` is NULL for packets we send.
 */
static void log_pkt(const char *dir, const uint8_t *buf, int len, const radio_rx_t *rx)
{
    if (!N.log) {
        return;
    }
    pkt_t p;
    proto_err_t err = proto_decode(buf, len, &p);
    if (err != PROTO_OK) {
        ESP_LOGI(TAG, "%s %d bytes, not ours (%s)", dir, len, proto_err_str(err));
        return;
    }
    char s[ID_MAX_LEN + 1], d[ID_MAX_LEN + 1], extra[48] = "";
    if (p.type == PT_LINK_REPLY) {
        snprintf(extra, sizeof extra, " %s", proto_reply_name(p.payload[0]));
    } else if (p.type == PT_MSG) {
        snprintf(extra, sizeof extra, " \"%.*s\"", p.plen > 32 ? 32 : p.plen, (const char *)p.payload);
    }
    if (p.flags & PF_HOPS_MASK) {
        size_t n = strlen(extra);
        snprintf(extra + n, sizeof extra - n, " (relay %d)", p.flags & PF_HOPS_MASK);
    }
    if (rx) {
        ESP_LOGI(TAG, "%s %s %s->%s #%u%s  %d dBm, SNR %.1f dB", dir, proto_type_name(p.type), id_or(p.src, s),
                 id_or(p.dst, d), p.id, extra, rx->rssi, rx->snr_x4 / 4.0);
    } else {
        ESP_LOGI(TAG, "%s %s %s->%s #%u%s  %d B, %lu ms", dir, proto_type_name(p.type), id_or(p.src, s),
                 id_or(p.dst, d), p.id, extra, len,
                 (unsigned long)airtime_ms(len, (p.flags & PF_LONG_PREAMBLE) != 0));
    }
}

/* ------------------------------------------------------------------------- */
/* The phone relay                                                            */
/* ------------------------------------------------------------------------- */

/* log_phone_pkt() -- with `radio log on`, one line per packet through the
 * phone, arrows as in ble.c ("phone << ..." goes to the phone, "phone >> ..."
 * comes from it): "phone << MSG R2D->K7Q #4211 (topic K7Q)" or
 * "phone >> RECEIPT K7Q->R2D #977". */
static void log_phone_pkt(bool out, const uint8_t *buf, int len, uint32_t topic)
{
    if (!N.log) {
        return;
    }
    pkt_t p;
    char s[ID_MAX_LEN + 1], d[ID_MAX_LEN + 1], t[ID_MAX_LEN + 1];
    if (proto_decode(buf, len, &p) != PROTO_OK) {
        ESP_LOGI(TAG, "phone %s %d bytes, not ours", out ? "<<" : ">>", len);
        return;
    }
    if (out) {
        ESP_LOGI(TAG, "phone << %s %s->%s #%u (topic %s)", proto_type_name(p.type), id_or(p.src, s), id_or(p.dst, d),
                 p.id, id_or(topic, t));
    } else {
        ESP_LOGI(TAG, "phone >> %s %s->%s #%u", proto_type_name(p.type), id_or(p.src, s), id_or(p.dst, d), p.id);
    }
}

/*
 * relay_to_phone() -- a packet the engine is transmitting also goes to the
 * phone, once for each topic phone_relay_targets() names: the device it is
 * for, or, for a general SOS, the partner and every contact. Nothing if no
 * phone is ready. Called for every copy the engine sends (retries too: the
 * receiver acts once).
 */
static void relay_to_phone(const uint8_t *pkt, int len)
{
    if (!ble_phone_ready() || ntest_active()) {
        return; /* (during a test, each path carries its own copies: ntest.c) */
    }
    uint32_t to[PHONE_RELAY_MAX];
    int n = phone_relay_targets(pkt, len, N.own, N.contacts, N.n_contacts, N.eng.peer, to, PHONE_RELAY_MAX);
    static char line[PHONE_LINE_MAX + 1]; /* (net task only) */
    for (int i = 0; i < n; i++) {
        if (phone_fmt_relay(line, sizeof line, to[i], pkt, len) > 0) {
            log_phone_pkt(true, pkt, len, to[i]);
            ble_send_line(line);
            N.phone_out++;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Engines                                                                    */
/* ------------------------------------------------------------------------- */

/* save_msgid() -- the engine reserved a new block of message IDs: store
 * where the next one starts (once every MSGID_BLOCK packets). */
static void save_msgid(void *ctx, uint16_t next_block)
{
    (void)ctx;
    store_save_msgid(next_block);
}

/*
 * engine_start() -- (re)create this device's engine for address `own`,
 * resuming message IDs from flash. The link is set separately afterwards
 * (net_set_link), since the UI owns the stored link.
 */
static void engine_start(uint32_t own, int64_t now)
{
    uint16_t stored;
    if (!store_load_msgid(&stored)) {
        stored = (uint16_t)esp_random(); /* first boot: anywhere, but never twice the same */
    }
    /* Without an ID nothing is sent, so don't reserve (and write) a block. */
    link_init(&N.eng, own, &N.lp, esp_random(), stored, own == ADDR_NONE ? NULL : save_msgid, NULL, now);
    link_set_tx_dbm(&N.eng, (int8_t)N.tx_dbm);
    link_set_blocklist(&N.eng, N.blocked, N.n_blocked);
    link_set_discoverable(&N.eng, N.discoverable);
    N.own = own;
    N.txing = false;
    N.eng_lb_tx = false;
    N.last_peer = ADDR_NONE;
}

/*
 * sim_become() -- the pretend partner takes the ID `addr`. With `linked`,
 * it starts already linked to us (it follows our link); otherwise it starts
 * fresh, e.g. to receive our request. Its pending actions are dropped.
 */
static void sim_become(uint32_t addr, bool linked, int64_t now)
{
    link_init(&N.sim, addr, &N.lp, esp_random(), (uint16_t)esp_random(), NULL, NULL, now);
    link_set_tx_dbm(&N.sim, (int8_t)N.tx_dbm);
    if (linked && N.own != ADDR_NONE) {
        link_set_peer(&N.sim, N.own, now);
    }
    N.sim_valid = true;
    N.sim_lb_tx = false;
    memset(N.act, 0, sizeof(N.act));
    char id[ID_MAX_LEN + 1];
    ESP_LOGI(TAG, "loopback: the pretend partner is now %s%s", id_or(addr, id), linked ? " (linked to us)" : "");
}

/* sim_schedule() -- the pretend partner's user will do `what` at `at`. */
static void sim_schedule(uint8_t what, uint32_t addr, const char *text, int64_t at)
{
    for (int i = 0; i < ACT_N; i++) {
        if (!N.act[i].what) {
            N.act[i].what = what;
            N.act[i].addr = addr;
            N.act[i].at = at;
            strncpy(N.act[i].text, text ? text : "", MSG_MAX_CHARS);
            N.act[i].text[MSG_MAX_CHARS] = '\0';
            return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Loopback channel                                                           */
/* ------------------------------------------------------------------------- */

/* lb_put() -- a packet leaves one engine; it reaches the other at `at`
 * (unless the partner is "out of range": `sim deaf on`). `rssi` is how
 * strongly we "hear" it when it comes towards us. */
static void lb_put(bool to_sim, const uint8_t *pkt, int len, int64_t at, int16_t rssi)
{
    if (N.sim_deaf) {
        return;
    }
    for (int i = 0; i < LB_N; i++) {
        if (!N.lb[i].used) {
            N.lb[i].used = true;
            N.lb[i].to_sim = to_sim;
            N.lb[i].at = at;
            N.lb[i].rssi = rssi;
            N.lb[i].len = (uint8_t)len;
            memcpy(N.lb[i].pkt, pkt, (size_t)len);
            return;
        }
    }
    /* full: the packet is lost, as on a real channel; retries cover it */
}

/*
 * lb_extra_answer() -- loopback: our DISCOVER `d` has just "ended": each of
 * the LB_EXTRA devices answers PRESENCE in a random slot of its window,
 * exactly as a real device would time it (link.c, on_discover), quoting the
 * default transmit power and the sweep's tag. One whose ID the pretend
 * partner has taken (we invited it) is the partner now: it answers through
 * the partner's engine instead, so it is never listed twice.
 */
static void lb_extra_answer(const pkt_t *d, int64_t now)
{
    uint32_t slots = d->payload[0] ? d->payload[0] : 1;
    uint32_t slot_ms = (uint32_t)d->payload[1] * 10;
    for (size_t k = 0; k < sizeof LB_EXTRA / sizeof LB_EXTRA[0]; k++) {
        if (N.sim_valid && id_to_addr(LB_EXTRA[k].id) == N.sim.own) {
            continue;
        }
        pkt_t q;
        proto_init_pkt(&q, PT_PRESENCE, id_to_addr(LB_EXTRA[k].id), d->src, (uint16_t)(esp_random() | 1));
        q.payload[0] = RADIO_TX_DBM_DEFAULT;
        q.payload[1] = (uint8_t)d->id; /* the sweep's tag */
        q.plen = DISC_PRESENCE_LEN;
        uint8_t buf[PROTO_MAX_PKT];
        int len = proto_encode(&q, buf, sizeof buf);
        if (len > 0) {
            int64_t start = now + DISC_TURN_MS + (int64_t)(esp_random() % slots) * slot_ms;
            lb_put(false, buf, len, start + airtime_ms(len, false), LB_EXTRA[k].rssi);
        }
    }
}

/*
 * lb_deliver_to_sim() -- a packet of ours reaches the pretend partner if it
 * is for the partner's current ID, or for everyone. A link request for some
 * other ID makes the partner become that device first, so that `S <any ID>`
 * finds someone to answer. Anything else for another ID is lost (nobody
 * there). Our DISCOVER also reaches the LB_EXTRA devices.
 */
static void lb_deliver_to_sim(const uint8_t *pkt, int len, int64_t now)
{
    pkt_t p;
    if (proto_decode(pkt, len, &p) != PROTO_OK) {
        return;
    }
    if (p.type == PT_DISCOVER) {
        lb_extra_answer(&p, now);
    }
    if (!N.sim_valid || (p.dst != N.sim.own && p.dst != ADDR_BROADCAST)) {
        if (p.type != PT_LINK_REQ) {
            return;
        }
        sim_become(p.dst, false, now);
    }
    link_on_rx(&N.sim, pkt, len, LB_PARTNER_RSSI, 40, now); /* a strong, clean signal */
}

/* log_sim_sweep() -- loopback: what the pretend partner's sweep (`sim disc`)
 * found, i.e. whether this device answered it. */
static void log_sim_sweep(const link_evt_t *e)
{
    char id[ID_MAX_LEN + 1], who[ID_MAX_LEN + 1];
    id_or(N.sim.own, who);
    if (e->arg == 0) {
        ESP_LOGI(TAG, "loopback: the pretend partner %s's sweep found nobody", who);
        return;
    }
    for (int i = 0; i < e->arg && i < DISC_MAX; i++) {
        const disc_entry_t *d = &e->found[i];
        ESP_LOGI(TAG, "loopback: the pretend partner %s's sweep found %s, %s (path loss %d dB)", who, id_or(d->addr, id),
                 disc_prox_name(d->prox), disc_loss_db(d));
    }
}

/*
 * loopback_tick() -- move the loopback world forward: deliver packets whose
 * airtime is over, finish simulated transmissions, run the partner's
 * engine and its user's actions, and start its next transmission.
 */
static void loopback_tick(int64_t now)
{
    for (int i = 0; i < LB_N; i++) {
        if (!N.lb[i].used || N.lb[i].at > now) {
            continue;
        }
        /* Deliver from a copy: the slot is free from here on, and delivering
         * can put new packets on the channel (our DISCOVER makes the LB_EXTRA
         * devices answer), which may reuse this very slot. */
        lb_pkt_t b = N.lb[i];
        N.lb[i].used = false;
        if (b.to_sim) {
            lb_deliver_to_sim(b.pkt, b.len, now);
        } else {
            radio_rx_t rx = {.len = b.len, .rssi = b.rssi, .snr_x4 = 40};
            log_pkt("rx", b.pkt, b.len, &rx);
            ntest_on_rx(b.pkt, b.len, rx.rssi, rx.snr_x4, now);
            link_on_rx(&N.eng, b.pkt, b.len, rx.rssi, rx.snr_x4, now);
        }
    }
    if (N.eng_lb_tx && now >= N.eng_lb_end) {
        N.eng_lb_tx = false;
        link_tx_done(&N.eng, now, true);
    }
    if (!N.sim_valid) {
        return;
    }
    if (N.sim_lb_tx && now >= N.sim_lb_end) {
        N.sim_lb_tx = false;
        link_tx_done(&N.sim, now, true);
    }
    /* The partner follows our link when it moves (e.g. an SOS made it). */
    if (N.eng.peer != N.last_peer) {
        N.last_peer = N.eng.peer;
        if (N.eng.peer != ADDR_NONE && N.eng.peer != N.sim.own) {
            sim_become(N.eng.peer, true, now);
        }
    }
    link_poll(&N.sim, now);
    /* The partner's user: accept every request, echo every message,
     * acknowledge every SOS (after a human-like pause). Its sweeps (`sim
     * disc`) are only logged. */
    link_evt_t e;
    while (link_get_event(&N.sim, &e)) {
        if (e.kind == LEV_REQ_IN) {
            sim_schedule(ACT_ACCEPT, e.addr, NULL, now + LOOPBACK_ACCEPT_MS);
        } else if (e.kind == LEV_MSG) {
            sim_schedule(ACT_ECHO, e.addr, e.text, now + LOOPBACK_DELAY_MS);
        } else if (e.kind == LEV_SOS) {
            sim_schedule(ACT_SOS_ACK, e.addr, NULL, now + LOOPBACK_SOS_ACK_MS);
        } else if (e.kind == LEV_DISC_DONE) {
            log_sim_sweep(&e);
        }
    }
    for (int i = 0; i < ACT_N; i++) {
        sim_act_t *a = &N.act[i];
        if (!a->what || a->at > now) {
            continue;
        }
        if (a->what == ACT_ACCEPT) {
            link_accept(&N.sim, a->addr, now);
        } else if (a->what == ACT_ECHO) {
            link_send_msg(&N.sim, a->addr, a->text, now);
        } else {
            link_send_sos_ack(&N.sim, a->addr, now);
        }
        a->what = 0;
    }
    uint8_t pkt[PROTO_MAX_PKT];
    int len;
    bool lp;
    if (!N.sim_lb_tx && link_take_tx(&N.sim, now, pkt, &len, &lp)) {
        uint32_t air = airtime_ms(len, lp);
        lb_put(false, pkt, len, now + air, LB_PARTNER_RSSI);
        N.sim_lb_tx = true;
        N.sim_lb_end = now + air;
    }
}

/* ------------------------------------------------------------------------- */
/* Radio                                                                      */
/* ------------------------------------------------------------------------- */

/* service_radio() -- handle what the radio has to report (a few events at
 * most: one call per DIO0 rise normally finds exactly one). */
static void service_radio(int64_t now)
{
    uint8_t buf[PROTO_MAX_PKT + 8];
    radio_rx_t rx;
    for (int k = 0; k < 3; k++) {
        radio_ev_t ev = radio_service(buf, sizeof buf, &rx);
        if (ev == RADIO_EV_NONE) {
            return;
        }
        if (ev == RADIO_EV_TX_DONE) {
            N.txing = false;
            link_tx_done(&N.eng, now, true);
        } else if (ev == RADIO_EV_RX) {
            log_pkt("rx", buf, rx.len, &rx);
            if (!N.loopback) {
                ntest_on_rx(buf, rx.len, rx.rssi, rx.snr_x4, now);
                link_on_rx(&N.eng, buf, rx.len, rx.rssi, rx.snr_x4, now);
            }
        } else {
            if (N.log) {
                ESP_LOGI(TAG, "rx damaged packet (CRC error)");
            }
            if (!N.loopback) {
                ntest_on_rx_bad(now);
            }
        }
    }
}

/*
 * try_transmit() -- if the channel is ours, send the engine's next due
 * packet. Over the radio, we first hold back while someone else's packet
 * is in the air (it would be destroyed, and so would ours), but never for
 * longer than one longest packet (tm.lbt_max_ms: 1.5 s at SF9, 4.6 s at
 * SF12), in case the busy indication is stuck.
 */
static void try_transmit(int64_t now)
{
    uint8_t pkt[PROTO_MAX_PKT];
    int len;
    bool lp;
    if (N.loopback) {
        if (!N.eng_lb_tx && link_take_tx(&N.eng, now, pkt, &len, &lp)) {
            uint32_t air = airtime_ms(len, lp);
            log_pkt("tx", pkt, len, NULL);
            ntest_on_tx(pkt, len, air, 0, N.tx_dbm, now);
            relay_to_phone(pkt, len);
            lb_put(true, pkt, len, now + air, 0);
            N.eng_lb_tx = true;
            N.eng_lb_end = now + air;
        }
        return;
    }
    if (!N.radio_ok || N.txing || N.bench != NET_BENCH_OFF) {
        return; /* (bench power holds the radio: nothing goes out) */
    }
    if (radio_channel_busy()) {
        if (N.lbt_since == 0) {
            N.lbt_since = now;
        }
        if (now - N.lbt_since < (int64_t)N.eng.tm.lbt_max_ms) {
            return;
        }
    }
    int64_t lbt_ms = N.lbt_since ? now - N.lbt_since : 0; /* how long we held back for someone else */
    N.lbt_since = 0;
    if (!link_take_tx(&N.eng, now, pkt, &len, &lp)) {
        return;
    }
    log_pkt("tx", pkt, len, NULL);
    ntest_on_tx(pkt, len, airtime_ms(len, lp), lbt_ms, N.tx_dbm, now);
    relay_to_phone(pkt, len);
    if (radio_transmit(pkt, len, lp ? RADIO_PREAMBLE_LONG : RADIO_PREAMBLE_SHORT) != ESP_OK) {
        link_tx_done(&N.eng, now, false);
        return;
    }
    N.txing = true;
    N.tx_deadline = now + airtime_ms(len, lp) + TX_TIMEOUT_EXTRA_MS;
}

/* ------------------------------------------------------------------------- */
/* Events to the UI                                                           */
/* ------------------------------------------------------------------------- */

/* deliver_events() -- hand the engine's events to the UI. PONGs are a bench
 * tool: they are only logged. Test mode takes the ones that are its own
 * (ntest_on_event). */
static void deliver_events(int64_t now)
{
    link_evt_t e;
    char id[ID_MAX_LEN + 1];
    while (link_get_event(&N.eng, &e)) {
        if (e.kind == LEV_PONG) {
            ESP_LOGI(TAG, "PONG from %s: they hear us at %d dBm (SNR %.1f dB); we hear them at %d dBm (SNR %.1f dB); "
                          "their power %d dBm",
                     id_or(e.addr, id), e.far_rssi, e.far_snr_x4 / 4.0, e.rssi, e.snr_x4 / 4.0, e.far_dbm);
            continue;
        }
        if (N.log) {
            ESP_LOGI(TAG, "event %s %s arg=%d", link_event_name(e.kind), id_or(e.addr, id), e.arg);
        }
        if (ntest_on_event(&e, now)) {
            continue;
        }
        ui_post_net(&e);
    }
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                   */
/* ------------------------------------------------------------------------- */

/* run_command() -- apply one command from the queue to the engine. */
static void run_command(const net_cmd_t *c, int64_t now)
{
    switch ((net_cmd_op_t)c->op) {
    case NC_IDENTITY:
        engine_start(c->addr, now);
        if (N.loopback) {
            sim_become(id_to_addr("TST"), false, now);
        }
        break;
    case NC_LOOPBACK:
        N.loopback = c->arg != 0;
        memset(N.lb, 0, sizeof(N.lb)); /* packets in flight on the pretend channel are lost */
        if (N.eng_lb_tx) {
            link_tx_done(&N.eng, now, false); /* the engine must not wait for it forever */
        }
        N.eng_lb_tx = N.sim_lb_tx = false;
        /* (a real transmission still in progress finishes normally: its
         * TX_DONE is handled whatever the mode) */
        if (N.loopback) {
            /* The partner starts as our current link partner, or "TST". */
            uint32_t peer = N.eng.peer;
            sim_become(peer != ADDR_NONE ? peer : id_to_addr("TST"), peer != ADDR_NONE, now);
            N.last_peer = peer;
        } else {
            N.sim_valid = false;
        }
        ESP_LOGI(TAG, "loopback %s", N.loopback ? "on: the radio is not used" : "off");
        break;
    case NC_LINK:
        link_set_peer(&N.eng, c->addr, now);
        break;
    case NC_BLOCKLIST:
        N.n_blocked = (uint8_t)(c->arg > BLOCK_MAX ? BLOCK_MAX : c->arg);
        memcpy(N.blocked, c->list, N.n_blocked * sizeof(uint32_t));
        link_set_blocklist(&N.eng, N.blocked, N.n_blocked);
        break;
    case NC_POWER:
        N.want_dbm = c->arg; /* applied by apply_power() once the radio is free */
        break;
    case NC_REQUEST:
        link_request(&N.eng, c->addr, now);
        break;
    case NC_ACCEPT:
        link_accept(&N.eng, c->addr, now);
        break;
    case NC_DECLINE:
        link_decline(&N.eng, c->addr, now);
        break;
    case NC_UNPAIR:
        link_unpair(&N.eng, c->addr, now);
        break;
    case NC_MSG:
        link_send_msg(&N.eng, c->addr, c->text, now);
        break;
    case NC_SOS:
        link_send_sos(&N.eng, c->addr, now);
        break;
    case NC_SOS_CANCEL:
        link_send_sos_cancel(&N.eng, c->addr, now);
        break;
    case NC_SOS_ACK:
        link_send_sos_ack(&N.eng, c->addr, now);
        break;
    case NC_PING:
        link_ping(&N.eng, now);
        ESP_LOGI(TAG, "PING sent: answers appear as PONG lines within a couple of seconds");
        break;
    case NC_LOG:
        N.log = c->arg != 0;
        break;
    case NC_SIM_REQUEST:
        if (!N.loopback) {
            ESP_LOGW(TAG, "sim: turn loopback on first");
            break;
        }
        sim_become(c->addr, false, now);
        link_request(&N.sim, N.own, now);
        break;
    case NC_SIM_DEAF:
        N.sim_deaf = c->arg != 0;
        ESP_LOGI(TAG, "loopback: the pretend partner is %s", N.sim_deaf ? "out of range" : "back in range");
        break;
    case NC_DISCOVER:
        link_discover(&N.eng, now);
        break;
    case NC_DISCOVERABLE:
        N.discoverable = c->arg != 0;
        link_set_discoverable(&N.eng, N.discoverable);
        break;
    case NC_SIM_DISCOVER:
        if (!N.loopback || !N.sim_valid) {
            ESP_LOGW(TAG, "sim: turn loopback on first");
            break;
        }
        link_discover(&N.sim, now);
        ESP_LOGI(TAG, "loopback: the pretend partner sweeps (D); its result follows in about 4 s");
        break;
    case NC_CONTACTS:
        N.n_contacts = (uint8_t)(c->arg > PEER_SLOTS ? PEER_SLOTS : c->arg);
        memcpy(N.contacts, c->list, N.n_contacts * sizeof(uint32_t));
        break;
    case NC_PHONE_RX:
        log_phone_pkt(false, c->pkt, c->arg, ADDR_NONE);
        if (!ntest_on_phone_rx(c->pkt, c->arg, now)) {
            link_on_rx_via(&N.eng, c->pkt, c->arg, 0, 0, true, now); /* no radio readings: it came by phone */
        }
        break;
    case NC_TEST_START:
        ntest_start(c->args, c->arg != 0, N.loopback, N.tx_dbm, N.lp.sf, now);
        break;
    case NC_TEST_REFUSE:
        ntest_refuse(c->text, now);
        break;
    case NC_TEST_STOP:
        ntest_stop(c->text, now);
        break;
    case NC_TEST_BATTERY:
        ntest_battery(c->addr == ADDR_NONE ? -1 : (int)c->addr, (int8_t)(c->arg & 0xFF), (c->arg >> 8) != 0);
        break;
    case NC_SF:
        N.want_sf = c->arg; /* applied by apply_sf() once the radio is free */
        break;
    case NC_BENCH:
        N.want_bench = c->arg; /* applied by apply_bench() once the radio is free */
        N.want_bench_dbm = (int)c->addr;
        break;
    }
}

/* apply_power() -- a requested power change, once no packet is on air (the
 * PA registers can only be written in standby). The chip clamps it to what
 * it can do (20 dBm on an SX1276); what it took is what the engine quotes
 * in PING / PRESENCE and test mode logs. */
static void apply_power(void)
{
    if (N.want_dbm < 0 || N.txing) {
        return;
    }
    int max = radio_tx_dbm_max();
    int dbm = N.want_dbm < RADIO_TX_DBM_MIN ? RADIO_TX_DBM_MIN : N.want_dbm > max ? max : N.want_dbm;
    N.want_dbm = -1;
    radio_set_tx_power(dbm);
    N.tx_dbm = dbm;
    link_set_tx_dbm(&N.eng, (int8_t)dbm);
    if (N.sim_valid) {
        link_set_tx_dbm(&N.sim, (int8_t)dbm);
    }
    ESP_LOGI(TAG, "transmit power %d dBm", dbm);
}

/*
 * apply_bench() -- bench power's next radio state, once no packet is on air.
 * A carrier that was on is counted against the duty cycle when it ends.
 */
static void apply_bench(int64_t now)
{
    if (N.want_bench < 0 || N.txing) {
        return;
    }
    int mode = N.want_bench;
    N.want_bench = -1;
    if (N.carrier_since) {
        link_add_airtime(&N.eng, now, (uint32_t)(now - N.carrier_since));
        N.carrier_since = 0;
    }
    if (!N.radio_ok) {
        return;
    }
    esp_err_t err = ESP_OK;
    switch (mode) {
    case NET_BENCH_STANDBY:
        err = radio_bench_standby();
        break;
    case NET_BENCH_CARRIER:
        err = radio_bench_carrier(N.want_bench_dbm);
        if (err == ESP_OK) {
            N.carrier_since = now;
        }
        break;
    default: /* LISTEN, OFF */
        err = radio_bench_listen();
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bench: the radio refused (%s)", esp_err_to_name(err));
    }
    N.bench = mode;
}

/*
 * apply_sf() -- a requested spreading factor, once no packet is on air (the
 * modem is reprogrammed in standby). The engine's timing follows, and LoRa
 * starts "checking" (link_set_lora); so does the loopback partner, which
 * lives on the same pretend channel. A setting the radio refuses (or no
 * radio) still changes the engine, so loopback tests work on one board.
 */
static void apply_sf(int64_t now)
{
    if (N.want_sf < 0 || N.txing) {
        return;
    }
    int sf = N.want_sf < RADIO_SF_MIN ? RADIO_SF_MIN : N.want_sf > RADIO_SF_MAX ? RADIO_SF_MAX : N.want_sf;
    N.want_sf = -1;
    if (N.radio_ok && radio_set_sf(sf) != ESP_OK) {
        ESP_LOGW(TAG, "the radio refused SF%d", sf);
    }
    N.lp.sf = (uint8_t)sf;
    link_set_lora(&N.eng, &N.lp, now);
    if (N.sim_valid) {
        link_set_lora(&N.sim, &N.lp, now);
    }
    ESP_LOGI(TAG, "spreading factor SF%d: keepalive every %lu s, lost after %lu s", sf,
             (unsigned long)(N.eng.tm.keepalive_ms / 1000), (unsigned long)(N.eng.tm.lost_ms / 1000));
}

/* post() -- queue a command for the net task and wake it. False if the
 * queue stayed full for 50 ms (the task is stuck: should never happen). */
static bool post(const net_cmd_t *c)
{
    if (!N.cmdq || xQueueSend(N.cmdq, c, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "command queue full: command %d dropped", c->op);
        return false;
    }
    if (N.task) {
        xTaskNotify(N.task, NB_CMD, eSetBits); /* (otherwise it wakes within MAX_SLEEP_MS) */
    }
    return true;
}

/* post_simple() -- a command with just an address and a number. */
static bool post_simple(uint8_t op, uint32_t addr, int32_t arg)
{
    net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = op;
    c.addr = addr;
    c.arg = arg;
    return post(&c);
}

/* ------------------------------------------------------------------------- */
/* Status snapshot                                                            */
/* ------------------------------------------------------------------------- */

/* update_status() -- refresh the snapshot that net_get_status() hands out
 * (every 250 ms), under the lock. */
static void update_status(int64_t now)
{
    net_status_t s;
    memset(&s, 0, sizeof(s));
    const link_t *L = &N.eng;
    s.radio_ok = N.radio_ok;
    s.loopback = N.loopback;
    s.own = N.own;
    s.tx_dbm = N.tx_dbm;
    s.tx_dbm_max = radio_tx_dbm_max();
    s.sf = N.lp.sf;
    s.peer = L->peer;
    s.state = L->state;
    s.heard_ago_s = L->peer != ADDR_NONE ? (int32_t)((now - L->last_heard) / 1000) : -1;
    s.peer_rssi = L->peer_rssi;
    s.peer_snr_x4 = L->peer_snr_x4;
    for (uint8_t p = LP_LORA; p <= LP_RELAY; p++) {
        s.path_st[p] = link_path_state(L, p, now);
        s.heard_s[p] = link_heard_ago_s(L, p, now);
        s.they_s[p] = link_they_heard_s(L, p, now);
    }
    s.reports = L->rep_ever;
    s.lost_why = L->lost_why;
    s.bench = (uint8_t)N.bench;
    s.busy = link_busy(L, now);
    s.req_phase = L->req_phase;
    s.req_to = L->req_to;
    s.n_pend = L->n_pend;
    s.n_msg = L->n_msg;
    s.duty_used_ms = link_duty_used_ms(L, now);
    s.duty_budget_ms = dc_budget_ms();
    s.pkts_tx = L->n_tx;
    s.pkts_rx = L->n_rx;
    s.pkts_dup = L->n_dup;
    s.pkts_bad = L->n_bad;
    s.phone_in = L->n_rx_phone;
    s.phone_out = N.phone_out;
    s.disc_answered = L->n_disc_answered;
    s.disc_refused = L->n_disc_refused;
    radio_stats_t rs;
    radio_get_stats(&rs);
    s.pkts_crc = rs.rx_bad;
    s.noise_dbm = N.radio_ok && !N.txing ? radio_noise_dbm() : 0;
    s.stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL); /* the least ever left free */
    xSemaphoreTake(N.lock, portMAX_DELAY);
    N.st = s;
    xSemaphoreGive(N.lock);
}

/* ------------------------------------------------------------------------- */
/* The task                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * net_task() -- the network task. Finds and configures the radio, then
 * loops forever: sleep until something is due (or a radio interrupt or a
 * command wakes it), then run commands, service the radio, catch a stuck
 * transmission, move the loopback world, run the engine's timers, hand its
 * events to the UI and transmit what is due. Every 10 s it checks the radio
 * is still listening; every 250 ms it refreshes the status snapshot.
 */
static void net_task(void *arg)
{
    (void)arg;
    /* This task can start before xTaskCreatePinnedToCore() has stored its
     * handle (it outranks app_main on the same core), so fetch it here: the
     * radio interrupt needs it to wake us. */
    N.task = xTaskGetCurrentTaskHandle();
    radio_cfg_t rc = {
        .freq_hz = RADIO_FREQ_HZ,
        .bw_hz = RADIO_BW_HZ,
        .sf = N.lp.sf,
        .cr = RADIO_CR,
        .sync_word = RADIO_SYNC_WORD,
        .tx_dbm = (int8_t)N.tx_dbm,
        .rx_preamble = RADIO_PREAMBLE_LONG > RADIO_PREAMBLE_SHORT ? RADIO_PREAMBLE_LONG : RADIO_PREAMBLE_SHORT,
    };
    N.radio_ok = radio_init(&rc, N.task, NB_RADIO) == ESP_OK;
    if (!N.radio_ok) {
        ESP_LOGW(TAG, "no radio: only the loopback test network works (console: loopback on)");
    }
    engine_start(ADDR_NONE, now_ms());
    int64_t last_status = 0;

    for (;;) {
        int64_t now = now_ms();
        /* Sleep until the engine has something due (at most MAX_SLEEP_MS),
         * or a radio interrupt / command arrives. */
        int64_t wake = link_next_due(&N.eng, now);
        if (wake > now + MAX_SLEEP_MS) {
            wake = now + MAX_SLEEP_MS;
        }
        if (N.loopback) {
            wake = now + 20; /* the loopback world moves in small steps */
        }
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(wake > now ? wake - now : 0));
        now = now_ms();

        net_cmd_t c;
        while (xQueueReceive(N.cmdq, &c, 0) == pdTRUE) {
            run_command(&c, now);
        }
        if (N.radio_ok && ((bits & NB_RADIO) || radio_irq_pending())) {
            service_radio(now);
        }
        if (N.txing && now > N.tx_deadline) {
            radio_recover();
            N.txing = false;
            link_tx_done(&N.eng, now, false);
        }
        if (N.loopback) {
            loopback_tick(now);
        }
        apply_power();
        apply_sf(now);
        apply_bench(now);
        /* The relay counts as a path only while a phone is here and not in
         * use by a test (each test path carries its own copies: ntest.c). */
        link_set_relay(&N.eng, ble_phone_ready() && !ntest_active(), now);
        link_poll(&N.eng, now);
        deliver_events(now);
        ntest_tick(now); /* test traffic: a new L copy can go out just below */
        deliver_events(now); /* a refused send reports at once */
        try_transmit(now);
        deliver_events(now);

        if (N.radio_ok && !N.txing && N.bench == NET_BENCH_OFF && now >= N.next_check) {
            N.next_check = now + HEALTH_CHECK_MS;
            radio_check();
        }
        if (now - last_status >= 250) {
            last_status = now;
            update_status(now);
        }
    }
}

/* ========================================================================= */
/* API (any task)                                                             */
/* ========================================================================= */

/* noise_now() -- the channel's noise floor for test mode's STAT lines (0
 * while transmitting or without a radio). */
static int noise_now(void)
{
    return N.radio_ok && !N.txing ? radio_noise_dbm() : 0;
}

/*
 * test_set_radio() -- test mode's point test switches the radio for a step
 * (ntest.c, in this task): the spreading factor and power to apply, which
 * apply_sf() and apply_power() do on this loop or the next, once no packet
 * is on air. Nothing is saved: the stored setting comes back at the end (and
 * at any restart). Returns the power the chip will use (clamped to its
 * range, as apply_power() does), for the STEP line.
 */
static int test_set_radio(int sf, int dbm)
{
    int max = radio_tx_dbm_max();
    int d = dbm < RADIO_TX_DBM_MIN ? RADIO_TX_DBM_MIN : dbm > max ? max : dbm;
    N.want_sf = sf;
    N.want_dbm = d;
    return d;
}

/* net_init() -- create the command queue and the net task (core 0,
 * priority 10, above the UI so radio replies go out promptly). The task
 * itself looks for the radio. Call after pmic_init(): it powers the radio. */
void net_init(void)
{
    N.lock = xSemaphoreCreateMutex();
    N.cmdq = xQueueCreate(CMDQ_LEN, sizeof(net_cmd_t));
    N.lp = (lora_params_t){RADIO_SF_DEFAULT, RADIO_BW_HZ, RADIO_CR}; /* the stored setting follows from the UI (net_set_sf) */
    N.tx_dbm = RADIO_TX_DBM_DEFAULT;
    N.want_dbm = -1;
    N.want_sf = -1;
    N.want_bench = -1;
    N.discoverable = true; /* until the UI says otherwise (silent mode, Z) */
    N.own = ADDR_NONE;
    N.st.own = ADDR_NONE;
    N.st.peer = ADDR_NONE;
#if VERBOSE_LOG
    N.log = true; /* board.h: show every packet (`radio log off` stops it) */
#endif
    ntest_init(&N.eng, noise_now, test_set_radio);
    xTaskCreatePinnedToCore(net_task, "net", 6144, NULL, 10, &N.task, 0); /* (test mode's lines need the extra KB) */
}

/* net_set_identity() -- queue: restart the engine for address `own`. */
void net_set_identity(uint32_t own)
{
    post_simple(NC_IDENTITY, own, 0);
}

/* net_set_loopback() -- queue: switch between the radio and the pretend
 * partner. The flag is set at once so net_has_transport() is right
 * immediately. */
void net_set_loopback(bool on)
{
    N.loopback = on; /* visible at once to net_has_transport(); the task does the rest */
    post_simple(NC_LOOPBACK, 0, on);
}

/* net_loopback() -- is the loopback network on? */
bool net_loopback(void)
{
    return N.loopback;
}

/* net_has_transport() -- can anything be sent at all (radio or loopback)? */
bool net_has_transport(void)
{
    return N.loopback || N.radio_ok;
}

/* net_set_link() -- queue: change the link without a handshake. */
void net_set_link(uint32_t peer)
{
    post_simple(NC_LINK, peer, 0);
}

/* net_set_blocklist() -- queue: copy the user's blocklist to the engine. */
void net_set_blocklist(const uint32_t *list, int n)
{
    net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = NC_BLOCKLIST;
    c.arg = n > BLOCK_MAX ? BLOCK_MAX : n;
    memcpy(c.list, list, (size_t)c.arg * sizeof(uint32_t));
    post(&c);
}

/* net_set_contacts() -- queue: the user's contacts (general SOS through the phone). */
void net_set_contacts(const uint32_t *list, int n)
{
    net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = NC_CONTACTS;
    c.arg = n > PEER_SLOTS ? PEER_SLOTS : n;
    memcpy(c.list, list, (size_t)c.arg * sizeof(uint32_t));
    post(&c);
}

/* net_phone_rx() -- queue: a packet that came down from the phone. Packets
 * longer than a packet can be are dropped here. */
void net_phone_rx(const uint8_t *pkt, int len)
{
    if (len <= 0 || len > PROTO_MAX_PKT) {
        return;
    }
    net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = NC_PHONE_RX;
    c.arg = len;
    memcpy(c.pkt, pkt, (size_t)len);
    post(&c);
}

/* net_set_tx_power() -- queue: new transmit power (applied between packets). */
void net_set_tx_power(int dbm)
{
    post_simple(NC_POWER, 0, dbm);
}

/* net_bench() -- queue: bench power's radio state (applied between packets). */
void net_bench(int mode, int dbm)
{
    post_simple(NC_BENCH, (uint32_t)dbm, mode);
}

/* net_set_sf() -- queue: new spreading factor (applied between packets). */
void net_set_sf(int sf)
{
    post_simple(NC_SF, 0, sf);
}

/* net_link_request() -- queue: S / P, ask `to` to connect. */
void net_link_request(uint32_t to)
{
    post_simple(NC_REQUEST, to, 0);
}

/* net_link_accept() -- queue: A, accept the request from `from`. */
void net_link_accept(uint32_t from)
{
    post_simple(NC_ACCEPT, from, 0);
}

/* net_link_decline() -- queue: N, decline the request from `from`. */
void net_link_decline(uint32_t from)
{
    post_simple(NC_DECLINE, from, 0);
}

/* net_unpair() -- queue: X, tell `addr` we forgot them. */
void net_unpair(uint32_t addr)
{
    post_simple(NC_UNPAIR, addr, 0);
}

/* net_send_text() -- queue a message for `dst` (the announced recipient).
 * Refused at once only if there is no transport or the queue is full. */
net_result_t net_send_text(uint32_t dst, const char *text)
{
    if (!net_has_transport()) {
        return NET_ERR_NO_TRANSPORT;
    }
    net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = NC_MSG;
    c.addr = dst;
    strncpy(c.text, text, MSG_MAX_CHARS);
    return post(&c) ? NET_OK : NET_ERR_QUEUE;
}

/* net_send_sos() -- queue: our SOS (or a resend), to `dst` or everyone. */
void net_send_sos(uint32_t dst)
{
    post_simple(NC_SOS, dst, 0);
}

/* net_send_sos_cancel() -- queue: our SOS is over. */
void net_send_sos_cancel(uint32_t dst)
{
    post_simple(NC_SOS_CANCEL, dst, 0);
}

/* net_send_sos_ack() -- queue: we acknowledge `dst`'s SOS. */
void net_send_sos_ack(uint32_t dst)
{
    post_simple(NC_SOS_ACK, dst, 0);
}

/* net_discover() -- queue: D, sweep for devices around (LEV_DISC_DONE follows). */
void net_discover(void)
{
    post_simple(NC_DISCOVER, 0, 0);
}

/* net_set_discoverable() -- queue: silent mode off (true) / on (false). */
void net_set_discoverable(bool on)
{
    post_simple(NC_DISCOVERABLE, 0, on);
}

/* net_ping() -- queue: broadcast a PING (answers are logged as PONG lines). */
void net_ping(void)
{
    post_simple(NC_PING, 0, 0);
}

/* net_set_log() -- queue: log every packet on / off. */
void net_set_log(bool on)
{
    post_simple(NC_LOG, 0, on);
}

/* net_sim_request() -- queue: (loopback) the pretend partner, as `from`,
 * asks us to connect. */
void net_sim_request(uint32_t from)
{
    post_simple(NC_SIM_REQUEST, from, 0);
}

/* net_sim_deaf() -- queue: (loopback) the pretend partner goes out of / back
 * into range. */
void net_sim_deaf(bool on)
{
    post_simple(NC_SIM_DEAF, 0, on);
}

/* net_sim_discover() -- queue: (loopback) the pretend partner sweeps (D). */
void net_sim_discover(void)
{
    post_simple(NC_SIM_DISCOVER, 0, 0);
}

/* net_test_start() -- queue: start a test (ntest.h), parameters as text
 * (at most TT_ARGS_MAX characters). */
void net_test_start(const char *args, bool from_phone)
{
    net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = NC_TEST_START;
    c.arg = from_phone;
    strncpy(c.args, args ? args : "", TT_ARGS_MAX);
    post(&c);
}

/* net_test_refuse() -- queue: the UI refused a start; log why (REFUSED). */
void net_test_refuse(const char *reason)
{
    net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = NC_TEST_REFUSE;
    strncpy(c.text, reason, MSG_MAX_CHARS);
    post(&c);
}

/* net_test_stop() -- queue: stop the test (`reason` goes in the STOP line). */
void net_test_stop(const char *reason)
{
    net_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = NC_TEST_STOP;
    strncpy(c.text, reason, MSG_MAX_CHARS);
    post(&c);
}

/* net_test_battery() -- queue: the latest battery reading, for test mode. */
void net_test_battery(int mv, int pct, bool charging)
{
    post_simple(NC_TEST_BATTERY, mv < 0 ? ADDR_NONE : (uint32_t)mv, (int32_t)((pct & 0xFF) | (charging ? 0x100 : 0)));
}

/* net_get_status() -- copy the latest snapshot (refreshed every 250 ms by
 * the net task). Safe from any task. */
void net_get_status(net_status_t *st)
{
    xSemaphoreTake(N.lock, portMAX_DELAY);
    *st = N.st;
    xSemaphoreGive(N.lock);
}
