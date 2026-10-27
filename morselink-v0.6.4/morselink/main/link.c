/*
 * MorseLink — link protocol engine (see link.h for the rules and how to
 * drive it).
 *
 * Layout of this file:
 *   1. small helpers: random jitter, events, airtime, blocklist
 *   2. the transmit queue ("outbox"): every packet we send goes through it,
 *      with its number of copies, the pause between them and how long to
 *      wait for an answer afterwards
 *   3. link bookkeeping shared by several paths (changing partner,
 *      forgetting a device, the message queue)
 *   4. user actions (link_request, link_accept, ...)
 *   5. receiving: one handler per packet type
 *   6. timers (link_poll) and the radio interface (link_take_tx / _tx_done)
 */
#include "link.h"

#include <string.h>

/* Kinds of transmit-queue entry, in priority order: when several packets
 * are due at once, the lowest kind goes first. */
typedef enum {
    OB_FREE = 0,
    OB_SOS,       /* our own SOS, SOS_ACK or SOS_CANCEL */
    OB_REPLY,     /* an immediate answer: RECEIVED, BUSY, ACCEPT, ENDED, receipts, confirms, PONG */
    OB_RELAY,     /* someone else's SOS-family packet, passed on once */
    OB_ACCEPT,    /* our user's ACCEPT, repeated until PAIR_CONFIRM arrives */
    OB_REQ,       /* our LINK_REQ, repeated until any answer arrives */
    OB_MSG,       /* our MSG, repeated until its MSG_RECEIPT arrives */
    OB_NOTICE,    /* ENDED / DECLINE / UNPAIR: a few copies, nothing comes back */
    OB_DISC,      /* our DISCOVER (D): one copy; the answers come in slots after it */
    OB_KEEPALIVE, /* "still here" */
    OB_PING,      /* bench tool */
} ob_kind_t;

#define ANY_ADDR 0xFFFFFFFFu        /* wildcard for ob_cancel(): not a 24-bit address */
#define EX_KEEP_MS (60 * 60 * 1000) /* remember a former partner for an hour */
#define EX_REPLY_GAP_MS 10000       /* tell a former partner "ended" at most every 10 s */
#define OB_HARD_DEFAULT_MS 60000    /* any queued packet is abandoned after a minute (unless given longer) */
#define TX_RETRY_MS 250             /* the radio failed to send: try that copy again this soon */
#define QUICK_KA_MIN_MS 200         /* an extra keepalive (ka_soon) goes out this long ... */
#define QUICK_KA_SPREAD_MS 1000     /* ... plus up to this much at random after the reason for it */
/* Timing that grows with the airtime (answers' deadlines, PONG spread, SOS
 * relay pauses, ...) is in L->tm: see link_timing() below. */

/* ========================================================================= */
/* 1. Helpers                                                                 */
/* ========================================================================= */

/*
 * rnd() -- next value of a 32-bit xorshift generator.
 * Only used for jitter, so statistical quality hardly matters; what matters
 * is that two devices don't make the same choices.
 * Side effect: advances L->rng.
 */
static uint32_t rnd(link_t *L)
{
    uint32_t x = L->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    L->rng = x ? x : 0x9E3779B9u; /* xorshift must never reach 0 */
    return L->rng;
}

/* rnd_upto() -- a random number from 0 to n inclusive. */
static uint32_t rnd_upto(link_t *L, uint32_t n)
{
    return n ? rnd(L) % (n + 1) : 0;
}

/*
 * emit() -- append an event for the user interface.
 * In: kind, the other device's address, and a kind-specific argument.
 * Returns the event so the caller can fill in text or signal levels.
 * Side effect: when the queue is full the oldest event is dropped (and
 * counted); the caller drains the queue after every call, so that only
 * happens if something has gone badly wrong.
 */
static link_evt_t *emit(link_t *L, uint8_t kind, uint32_t addr, uint8_t arg)
{
    if (L->ev_n == LINK_EV_N) {
        L->ev_head = (uint8_t)((L->ev_head + 1) % LINK_EV_N);
        L->ev_n--;
        L->n_ev_dropped++;
    }
    link_evt_t *e = &L->ev[(L->ev_head + L->ev_n) % LINK_EV_N];
    L->ev_n++;
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->addr = addr;
    e->arg = arg;
    return e;
}

/* pkt_airtime_ms() -- airtime of `len` bytes, rounded up to whole ms. */
static uint32_t pkt_airtime_ms(const link_t *L, int len, bool long_preamble)
{
    int pre = long_preamble ? RADIO_PREAMBLE_LONG : RADIO_PREAMBLE_SHORT;
    return (lora_airtime_us(len, &L->lora, pre) + 999u) / 1000u;
}

/*
 * ka_interval() -- time until the next keepalive: the keepalive period (30 s
 * at SF9) minus a random 0..jitter (4 s at SF9), so that two partners that
 * started at the same moment drift apart instead of colliding every time,
 * and three periods always fit inside the lost time.
 */
static int64_t ka_interval(link_t *L)
{
    return (int64_t)L->tm.keepalive_ms - (int64_t)rnd_upto(L, L->tm.ka_jitter_ms);
}

/*
 * ka_soon() -- send our keepalive within about a second (unless one was
 * due sooner anyway): the partner then gets our report at once, instead of
 * up to a period later. At most once per tm.quick_ka_gap_ms (10 s at SF9),
 * so a link that stays one-way never makes extra traffic; the extra one
 * also restarts the regular period (link_tx_done), so it adds none on
 * average either.
 */
static void ka_soon(link_t *L, int64_t now)
{
    if (L->quick_ka_at != LINK_NEVER && now - L->quick_ka_at < (int64_t)L->tm.quick_ka_gap_ms) {
        return;
    }
    L->quick_ka_at = now;
    int64_t t = now + QUICK_KA_MIN_MS + (int64_t)rnd_upto(L, QUICK_KA_SPREAD_MS);
    if (t < L->ka_due) {
        L->ka_due = t;
    }
}

/* is_blocked() -- is `a` on the user's blocklist (our copy of it)? */
static bool is_blocked(const link_t *L, uint32_t a)
{
    for (int i = 0; i < L->n_blocked; i++) {
        if (L->blocked[i] == a) {
            return true;
        }
    }
    return false;
}

/* ---- reminder list ("ex" list) ------------------------------------------- */
/*
 * Devices that may wrongly believe they are linked to us: former partners,
 * someone whose accept we gave up on, someone we unpaired. When their
 * keepalive arrives they are reminded (see on_keepalive), at most every
 * EX_REPLY_GAP_MS. Entries are created by those events, or by on_keepalive
 * itself for a device we don't know (e.g. after we restarted).
 */

/* ex_find() -- the entry for `a`, or NULL. Expired entries don't count. */
static link_ex_t *ex_find(link_t *L, uint32_t a, int64_t now)
{
    for (int i = 0; i < LINK_EX_N; i++) {
        if (L->ex[i].addr == a && now < L->ex[i].until) {
            return &L->ex[i];
        }
    }
    return NULL;
}

/*
 * ex_add() -- remember that `a` may still believe it is linked to us.
 * Reuses a's entry, else a free one, else the one closest to expiring.
 * `last_reply` starts at now: whoever calls this has just told them, or
 * will hear from them within a keepalive period anyway. `unpaired` says
 * which reminder they get (UNPAIR or ENDED).
 */
static void ex_add(link_t *L, uint32_t a, bool unpaired, int64_t now)
{
    link_ex_t *slot = NULL;
    for (int i = 0; i < LINK_EX_N && !slot; i++) {
        if (L->ex[i].addr == a) {
            slot = &L->ex[i];
        }
    }
    for (int i = 0; i < LINK_EX_N && !slot; i++) {
        if (L->ex[i].addr == ADDR_NONE || now >= L->ex[i].until) {
            slot = &L->ex[i];
        }
    }
    if (!slot) {
        slot = &L->ex[0];
        for (int i = 1; i < LINK_EX_N; i++) {
            if (L->ex[i].until < slot->until) {
                slot = &L->ex[i];
            }
        }
    }
    slot->addr = a;
    slot->until = now + EX_KEEP_MS;
    slot->last_reply = now;
    slot->unpaired = unpaired;
}

/* ex_remove() -- `a` is our partner again (or forgotten): drop its entry. */
static void ex_remove(link_t *L, uint32_t a)
{
    for (int i = 0; i < LINK_EX_N; i++) {
        if (L->ex[i].addr == a) {
            L->ex[i].addr = ADDR_NONE;
        }
    }
}

/* ---- responders to our SOS ------------------------------------------------- */

/* sosrel_add() -- `a` acknowledged our SOS: for the next LINK_SOSREL_MS
 * (extended by each resend), never tell it "ended". */
static void sosrel_add(link_t *L, uint32_t a, int64_t now)
{
    link_sosrel_t *slot = &L->sosrel[0];
    for (int i = 0; i < LINK_SOSREL_N; i++) {
        if (L->sosrel[i].addr == a) {
            slot = &L->sosrel[i];
            break;
        }
        if (L->sosrel[i].until < slot->until) {
            slot = &L->sosrel[i]; /* otherwise reuse the oldest */
        }
    }
    slot->addr = a;
    slot->until = now + LINK_SOSREL_MS;
}

/* sosrel_has() -- is `a` a responder to our SOS that is still going on? */
static bool sosrel_has(const link_t *L, uint32_t a, int64_t now)
{
    for (int i = 0; i < LINK_SOSREL_N; i++) {
        if (L->sosrel[i].addr == a && now < L->sosrel[i].until) {
            return true;
        }
    }
    return false;
}

/* ========================================================================= */
/* 2. Transmit queue                                                          */
/* ========================================================================= */

/*
 * ob_add() -- queue a packet for transmission.
 * In:  kind      ob_kind_t (sets priority and what happens when it runs out)
 *      p         the packet (encoded here)
 *      tries     how many copies to send
 *      gap_ms    pause between copies (plus random jitter)
 *      wait_ms   after the last copy, how long to wait for an answer before
 *                calling it "exhausted" (0: forget it once sent)
 *      delay_ms  don't send before now + delay_ms
 *      hard_ms   abandon it after this long whatever happens (0: default)
 *      exempt    SOS family: ignores the duty-cycle budget
 * Returns the entry, or NULL if the queue is full or the packet malformed.
 * The slot on air right now is never reused (link_tx_done still needs it).
 */
static link_ob_t *ob_add(link_t *L, uint8_t kind, const pkt_t *p, uint8_t tries, uint16_t gap_ms, uint16_t wait_ms,
                         uint32_t delay_ms, uint32_t hard_ms, bool exempt, int64_t now)
{
    for (int i = 0; i < LINK_OB_N; i++) {
        link_ob_t *o = &L->ob[i];
        if (o->kind != OB_FREE || i == L->inflight) {
            continue;
        }
        int len = proto_encode(p, o->pkt, (int)sizeof(o->pkt));
        if (len < 0) {
            return NULL;
        }
        o->kind = kind;
        o->len = (uint8_t)len;
        o->tries = tries;
        o->gap_ms = gap_ms;
        o->wait_ms = wait_ms;
        o->due = now + delay_ms;
        o->give_up = 0;
        o->hard = now + (hard_ms ? hard_ms : OB_HARD_DEFAULT_MS);
        o->exempt = exempt;
        o->dst = p->dst;
        o->id = p->id;
        return o;
    }
    return NULL;
}

/*
 * ob_cancel() -- remove queued packets of `kind` to `dst` (ANY_ADDR: any
 * destination) with message ID `id` (0: any ID). Used when an answer makes
 * further copies pointless. A packet on air right now finishes, but
 * link_tx_done() then finds its entry free and does nothing more.
 */
static void ob_cancel(link_t *L, uint8_t kind, uint32_t dst, uint16_t id)
{
    for (int i = 0; i < LINK_OB_N; i++) {
        link_ob_t *o = &L->ob[i];
        if (o->kind == kind && (dst == ANY_ADDR || o->dst == dst) && (id == 0 || o->id == id)) {
            o->kind = OB_FREE;
        }
    }
}

/* ob_has() -- is a packet of `kind` queued? */
static bool ob_has(const link_t *L, uint8_t kind)
{
    for (int i = 0; i < LINK_OB_N; i++) {
        if (L->ob[i].kind == kind) {
            return true;
        }
    }
    return false;
}

/* new_id() -- a fresh message ID (may save the next ID block to flash). */
static uint16_t new_id(link_t *L)
{
    return msgid_next(&L->ids);
}

/* send_link_reply() -- queue a one-copy LINK_REPLY: `status` about the packet
 * whose ID is `ref` (0 when it answers nothing in particular). */
static void send_link_reply(link_t *L, uint32_t to, uint8_t status, uint16_t ref, int64_t now)
{
    pkt_t p;
    proto_init_pkt(&p, PT_LINK_REPLY, L->own, to, new_id(L));
    p.payload[0] = status;
    proto_put16(&p.payload[1], ref);
    p.plen = 3;
    ob_add(L, OB_REPLY, &p, 1, 0, 0, 0, L->tm.reply_hard_ms, false, now);
}

/* send_ref_reply() -- queue a one-copy PAIR_CONFIRM or MSG_RECEIPT quoting `ref`. */
static void send_ref_reply(link_t *L, uint8_t type, uint32_t to, uint16_t ref, int64_t now)
{
    pkt_t p;
    proto_init_pkt(&p, type, L->own, to, new_id(L));
    proto_put16(p.payload, ref);
    p.plen = 2;
    ob_add(L, OB_REPLY, &p, 1, 0, 0, 0, L->tm.reply_hard_ms, false, now);
}

/* send_notice() -- queue a packet nobody answers (ENDED, DECLINE, UNPAIR):
 * NOTICE_COPIES copies with the same ID, so the receiver acts on it once. */
static void send_notice(link_t *L, pkt_t *p, int64_t now)
{
    p->flags |= PF_LONG_PREAMBLE; /* unsolicited: must be able to wake a sleeping receiver (Phase 2) */
    ob_add(L, OB_NOTICE, p, NOTICE_COPIES, NOTICE_GAP_MS, 0, 0, 0, false, now);
}

/* ========================================================================= */
/* 3. Link bookkeeping                                                        */
/* ========================================================================= */

/* ---- requests waiting for our user -------------------------------------- */

/* pend_find() -- index of the pending request from `from`, or -1. */
static int pend_find(const link_t *L, uint32_t from)
{
    for (int i = 0; i < L->n_pend; i++) {
        if (L->pend[i].from == from) {
            return i;
        }
    }
    return -1;
}

/* pend_remove_at() -- delete entry i, keeping the others in arrival order. */
static void pend_remove_at(link_t *L, int i)
{
    memmove(&L->pend[i], &L->pend[i + 1], (size_t)(L->n_pend - i - 1) * sizeof(L->pend[0]));
    L->n_pend--;
}

/* pend_drop() -- delete the request from `from`, if any. With tell = true
 * and the user already told about it, emits LEV_REQ_GONE. */
static void pend_drop(link_t *L, uint32_t from, bool tell)
{
    int i = pend_find(L, from);
    if (i < 0) {
        return;
    }
    bool was_alerted = L->pend[i].alerted;
    pend_remove_at(L, i);
    if (tell && was_alerted) {
        emit(L, LEV_REQ_GONE, from, 0);
    }
}

/* ---- our own request and accept ----------------------------------------- */

/* req_close() -- our open request is over (answered, superseded, expired):
 * stop sending it. No event. */
static void req_close(link_t *L)
{
    if (L->req_phase != REQ_IDLE) {
        ob_cancel(L, OB_REQ, L->req_to, 0);
    }
    L->req_phase = REQ_IDLE;
}

/* acc_cancel() -- abandon the accept we are sending; with tell = true the
 * user hears it failed (LEV_ACCEPT_RESULT 0). */
static void acc_cancel(link_t *L, bool tell)
{
    if (!L->acc_active) {
        return;
    }
    ob_cancel(L, OB_ACCEPT, L->acc_to, 0);
    L->acc_active = false;
    if (tell) {
        emit(L, LEV_ACCEPT_RESULT, L->acc_to, 0);
    }
}

/* ---- the link's paths: LoRa and the relay (agreed 15 Oct) --------------- */
/*
 * The connection-lost fix. Up to v0.5.1 a failed message marked the link
 * lost without telling anyone, after which the 90 s silence check could
 * never fire; packets relayed by the phones counted as hearing the partner,
 * so a dead LoRa link never showed; and a partner we could hear but who
 * couldn't hear us looked connected. Now each path has its own status, it
 * is connected only when proven both ways (we hear them; their keepalive
 * report says they hear us), and every change of the link as a whole goes
 * through link_eval(), which reports it exactly once.
 */

/*
 * heard_byte() -- "seconds since t", as a keepalive report carries it:
 * 0..254, or KA_NEVER for never (LINK_NEVER) and for 255 s or more.
 */
static uint8_t heard_byte(int64_t t, int64_t now)
{
    if (t == LINK_NEVER) {
        return KA_NEVER;
    }
    int64_t s = (now - t) / 1000;
    return s < 0 ? 0 : s >= KA_NEVER ? KA_NEVER : (uint8_t)s;
}

/*
 * path_state() -- what path `p` (link_path_t) is doing right now, from what
 * we heard on it and what the partner last reported (see link.h):
 *   in    we heard the partner itself on p within the lost time;
 *   fresh their latest report arrived within the lost time, after our
 *         last failed message (they may not have heard that one) and, for
 *         LoRa, after the last change of radio setting;
 *   out   by that fresh report they heard us on p within the lost time.
 *         A partner that has never sent a report (firmware before v0.6.0)
 *         is taken to hear us whenever we hear them, as before, except
 *         that after a failed message only a packet heard since counts.
 * in and out: up. Not proven yet within the lost time after the path
 * (re)started, with no failure since: checking. We hear them but a fresh
 * report says they don't hear us, or a message has failed and no report
 * came since: they can't hear us (only while we are still hearing them,
 * within two keepalive periods: when both directions fall silent together,
 * their last report turns "negative" a little before our own hearing runs
 * out, and that is not one-way). A fresh report says they hear us, but we
 * don't hear them (the report came the other way): we can't hear them.
 * Anything else: lost. The relay is off while no phone is here. Pure.
 */
static uint8_t path_state(const link_t *L, uint8_t p, int64_t now)
{
    if (L->peer == ADDR_NONE || (L->state != LS_UP && L->state != LS_LOST)) {
        return PS_NONE;
    }
    if (p == LP_RELAY && !L->relay_on) {
        return PS_OFF;
    }
    const int64_t lost = L->tm.lost_ms;
    const int64_t heard = p == LP_LORA ? L->lora_heard : L->relay_heard;
    const bool in = heard != LINK_NEVER && now - heard < lost;
    /* Heard within two keepalive periods: we are still hearing them now
     * (one keepalive may have been lost), not just a while ago. */
    const bool recent = in && now - heard < lost * 2 / 3;
    /* Reports from before these moments don't count (LINK_NEVER is the
     * smallest time there is, so "after LINK_NEVER" means any report). */
    int64_t valid_after = L->fail_at;
    if (p == LP_LORA && L->lora_rep_from > valid_after) {
        valid_after = L->lora_rep_from;
    }
    const bool fresh = L->rep_ever && L->rep_at > valid_after && now - L->rep_at < lost;
    const uint8_t r = p == LP_LORA ? L->rep_lora_s : L->rep_relay_s;
    bool out;
    if (L->rep_ever) {
        out = fresh && r != KA_NEVER && (int64_t)r * 1000 < lost;
    } else {
        out = in && heard > L->fail_at; /* (any time is after LINK_NEVER: no failure) */
    }
    if (in && out) {
        return PS_UP;
    }
    const int64_t since = p == LP_LORA ? L->lora_since : L->relay_since;
    if (L->fail_at < since && now - since < lost) {
        return PS_CHECKING; /* just (re)started: give it the lost time to prove itself */
    }
    const bool failed_since = L->fail_at != LINK_NEVER && L->rep_at <= L->fail_at; /* no report since the failure */
    if (recent && ((fresh && !out) || failed_since)) {
        return PS_THEY_CANT;
    }
    if (!in && out) {
        return PS_WE_CANT;
    }
    return PS_LOST;
}

/*
 * link_eval() -- bring the link's status up to date. Runs after anything
 * heard, a failed message, a phone coming or going, a new radio setting,
 * and on every poll (paths also go down just by time passing).
 *  - Each path's state; a change is an event (LEV_PATH). A path that has
 *    just become "they can't hear us", or has just become connected, sends
 *    our keepalive soon (ka_soon): the partner learns from our report that
 *    we hear them, and both ends agree within seconds instead of a period.
 *  - The link as a whole: up while a path is up, and still up while a path
 *    is checking (a link that has just started, or a new radio setting,
 *    gets the lost time to prove itself; a lost link is not restored by a
 *    path that is only checking). Every move between up and lost is
 *    reported exactly once: LEV_LINK_LOST with its reason, or
 *    LEV_LINK_RESTORED. `why` gives the reason (LOST_NO_RECEIPT from a failed
 *    message); -1 lets the paths decide: one-way if a path works in one
 *    direction, otherwise silence.
 */
static void link_eval(link_t *L, int64_t now, int why)
{
    if (L->peer == ADDR_NONE || (L->state != LS_UP && L->state != LS_LOST)) {
        L->path_st[LP_LORA] = L->path_st[LP_RELAY] = PS_NONE; /* no link: nothing to report */
        return;
    }
    uint8_t st[2];
    for (uint8_t p = LP_LORA; p <= LP_RELAY; p++) {
        st[p] = path_state(L, p, now);
        const uint8_t was = L->path_st[p];
        if (st[p] == was) {
            continue;
        }
        L->path_st[p] = st[p];
        emit(L, LEV_PATH, L->peer, (uint8_t)(p << 4 | st[p]));
        if (st[p] == PS_THEY_CANT || st[p] == PS_UP) {
            ka_soon(L, now); /* they can't hear us, or the path is newly up: our report tells them we hear them */
        }
    }
    const bool any_up = st[LP_LORA] == PS_UP || st[LP_RELAY] == PS_UP;
    const bool checking = st[LP_LORA] == PS_CHECKING || st[LP_RELAY] == PS_CHECKING;
    const bool up = any_up || (L->state == LS_UP && checking);
    if (L->state == LS_UP && !up) {
        if (why < 0) {
            bool one_way = false;
            for (int p = LP_LORA; p <= LP_RELAY; p++) {
                one_way |= st[p] == PS_THEY_CANT || st[p] == PS_WE_CANT;
            }
            why = one_way ? LOST_ONE_WAY : LOST_SILENCE;
        }
        L->state = LS_LOST;
        L->lost_why = (uint8_t)why;
        emit(L, LEV_LINK_LOST, L->peer, (uint8_t)why);
    } else if (L->state == LS_LOST && up) {
        L->state = LS_UP;
        emit(L, LEV_LINK_RESTORED, L->peer, 0);
    }
}

/*
 * note_heard() -- the partner itself (hop 0) sent packet `p`, which reached
 * us over LoRa or (via_phone) through the phones: that path works from
 * them to us. Over LoRa it also gives the partner's signal readings. A
 * keepalive addressed to us carries their report (how long since they
 * heard us on each path), unless it comes from older firmware (no payload).
 */
static void note_heard(link_t *L, const pkt_t *p, bool via_phone, int16_t rssi, int8_t snr_x4, int64_t now)
{
    L->last_heard = now;
    if (via_phone) {
        L->relay_heard = now;
    } else {
        L->lora_heard = now;
        L->peer_rssi = rssi; /* (a relayed copy says nothing about the radio) */
        L->peer_snr_x4 = snr_x4;
    }
    if (p->type == PT_KEEPALIVE && p->dst == L->own && p->plen >= KA_REPORT_LEN) {
        L->rep_ever = true;
        L->rep_at = now;
        L->rep_lora_s = p->payload[0];
        L->rep_relay_s = p->payload[1];
    }
}

/*
 * paths_reset() -- a link (re)starts with `peer`: nothing heard or reported
 * yet on either path, no failure, both paths "checking" (the relay "off"
 * without a phone). No events: the states are reported as they change.
 */
static void paths_reset(link_t *L, uint32_t peer, int64_t now)
{
    L->lora_since = L->relay_since = now;
    L->lora_heard = L->relay_heard = LINK_NEVER;
    L->rep_ever = false;
    L->rep_at = LINK_NEVER;
    L->rep_lora_s = L->rep_relay_s = KA_NEVER;
    L->lora_rep_from = LINK_NEVER;
    L->fail_at = LINK_NEVER;
    L->quick_ka_at = LINK_NEVER;
    L->lost_why = LOST_SILENCE;
    L->path_st[LP_LORA] = peer == ADDR_NONE ? PS_NONE : PS_CHECKING;
    L->path_st[LP_RELAY] = peer == ADDR_NONE ? PS_NONE : L->relay_on ? PS_CHECKING : PS_OFF;
}

/* ---- the message queue -------------------------------------------------- */

static void msg_pump(link_t *L, int64_t now);

/* msg_shift() -- drop msgq[0], moving the rest up. */
static void msg_shift(link_t *L)
{
    memmove(&L->msgq[0], &L->msgq[1], (size_t)(L->n_msg - 1) * sizeof(L->msgq[0]));
    L->n_msg--;
}

/*
 * msg_fail_all() -- the message being delivered got no receipt, so the ones
 * queued behind it won't fare better either: report them all in one
 * LEV_MSG_FAILED, texts joined in order (cut at MSG_MAX_CHARS), so the user
 * hears one error and gets one draft back. If they were for our partner,
 * the partner's earlier reports no longer prove they hear us (fail_at), so
 * the link goes down now with its own event, LEV_LINK_LOST "no-receipt"
 * (the UI plays no second alert for it), and comes back with the next
 * fresh report. (Up to v0.5.1 it went down silently, and was then never
 * reported lost at all: the bug of 15 Oct.)
 */
static void msg_fail_all(link_t *L, int64_t now)
{
    if (!L->n_msg) {
        return;
    }
    uint32_t to = L->msgq[0].to;
    link_evt_t *e = emit(L, LEV_MSG_FAILED, to, 0); /* 0: every message that was waiting */
    size_t n = 0;
    for (int i = 0; i < L->n_msg && n < MSG_MAX_CHARS; i++) {
        size_t k = strlen(L->msgq[i].text);
        if (n) {
            e->text[n++] = ' ';
        }
        if (n + k > MSG_MAX_CHARS) {
            k = MSG_MAX_CHARS - n;
        }
        memcpy(e->text + n, L->msgq[i].text, k);
        n += k;
    }
    e->text[n] = '\0';
    ob_cancel(L, OB_MSG, ANY_ADDR, 0);
    L->n_msg = 0;
    L->msg_inflight = false;
    if (to == L->peer && (L->state == LS_UP || L->state == LS_LOST)) {
        L->fail_at = now;
        link_eval(L, now, LOST_NO_RECEIPT); /* up -> lost, reported once (already lost: nothing more) */
    }
}

/*
 * msg_pump() -- if no message is being delivered and one is waiting, hand
 * msgq[0] to the transmit queue (LINK_TRIES copies, then wait for the
 * receipt). Gives every message its own ID at this point.
 */
static void msg_pump(link_t *L, int64_t now)
{
    while (L->n_msg && !L->msg_inflight) {
        link_msg_t *m = &L->msgq[0];
        m->id = new_id(L);
        pkt_t p;
        proto_init_pkt(&p, PT_MSG, L->own, m->to, m->id);
        p.flags = PF_LONG_PREAMBLE;
        size_t n = strlen(m->text);
        memcpy(p.payload, m->text, n);
        p.plen = (uint8_t)n;
        if (ob_add(L, OB_MSG, &p, LINK_TRIES, L->tm.retry_gap_ms, L->tm.reply_wait_ms, 0, L->tm.hard_ms, false, now)) {
            L->msg_inflight = true;
        } else {
            msg_fail_all(L, now); /* queue full or bad address: fail now rather than never */
        }
    }
}

/* msg_drop_to() -- silently discard every message for `addr` (we forgot
 * them), then carry on with the next one for someone else, if any. */
static void msg_drop_to(link_t *L, uint32_t addr, int64_t now)
{
    int w = 0;
    for (int r = 0; r < L->n_msg; r++) {
        if (L->msgq[r].to == addr) {
            if (r == 0 && L->msg_inflight) {
                ob_cancel(L, OB_MSG, addr, L->msgq[0].id);
                L->msg_inflight = false;
            }
            continue;
        }
        if (w != r) {
            L->msgq[w] = L->msgq[r];
        }
        w++;
    }
    L->n_msg = (uint8_t)w;
    msg_pump(L, now);
}

/* ---- changing partner ---------------------------------------------------- */

/*
 * change_peer() -- make `peer` our partner (ADDR_NONE: none).
 * Side effects, in order:
 *  - the previous partner, if the link with them was still up (or lost),
 *    gets an ENDED notice (if it misses it, its next keepalive is answered
 *    ENDED: see on_keepalive);
 *  - the new link starts "up", with both paths "checking" (paths_reset)
 *    and the next keepalive a period away; a new partner has no message
 *    yet (so we're not busy), while re-confirming the same partner keeps
 *    the conversation's clock;
 *  - our open request is closed: either it was to them (done) or it has
 *    been superseded ("the last selected recipient" wins);
 *  - an accept in progress for someone else is abandoned (user told);
 *  - a request from the new partner waiting for our user is moot.
 */
static void change_peer(link_t *L, uint32_t peer, int64_t now)
{
    uint32_t old = L->peer;
    if (old != ADDR_NONE && old != peer) {
        ob_cancel(L, OB_KEEPALIVE, old, 0); /* a stale keepalive would keep their link alive */
        if (L->state == LS_UP || L->state == LS_LOST) {
            pkt_t p;
            proto_init_pkt(&p, PT_LINK_REPLY, L->own, old, new_id(L));
            p.payload[0] = LR_ENDED;
            proto_put16(&p.payload[1], 0);
            p.plen = 3;
            send_notice(L, &p, now);
            ex_add(L, old, false, now);
        }
    }
    if (peer != old) {
        L->last_msg = LINK_NEVER;
        L->peer_rssi = 0;
        L->peer_snr_x4 = 0;
    }
    L->peer = peer;
    L->state = (peer == ADDR_NONE) ? LS_NONE : LS_UP;
    L->last_heard = now;
    paths_reset(L, peer, now);
    L->ka_due = now + ka_interval(L);
    if (peer != ADDR_NONE) {
        ex_remove(L, peer);
        pend_drop(L, peer, true);
    }
    req_close(L);
    if (L->acc_active && L->acc_to != peer) {
        acc_cancel(L, true);
    }
}

/*
 * forget() -- drop everything to do with `addr`: the link if it was our
 * partner (without an ENDED notice: an UNPAIR says more), our request to
 * them, our accept of them, their request to us, queued messages to them.
 * No events.
 */
static void forget(link_t *L, uint32_t addr, int64_t now)
{
    if (L->peer == addr) {
        ob_cancel(L, OB_KEEPALIVE, addr, 0);
        L->peer = ADDR_NONE;
        L->state = LS_NONE;
        L->path_st[LP_LORA] = L->path_st[LP_RELAY] = PS_NONE;
    }
    if (L->req_phase != REQ_IDLE && L->req_to == addr) {
        req_close(L);
    }
    if (L->acc_active && L->acc_to == addr) {
        acc_cancel(L, false);
    }
    pend_drop(L, addr, false);
    ex_remove(L, addr);
    msg_drop_to(L, addr, now);
}

/* ========================================================================= */
/* 4. Set-up and user actions                                                 */
/* ========================================================================= */

/* air_ms() -- airtime of `len` bytes at setting `lp`, whole ms (rounded up). */
static uint32_t air_ms(const lora_params_t *lp, int len)
{
    return (lora_airtime_us(len, lp, RADIO_PREAMBLE_LONG) + 999u) / 1000u;
}

/* at_least() -- the larger of two durations. */
static uint32_t at_least(uint32_t v, uint32_t floor)
{
    return v > floor ? v : floor;
}

/*
 * link_timing() -- see link.h. Each value is today's SF9 constant, or what
 * the airtime needs if that is more. The rules, with the SF12 result:
 *   keepalive     30 s up to SF10, 45 s at SF11, 60 s at SF12 (config.h);
 *                 jitter and lost time in proportion: 8 s, 180 s
 *   retry gap     enough for the receipt to come back after a copy, with
 *                 room for the other side's own packet: 800 ms + 2.5
 *                 receipts' airtime (1.6 s floor) -> 3.7 s; jitter half the
 *                 gap; the wait after the last copy 0.4 s more than the gap
 *   hard limit    five copies of the longest message with their gaps, the
 *                 wait and 5 s to spare (30 s floor) -> 58 s
 *   answers       an answer later than a retry gap and a half (+1 s) is
 *                 useless (5 s floor) -> 6.5 s
 *   listen-before-talk  hold back at most one longest packet (+0.3 s;
 *                 1.5 s floor) -> 4.6 s
 *   PONG spread, SOS relay pause, discovery send limit and late answers:
 *                 a few packets' airtime each, floors as before.
 */
void link_timing(const lora_params_t *lp, link_timing_t *t)
{
    const uint32_t rcpt = air_ms(lp, PROTO_HDR_LEN + 2);            /* a receipt or confirm: 144 ms at SF9 */
    const uint32_t bare = air_ms(lp, PROTO_HDR_LEN);                /* a bare header (SOS): 144 ms */
    const uint32_t pong = air_ms(lp, PROTO_HDR_LEN + 4);            /* a PONG: 165 ms */
    const uint32_t pres = air_ms(lp, PROTO_HDR_LEN + DISC_PRESENCE_LEN); /* a PRESENCE: 144 ms */
    const uint32_t disc = air_ms(lp, PROTO_HDR_LEN + DISC_DISCOVER_LEN); /* a DISCOVER: 144 ms */
    const uint32_t big = air_ms(lp, PROTO_HDR_LEN + MSG_MAX_CHARS); /* a 100-character message: 595 ms */
    t->keepalive_ms = lp->sf >= 12 ? RADIO_KA_SF12_MS : lp->sf == 11 ? RADIO_KA_SF11_MS : KEEPALIVE_MS;
    t->ka_jitter_ms = (uint32_t)((uint64_t)t->keepalive_ms * KEEPALIVE_JITTER_MS / KEEPALIVE_MS);
    t->lost_ms = (uint32_t)((uint64_t)t->keepalive_ms * LINK_LOST_MS / KEEPALIVE_MS);
    t->quick_ka_gap_ms = t->keepalive_ms / 3;
    const uint32_t gap = at_least(800 + rcpt * 5 / 2, RETRY_GAP_MS);
    t->retry_gap_ms = (uint16_t)gap;
    t->retry_jitter_ms = (uint16_t)(gap * RETRY_JITTER_MS / RETRY_GAP_MS);
    t->reply_wait_ms = (uint16_t)(gap + (REPLY_WAIT_MS - RETRY_GAP_MS));
    const uint32_t per_copy = big + gap + t->retry_jitter_ms;
    t->hard_ms = at_least(LINK_TRIES * per_copy + t->reply_wait_ms + 5000, LINK_HARD_TIMEOUT_MS);
    t->reply_hard_ms = at_least(gap + t->retry_jitter_ms + 1000, 5000);
    t->lbt_max_ms = at_least(big + 300, 1500);
    t->pong_spread_ms = at_least(4 * pong, 1000);
    t->sos_relay_max_ms = at_least(6 * bare, SOS_RELAY_MAX_MS);
    t->relay_hard_ms = at_least(t->sos_relay_max_ms + t->lbt_max_ms + 2 * bare, 10000);
    t->disc_send_hard_ms = at_least(t->lbt_max_ms + 3 * disc, DISC_SEND_HARD_MS);
    t->disc_late_ms = at_least(pres + 200, DISC_LATE_MS);
    t->max_pkt_ms = big;
}

/* slot_for() -- the sweep's answer slot at setting `lp`: a PRESENCE's
 * airtime plus DISC_SLOT_GUARD_MS, in whole 10 ms (as the DISCOVER carries it). */
static uint16_t slot_for(const lora_params_t *lp)
{
    uint32_t ms = air_ms(lp, PROTO_HDR_LEN + DISC_PRESENCE_LEN) + DISC_SLOT_GUARD_MS;
    return (uint16_t)((ms + 9) / 10 * 10);
}

/* link_disc_window_ms() -- see link.h: the DISCOVER itself, then the turn
 * round, the slots and the stragglers' time (about 3.7 s at SF9, 21 s at
 * SF12). */
uint32_t link_disc_window_ms(const lora_params_t *lp)
{
    link_timing_t t;
    link_timing(lp, &t);
    return air_ms(lp, PROTO_HDR_LEN + DISC_DISCOVER_LEN) + DISC_TURN_MS + DISC_SLOTS * (uint32_t)slot_for(lp) +
           t.disc_late_ms;
}

/* link_init() -- see link.h. Everything starts empty: no partner, no
 * requests, nothing queued, no phone to relay; message IDs resume from
 * `msgid_stored`. */
void link_init(link_t *L, uint32_t own, const lora_params_t *lp, uint32_t seed, uint16_t msgid_stored,
               msgid_save_fn save, void *save_ctx, int64_t now)
{
    memset(L, 0, sizeof(*L));
    L->own = own;
    L->lora = *lp;
    link_timing(lp, &L->tm);
    paths_reset(L, ADDR_NONE, now); /* (the "never" times: 0 would mean "at boot") */
    L->tx_dbm = RADIO_TX_DBM_DEFAULT;
    L->rng = seed ? seed : 0x12345678u;
    L->peer = ADDR_NONE;
    L->state = LS_NONE;
    L->last_msg = LINK_NEVER;
    L->req_to = ADDR_NONE;
    L->acc_to = ADDR_NONE;
    L->inflight = -1;
    for (int i = 0; i < LINK_EX_N; i++) {
        L->ex[i].addr = ADDR_NONE; /* 0 would be a real address ("222") */
    }
    for (int i = 0; i < LINK_SOSREL_N; i++) {
        L->sosrel[i].addr = ADDR_NONE;
    }
    L->discoverable = true; /* silent mode is the user's setting: link_set_discoverable() */
    disc_limit_init(&L->disc_lim);
    dc_init(&L->dc);
    dedup_init(&L->dd);
    msgid_init(&L->ids, msgid_stored, save, save_ctx);
}

/* link_set_tx_dbm() -- our transmit power, quoted in PING / PONG. */
void link_set_tx_dbm(link_t *L, int8_t dbm)
{
    L->tx_dbm = dbm;
}

/* link_set_blocklist() -- replace our copy of the user's blocklist (at most
 * BLOCK_MAX entries). */
void link_set_blocklist(link_t *L, const uint32_t *list, int n)
{
    if (n > BLOCK_MAX) {
        n = BLOCK_MAX;
    }
    memcpy(L->blocked, list, (size_t)n * sizeof(uint32_t));
    L->n_blocked = (uint8_t)n;
}

/* link_set_discoverable() -- silent mode (Z) off (true) or on (false): whether
 * other devices' sweeps get an answer from us. Our own sweeps work either
 * way. */
void link_set_discoverable(link_t *L, bool on)
{
    L->discoverable = on;
}

/*
 * link_set_relay() -- see link.h. A phone coming back restarts the relay's
 * "checking" time (what was heard through it before still counts, so a
 * short Bluetooth drop shows no gap); one going away makes the relay "off"
 * at once, which brings the link down if LoRa isn't working either.
 */
void link_set_relay(link_t *L, bool on, int64_t now)
{
    if (L->relay_on == on) {
        return;
    }
    L->relay_on = on;
    if (on) {
        L->relay_since = now;
    }
    link_eval(L, now, -1);
}

/*
 * link_set_lora() -- see link.h. Everything heard over LoRa, and every
 * report about LoRa, from before the change no longer counts: LoRa is
 * "checking" for the (new) lost time, and our next keepalive goes out
 * within a few seconds, so a partner already on the new setting hears us
 * at once. A link that was up stays up meanwhile (link_eval), which leaves
 * time to change the other device too.
 */
void link_set_lora(link_t *L, const lora_params_t *lp, int64_t now)
{
    L->lora = *lp;
    link_timing(lp, &L->tm);
    L->lora_since = now;
    L->lora_heard = LINK_NEVER;
    L->lora_rep_from = now;
    L->peer_rssi = 0;
    L->peer_snr_x4 = 0;
    if (L->peer != ADDR_NONE) {
        L->ka_due = now + 1000 + rnd_upto(L, 4000);
    }
    link_eval(L, now, -1);
}

/*
 * link_set_peer() -- see link.h. Choosing the partner we already have (and
 * are still linked to) changes nothing. Otherwise the new link starts as
 * "up" and we say hello within a few seconds, so that a partner who lost
 * us while we were off (or busy elsewhere) hears us again quickly.
 */
void link_set_peer(link_t *L, uint32_t peer, int64_t now)
{
    if (peer == L->peer && (L->state == LS_UP || L->state == LS_LOST)) {
        return;
    }
    change_peer(L, peer, now);
    if (peer != ADDR_NONE) {
        L->ka_due = now + 1000 + rnd_upto(L, 4000);
    }
}

/*
 * link_request() -- S / P: open a request to `to` (see link.h). Closes any
 * older request ("the newest wins") and abandons an accept in progress for
 * someone else. Queues LINK_TRIES copies of LINK_REQ; the answers are
 * handled in on_link_reply(), the silence in exhausted().
 */
bool link_request(link_t *L, uint32_t to, int64_t now)
{
    if (L->own == ADDR_NONE || !proto_addr_valid(to) || to == L->own) {
        emit(L, LEV_REQ_RESULT, to, LRES_UNAVAILABLE);
        return false;
    }
    req_close(L); /* only one request open at a time: the newest wins */
    if (L->acc_active && L->acc_to != to) {
        /* The user changed their mind about the request they were accepting.
         * They may already have heard our ACCEPT: if so, their keepalive
         * will be answered "ended". */
        ex_add(L, L->acc_to, false, now);
        acc_cancel(L, false);
    }
    L->req_to = to;
    L->req_id = new_id(L);
    L->req_phase = REQ_SENDING;
    L->req_opened = now;
    pkt_t p;
    proto_init_pkt(&p, PT_LINK_REQ, L->own, to, L->req_id);
    p.flags = PF_LONG_PREAMBLE;
    if (!ob_add(L, OB_REQ, &p, LINK_TRIES, L->tm.retry_gap_ms, L->tm.reply_wait_ms, 0, L->tm.hard_ms, false, now)) {
        L->req_phase = REQ_IDLE;
        emit(L, LEV_REQ_RESULT, to, LRES_UNAVAILABLE);
        return false;
    }
    return true;
}

/*
 * link_accept() -- A: accept the pending request from `from`. Queues
 * LINK_TRIES copies of LINK_REPLY ACCEPT; the link only moves when the
 * requester's PAIR_CONFIRM arrives (on_confirm), so a lost accept never
 * strands our current partner.
 */
bool link_accept(link_t *L, uint32_t from, int64_t now)
{
    int i = pend_find(L, from);
    if (L->own == ADDR_NONE || i < 0) {
        emit(L, LEV_ACCEPT_RESULT, from, 0);
        return false;
    }
    uint16_t ref = L->pend[i].ref;
    pend_remove_at(L, i);
    acc_cancel(L, true); /* one accept at a time */
    L->acc_active = true;
    L->acc_to = from;
    L->acc_id = new_id(L);
    pkt_t p;
    proto_init_pkt(&p, PT_LINK_REPLY, L->own, from, L->acc_id);
    p.flags = PF_LONG_PREAMBLE; /* they asked a while ago and may be asleep (Phase 2) */
    p.payload[0] = LR_ACCEPT;
    proto_put16(&p.payload[1], ref);
    p.plen = 3;
    if (!ob_add(L, OB_ACCEPT, &p, LINK_TRIES, L->tm.retry_gap_ms, L->tm.reply_wait_ms, 0, L->tm.hard_ms, false, now)) {
        L->acc_active = false;
        emit(L, LEV_ACCEPT_RESULT, from, 0);
        return false;
    }
    return true;
}

/* link_decline() -- N: drop the pending request from `from` and send
 * DECLINE (NOTICE_COPIES copies, quoting their request's ID). */
bool link_decline(link_t *L, uint32_t from, int64_t now)
{
    int i = pend_find(L, from);
    if (i < 0) {
        return false;
    }
    uint16_t ref = L->pend[i].ref;
    pend_remove_at(L, i);
    if (L->own != ADDR_NONE) {
        pkt_t p;
        proto_init_pkt(&p, PT_LINK_REPLY, L->own, from, new_id(L));
        p.payload[0] = LR_DECLINE;
        proto_put16(&p.payload[1], ref);
        p.plen = 3;
        send_notice(L, &p, now);
    }
    return true;
}

/*
 * link_unpair() -- see link.h. Besides the UNPAIR notice, they go on the
 * "ex" list marked unpaired: if they missed both copies, their next
 * keepalive is answered with UNPAIR again.
 */
void link_unpair(link_t *L, uint32_t addr, int64_t now)
{
    forget(L, addr, now);
    if (L->own == ADDR_NONE || !proto_addr_valid(addr)) {
        return;
    }
    pkt_t p;
    proto_init_pkt(&p, PT_UNPAIR, L->own, addr, new_id(L));
    send_notice(L, &p, now);
    ex_add(L, addr, true, now);
}

/*
 * send_msg() -- link_send_msg() and link_send_test_msg(): queue `text` for
 * our partner `to`. Refused (with LEV_MSG_FAILED, arg 1 = just this one, so
 * the user gets the text back; false) unless `to` is our partner, the link is
 * up and the queue has room. With even_if_lost (test mode's point test) a
 * lost link (LS_LOST) is accepted too, while an ended link or another partner
 * still refuses. Sending a message also makes us busy for LINK_BUSY_MS.
 */
static bool send_msg(link_t *L, uint32_t to, const char *text, int64_t now, bool even_if_lost)
{
    bool linked = L->state == LS_UP || (even_if_lost && L->state == LS_LOST);
    bool usable = L->own != ADDR_NONE && to != ADDR_NONE && to == L->peer && linked;
    if (!usable || !text || !*text || L->n_msg >= MSG_QUEUE_MAX + 1) {
        link_evt_t *e = emit(L, LEV_MSG_FAILED, to, 1); /* 1: just this one (the queue is untouched) */
        if (text) {
            strncpy(e->text, text, MSG_MAX_CHARS);
        }
        return false;
    }
    link_msg_t *m = &L->msgq[L->n_msg++];
    m->to = to;
    m->id = 0;
    strncpy(m->text, text, MSG_MAX_CHARS);
    m->text[MSG_MAX_CHARS] = '\0';
    L->last_msg = now; /* a message sent on the link: we are busy (spec §7) */
    msg_pump(L, now);
    return true;
}

/* link_send_msg() -- see link.h: the user's messages (the link must be up). */
bool link_send_msg(link_t *L, uint32_t to, const char *text, int64_t now)
{
    return send_msg(L, to, text, now, false);
}

/* link_send_test_msg() -- see link.h: test mode's point test, also while the
 * link is lost. */
bool link_send_test_msg(link_t *L, uint32_t to, const char *text, int64_t now)
{
    return send_msg(L, to, text, now, true);
}

/* sos_out() -- queue one of our own SOS-family packets: duty-cycle exempt,
 * `copies` copies with the same ID (receivers act on it once). */
static void sos_out(link_t *L, uint8_t type, uint32_t dst, uint8_t copies, int64_t now)
{
    if (L->own == ADDR_NONE) {
        return;
    }
    pkt_t p;
    proto_init_pkt(&p, type, L->own, dst, new_id(L));
    p.flags = PF_LONG_PREAMBLE; /* hop count 0 */
    ob_add(L, OB_SOS, &p, copies, L->tm.retry_gap_ms, 0, 0, 0, true, now);
}

/* link_send_sos() -- our SOS, or a resend of it. A resend also extends the
 * protection of the responders' links to us (see on_keepalive). */
void link_send_sos(link_t *L, uint32_t dst, int64_t now)
{
    for (int i = 0; i < LINK_SOSREL_N; i++) {
        if (L->sosrel[i].addr != ADDR_NONE && now < L->sosrel[i].until) {
            L->sosrel[i].until = now + LINK_SOSREL_MS;
        }
    }
    sos_out(L, PT_SOS, dst, SOS_COPIES, now);
}

/* link_send_sos_cancel() -- our SOS is over: its responders are ordinary
 * devices again (a keepalive from one that isn't our partner gets ENDED). */
void link_send_sos_cancel(link_t *L, uint32_t dst, int64_t now)
{
    for (int i = 0; i < LINK_SOSREL_N; i++) {
        L->sosrel[i].addr = ADDR_NONE;
    }
    sos_out(L, PT_SOS_CANCEL, dst, SOS_CANCEL_COPIES, now);
}

/* link_send_sos_ack() -- we acknowledge `dst`'s SOS (SOS_COPIES copies). */
void link_send_sos_ack(link_t *L, uint32_t dst, int64_t now)
{
    sos_out(L, PT_SOS_ACK, dst, SOS_COPIES, now);
}

/* link_ping() -- bench tool: broadcast a PING carrying our transmit power. */
void link_ping(link_t *L, int64_t now)
{
    if (L->own == ADDR_NONE) {
        return;
    }
    pkt_t p;
    proto_init_pkt(&p, PT_PING, L->own, ADDR_BROADCAST, new_id(L));
    p.flags = PF_LONG_PREAMBLE;
    p.payload[0] = PING_KIND_PING;
    p.payload[3] = (uint8_t)L->tx_dbm;
    p.plen = 4;
    ob_add(L, OB_PING, &p, 1, 0, 0, 0, 10000, false, now);
}

/* ---- discovery: our own sweep (D) ---------------------------------------- */

/*
 * disc_slot_ms() -- the answer slot length our sweeps ask for: a PRESENCE's
 * airtime plus DISC_SLOT_GUARD_MS, rounded up to the 10 ms steps the
 * DISCOVER carries it in (slot_for). 145 + 45 -> 190 ms at SF9 (spec §6:
 * "derived from airtime"), 1.2 s at SF12, so a slower radio setting
 * stretches the window by itself.
 */
static uint16_t disc_slot_ms(const link_t *L)
{
    return slot_for(&L->lora);
}

/*
 * disc_report() -- our sweep is over: put the found list in walking order
 * (near, mid, far; disc.h) and hand it to the user interface in one
 * LEV_DISC_DONE whose arg is the number of devices (0: nobody answered, or
 * the DISCOVER never got out). The engine forgets the sweep.
 */
static void disc_report(link_t *L)
{
    disc_list_sort(&L->disc_found);
    link_evt_t *e = emit(L, LEV_DISC_DONE, ADDR_NONE, L->disc_found.n);
    memcpy(e->found, L->disc_found.e, sizeof(disc_entry_t) * L->disc_found.n);
    L->disc_phase = DISC_IDLE;
}

/*
 * link_discover() -- D: start a sweep (see link.h). Queues one DISCOVER to
 * everyone, carrying the number of slots and their length; the window opens
 * when it has actually left (link_tx_done), since listen-before-talk may
 * hold it back a little. A sweep already queued or listening is dropped
 * without a report: only the newest one reports.
 */
bool link_discover(link_t *L, int64_t now)
{
    ob_cancel(L, OB_DISC, ANY_ADDR, 0); /* an older DISCOVER still waiting to go */
    disc_list_clear(&L->disc_found);
    L->disc_phase = DISC_IDLE;
    if (L->own == ADDR_NONE) {
        disc_report(L); /* no ID: nobody could answer us anyway */
        return false;
    }
    L->disc_id = new_id(L);
    L->disc_slot_ms = disc_slot_ms(L);
    pkt_t p;
    proto_init_pkt(&p, PT_DISCOVER, L->own, ADDR_BROADCAST, L->disc_id);
    p.flags = PF_LONG_PREAMBLE; /* unsolicited: must be able to wake a sleeping receiver (Phase 2) */
    p.payload[0] = DISC_SLOTS;
    p.payload[1] = (uint8_t)(L->disc_slot_ms / 10);
    p.plen = DISC_DISCOVER_LEN;
    if (!ob_add(L, OB_DISC, &p, 1, 0, 0, 0, L->tm.disc_send_hard_ms, false, now)) {
        disc_report(L); /* transmit queue full: report "nobody" rather than nothing */
        return false;
    }
    L->disc_phase = DISC_SENDING;
    return true;
}

/* ========================================================================= */
/* 5. Receiving                                                               */
/* ========================================================================= */

/*
 * on_link_req() -- someone asks to connect.
 * Answers, in order of precedence:
 *  - ACCEPT straight away if they are already our partner, if we are
 *    already accepting them, or if we were asking for them ourselves (then
 *    we link right now and our own request counts as accepted);
 *  - the same answer as before (RECEIVED or BUSY) if their earlier request
 *    is still waiting: a retry, or they asked again; no second alert;
 *  - nothing for a copy of a request we already dealt with (declined), or
 *    when LINK_PENDING_MAX requests are already waiting;
 *  - otherwise RECEIVED and an alert for our user if we're free, or BUSY
 *    and a place in the queue (alerted by link_poll once we're free).
 */
static void on_link_req(link_t *L, const pkt_t *p, bool dup, int64_t now)
{
    uint32_t from = p->src;
    bool linked = from == L->peer && (L->state == LS_UP || L->state == LS_LOST);
    bool accepting = L->acc_active && L->acc_to == from;
    bool mutual = L->req_phase != REQ_IDLE && L->req_to == from;
    if (mutual && !linked) {
        change_peer(L, from, now); /* both want it: done (closes our request) */
        emit(L, LEV_REQ_RESULT, from, LRES_ACCEPTED);
        linked = true;
    }
    if (linked || accepting) {
        send_link_reply(L, from, LR_ACCEPT, p->id, now);
        return;
    }
    int i = pend_find(L, from);
    if (i >= 0) {
        L->pend[i].ref = p->id; /* answers quote their newest request */
        send_link_reply(L, from, L->pend[i].alerted ? LR_RECEIVED : LR_BUSY, p->id, now);
        return;
    }
    if (dup || L->n_pend >= LINK_PENDING_MAX) {
        return;
    }
    link_pend_t *pe = &L->pend[L->n_pend++];
    pe->from = from;
    pe->ref = p->id;
    pe->arrived = now;
    pe->alerted = !link_busy(L, now);
    if (pe->alerted) {
        pe->alerted_at = now;
        emit(L, LEV_REQ_IN, from, 0);
        send_link_reply(L, from, LR_RECEIVED, p->id, now);
    } else {
        send_link_reply(L, from, LR_BUSY, p->id, now);
    }
}

/*
 * on_link_reply() -- an answer from `from`:
 *  RECEIVED  our request reached them: stop retrying, wait silently.
 *  BUSY      stop retrying; the user hears "busy" once; the request stays
 *            open in case they accept it later.
 *  DECLINE   our request is over: the user hears "no".
 *  ACCEPT    if our request to them is open: link, confirm, user hears OK.
 *            If they are already our partner: confirm again (a repeat).
 *            Otherwise we no longer want it: answer ENDED.
 *  ENDED     if we were accepting them: our accept failed. If they are our
 *            partner: they moved on, the link has ended.
 * RECEIVED, BUSY and DECLINE only count if they quote our current request's
 * ID, so a late copy about an older request can't close a newer one.
 */
static void on_link_reply(link_t *L, const pkt_t *p, int64_t now)
{
    uint8_t status = p->payload[0];
    uint16_t ref = proto_get16(&p->payload[1]);
    uint32_t from = p->src;
    bool about_ours = L->req_phase != REQ_IDLE && L->req_to == from;
    switch (status) {
    case LR_RECEIVED:
        if (about_ours && L->req_phase == REQ_SENDING && ref == L->req_id) {
            ob_cancel(L, OB_REQ, from, 0);
            L->req_phase = REQ_WAITING;
        }
        break;
    case LR_BUSY:
        if (about_ours && L->req_phase != REQ_QUEUED && ref == L->req_id) {
            ob_cancel(L, OB_REQ, from, 0);
            L->req_phase = REQ_QUEUED;
            emit(L, LEV_REQ_RESULT, from, LRES_BUSY);
        }
        break;
    case LR_DECLINE:
        if (about_ours && ref == L->req_id) {
            req_close(L);
            emit(L, LEV_REQ_RESULT, from, LRES_DECLINED);
        }
        break;
    case LR_ACCEPT:
        if (about_ours) {
            change_peer(L, from, now);
            send_ref_reply(L, PT_PAIR_CONFIRM, from, p->id, now);
            emit(L, LEV_REQ_RESULT, from, LRES_ACCEPTED);
        } else if (from == L->peer && (L->state == LS_UP || L->state == LS_LOST)) {
            send_ref_reply(L, PT_PAIR_CONFIRM, from, p->id, now);
        } else {
            send_link_reply(L, from, LR_ENDED, p->id, now);
        }
        break;
    case LR_ENDED:
        if (L->acc_active && L->acc_to == from) {
            acc_cancel(L, true);
        } else if (from == L->peer && (L->state == LS_UP || L->state == LS_LOST)) {
            L->state = LS_ENDED;
            ob_cancel(L, OB_KEEPALIVE, from, 0);
            emit(L, LEV_LINK_ENDED, from, 0);
        }
        break;
    default:
        break; /* a status from a newer version: ignore */
    }
}

/* on_confirm() -- PAIR_CONFIRM: the requester got our ACCEPT. Only now do we
 * move our link to them (so a lost accept never strands our old partner). */
static void on_confirm(link_t *L, const pkt_t *p, int64_t now)
{
    if (!L->acc_active || L->acc_to != p->src) {
        return; /* a repeat, or about an accept we gave up on */
    }
    ob_cancel(L, OB_ACCEPT, p->src, 0);
    L->acc_active = false;
    change_peer(L, p->src, now);
    emit(L, LEV_ACCEPT_RESULT, p->src, 1);
}

/*
 * on_keepalive() -- a KEEPALIVE addressed to us.
 * From our partner it only proves they're there (already noted). From
 * anyone else it means they believe they're linked to us when we aren't
 * linked to them: they missed our ENDED or UNPAIR notice, or we restarted.
 * Tell them (UNPAIR if we forgot them, ENDED otherwise), at most every
 * EX_REPLY_GAP_MS, so both ends agree again. Two exceptions, where their
 * belief is about to come true or must be honoured:
 *  - we are accepting them right now (our ACCEPT reached them first);
 *  - they acknowledged our SOS, which is still going on: acknowledging
 *    made us their partner without a handshake (spec §10), and they must
 *    be able to message us even though we may keep our own partner.
 */
static void on_keepalive(link_t *L, const pkt_t *p, int64_t now)
{
    uint32_t from = p->src;
    if (from == L->peer || (L->acc_active && L->acc_to == from) || sosrel_has(L, from, now)) {
        return;
    }
    link_ex_t *x = ex_find(L, from, now);
    if (!x) {
        ex_add(L, from, false, now);
        x = ex_find(L, from, now);
        x->last_reply = now - EX_REPLY_GAP_MS; /* first time: remind at once */
    }
    if (now - x->last_reply < EX_REPLY_GAP_MS) {
        return;
    }
    x->last_reply = now;
    if (x->unpaired) {
        pkt_t q;
        proto_init_pkt(&q, PT_UNPAIR, L->own, p->src, new_id(L));
        ob_add(L, OB_REPLY, &q, 1, 0, 0, 0, L->tm.reply_hard_ms, false, now);
    } else {
        send_link_reply(L, p->src, LR_ENDED, 0, now);
    }
}

/* on_msg() -- a message. Every copy is receipted (our previous receipt may
 * have been lost), but only the first is delivered. One from our partner
 * also makes us busy for the next 10 minutes. */
static void on_msg(link_t *L, const pkt_t *p, bool dup, int16_t rssi, int8_t snr_x4, int64_t now)
{
    send_ref_reply(L, PT_MSG_RECEIPT, p->src, p->id, now);
    if (dup) {
        return;
    }
    if (p->src == L->peer) {
        L->last_msg = now;
    }
    link_evt_t *e = emit(L, LEV_MSG, p->src, 0);
    memcpy(e->text, p->payload, p->plen); /* plen <= MSG_MAX_CHARS, checked by proto_decode */
    e->text[p->plen] = '\0';
    e->rssi = rssi;
    e->snr_x4 = snr_x4;
}

/* on_receipt() -- the receipt for msgq[0]? Then it was delivered: tell the
 * user (OK) and start on the next queued message. */
static void on_receipt(link_t *L, const pkt_t *p, int64_t now)
{
    uint16_t ref = proto_get16(p->payload);
    if (!L->msg_inflight || !L->n_msg) {
        return;
    }
    link_msg_t *m = &L->msgq[0];
    if (m->to != p->src || m->id != ref) {
        return; /* a late receipt for a copy already confirmed */
    }
    ob_cancel(L, OB_MSG, m->to, m->id);
    if (m->to == L->peer) {
        L->last_msg = now;
    }
    emit(L, LEV_MSG_SENT, m->to, 0);
    msg_shift(L);
    L->msg_inflight = false;
    msg_pump(L, now);
}

/* on_unpair() -- they forgot us: forget them too and tell the user. */
static void on_unpair(link_t *L, const pkt_t *p, bool dup, int64_t now)
{
    if (dup) {
        return;
    }
    bool was_link = p->src == L->peer && L->state != LS_NONE;
    forget(L, p->src, now);
    emit(L, LEV_UNPAIRED, p->src, was_link ? 1 : 0);
}

/* on_ping() -- a PING gets a PONG saying how well we heard it (after a
 * random pause, so several devices don't answer at the same moment). A
 * PONG for us becomes an event. */
static void on_ping(link_t *L, const pkt_t *p, int16_t rssi, int8_t snr_x4, int64_t now)
{
    if (p->payload[0] == PING_KIND_PING) {
        pkt_t q;
        proto_init_pkt(&q, PT_PING, L->own, p->src, new_id(L));
        q.payload[0] = PING_KIND_PONG;
        q.payload[1] = (uint8_t)(rssi > 0 ? 0 : rssi < -255 ? 255 : -rssi); /* dB below 0 dBm */
        q.payload[2] = (uint8_t)snr_x4;
        q.payload[3] = (uint8_t)L->tx_dbm;
        q.plen = 4;
        ob_add(L, OB_REPLY, &q, 1, 0, 0, rnd_upto(L, L->tm.pong_spread_ms), L->tm.reply_hard_ms, false, now);
    } else if (p->dst == L->own) {
        link_evt_t *e = emit(L, LEV_PONG, p->src, 0);
        e->rssi = rssi;
        e->snr_x4 = snr_x4;
        e->far_rssi = (int16_t)(-(int16_t)p->payload[1]);
        e->far_snr_x4 = (int8_t)p->payload[2];
        e->far_dbm = (int8_t)p->payload[3];
    }
}

/*
 * on_discover() -- someone is sweeping (D). Unless we are in silent mode
 * (Z), or have answered this searcher (or everyone together) often enough
 * lately (disc.h), queue a PRESENCE for a random slot of their window,
 * quoting our transmit power and their sweep's tag. The window's shape comes
 * from their packet, clamped to sane limits so a strange sweep can't make
 * everyone answer at once. The answer is abandoned if it can't leave by the
 * end of its slot plus the late time (tm.disc_late_ms: channel busy, duty cycle used up): by
 * then the searcher has stopped listening. Nothing reaches our user.
 */
static void on_discover(link_t *L, const pkt_t *p, int64_t now)
{
    if (p->dst != ADDR_BROADCAST) {
        return; /* only open sweeps exist so far */
    }
    /* (Order matters: in silent mode the limiter isn't even asked, so it
     * counts nothing; it only counts answers it allows.) */
    if (!L->discoverable || !disc_limit_allow(&L->disc_lim, p->src, now)) {
        L->n_disc_refused++;
        return;
    }
    uint32_t slots = p->payload[0];
    uint32_t slot_ms = (uint32_t)p->payload[1] * 10; /* sent in 10 ms steps */
    slots = slots < 1 ? 1 : slots > DISC_SLOTS_MAX ? DISC_SLOTS_MAX : slots;
    slot_ms = slot_ms < DISC_SLOT_MIN_MS ? DISC_SLOT_MIN_MS : slot_ms > DISC_SLOT_MAX_MS ? DISC_SLOT_MAX_MS : slot_ms;
    /* Slot k starts DISC_TURN_MS + k slots after their DISCOVER ended, which is
     * now: everyone in range heard it end at the same moment. */
    uint32_t delay = DISC_TURN_MS + rnd_upto(L, slots - 1) * slot_ms;
    pkt_t q;
    proto_init_pkt(&q, PT_PRESENCE, L->own, p->src, new_id(L));
    q.payload[0] = (uint8_t)L->tx_dbm;
    q.payload[1] = (uint8_t)p->id; /* the tag: the low byte of their DISCOVER's ID */
    q.plen = DISC_PRESENCE_LEN;
    if (ob_add(L, OB_REPLY, &q, 1, 0, 0, delay, delay + slot_ms + L->tm.disc_late_ms, false, now)) {
        L->n_disc_answered++;
    }
}

/*
 * on_presence() -- an answer to a sweep. It counts only while our own sweep
 * is listening and only with that sweep's tag, so a late answer to an
 * earlier sweep can't slip into this one. The device goes on the found list
 * with the strength we heard it at and the power it says it used; a second
 * answer from the same device is ignored (disc_list_add).
 */
static void on_presence(link_t *L, const pkt_t *p, int16_t rssi)
{
    if (L->disc_phase != DISC_LISTENING || p->dst != L->own || p->payload[1] != (uint8_t)L->disc_id) {
        return;
    }
    disc_list_add(&L->disc_found, p->src, rssi, (int8_t)p->payload[0]);
}

/*
 * on_sos_family() -- SOS, SOS_ACK and SOS_CANCEL.
 * A new one (not a duplicate) that isn't addressed to us alone is relayed
 * once, with the hop count raised, after a random pause, until it has been
 * relayed SOS_MAX_HOPS times. If it is for us (or everyone), the user
 * interface gets an event; the UI already handles repeats sensibly.
 */
static void on_sos_family(link_t *L, const pkt_t *p, bool dup, bool for_us, int hops, int16_t rssi, int8_t snr_x4,
                          int64_t now)
{
    if (dup) {
        return; /* a retry, or a copy someone else relayed */
    }
    if (p->dst != L->own && hops < SOS_MAX_HOPS) {
        pkt_t q = *p;
        q.flags = (uint8_t)((p->flags & ~PF_HOPS_MASK) | (hops + 1));
        uint32_t pause = SOS_RELAY_MIN_MS + rnd_upto(L, L->tm.sos_relay_max_ms - SOS_RELAY_MIN_MS);
        ob_add(L, OB_RELAY, &q, 1, 0, 0, pause, L->tm.relay_hard_ms, true, now);
    }
    if (!for_us) {
        return;
    }
    if (p->type == PT_SOS_ACK) {
        sosrel_add(L, p->src, now); /* a responder: their link to us must keep working */
    }
    uint8_t kind = p->type == PT_SOS ? LEV_SOS : p->type == PT_SOS_ACK ? LEV_SOS_ACK : LEV_SOS_CANCEL;
    link_evt_t *e = emit(L, kind, p->src, (p->type == PT_SOS && p->dst == L->own) ? 1 : 0);
    e->rssi = rssi;
    e->snr_x4 = snr_x4;
}

/*
 * rx_dispatch() -- the part of link_on_rx_via() that acts on a packet:
 *  1. packets for other devices are ignored, except the SOS family (relay);
 *  2. the blocklist silences everything but the SOS family (so a blocked
 *     device can't discover us, and never shows up in our own sweeps);
 *  3. duplicates are flagged (keepalives, pings and discovery don't need
 *     it: repeats are harmless, and a sweep's many answers would only push
 *     real packets out of the duplicate ring);
 *  4. the type's handler.
 */
static void rx_dispatch(link_t *L, const pkt_t *pp, int16_t rssi, int8_t snr_x4, int64_t now)
{
    const pkt_t p = *pp; /* (the handlers take it by pointer and keep nothing) */
    const bool sos = p.type == PT_SOS || p.type == PT_SOS_ACK || p.type == PT_SOS_CANCEL;
    const bool for_us = p.dst == L->own || p.dst == ADDR_BROADCAST;
    const int hops = p.flags & PF_HOPS_MASK;
    if (!for_us && !sos) {
        return;
    }
    if (!sos && is_blocked(L, p.src)) {
        return;
    }
    bool dup = false;
    if (p.type != PT_KEEPALIVE && p.type != PT_PING && p.type != PT_DISCOVER && p.type != PT_PRESENCE) {
        dup = dedup_check(&L->dd, p.src, p.id, now);
        if (dup) {
            L->n_dup++;
        }
    }
    if (sos) {
        on_sos_family(L, &p, dup, for_us, hops, rssi, snr_x4, now);
        return;
    }
    switch (p.type) {
    case PT_LINK_REQ:
        on_link_req(L, &p, dup, now);
        break;
    case PT_LINK_REPLY:
        on_link_reply(L, &p, now); /* every status is idempotent: duplicates are harmless */
        break;
    case PT_PAIR_CONFIRM:
        on_confirm(L, &p, now);
        break;
    case PT_KEEPALIVE:
        on_keepalive(L, &p, now);
        break;
    case PT_MSG:
        on_msg(L, &p, dup, rssi, snr_x4, now);
        break;
    case PT_MSG_RECEIPT:
        on_receipt(L, &p, now);
        break;
    case PT_UNPAIR:
        on_unpair(L, &p, dup, now);
        break;
    case PT_PING:
        on_ping(L, &p, rssi, snr_x4, now);
        break;
    case PT_DISCOVER:
        on_discover(L, &p, now);
        break;
    case PT_PRESENCE:
        on_presence(L, &p, rssi);
        break;
    default:
        break; /* every type that decodes is handled above */
    }
}

/*
 * link_on_rx() -- entry point for every received packet over LoRa
 * (link_on_rx_via() for one that may have come through the phone).
 */
void link_on_rx(link_t *L, const uint8_t *buf, int len, int16_t rssi, int8_t snr_x4, int64_t now)
{
    link_on_rx_via(L, buf, len, rssi, snr_x4, false, now);
}

/*
 * link_on_rx_via() -- link_on_rx() with the path the packet took (link.h).
 * Order:
 *  1. malformed, or our own packet relayed back: dropped; through the
 *     phone, discovery and ping packets too (they are about the radio, and
 *     the phone never relays them anyway);
 *  2. the packet is acted on (rx_dispatch);
 *  3. if it was sent by our partner itself (hop 0), whoever it was for (it
 *     may also be the packet that just made them our partner), that path
 *     works from them to us, and a keepalive brings their report
 *     (note_heard). Only over LoRa does it give their signal readings;
 *  4. the link's status is brought up to date (link_eval): a lost link
 *     comes back only once a path is proven both ways, so a packet from a
 *     partner who can't hear us doesn't restore it.
 */
void link_on_rx_via(link_t *L, const uint8_t *buf, int len, int16_t rssi, int8_t snr_x4, bool via_phone,
                    int64_t now)
{
    if (L->own == ADDR_NONE) {
        return;
    }
    pkt_t p;
    if (proto_decode(buf, len, &p) != PROTO_OK) {
        L->n_bad++;
        return;
    }
    if (p.src == L->own) {
        return;
    }
    if (via_phone && (p.type == PT_DISCOVER || p.type == PT_PRESENCE || p.type == PT_PING)) {
        return;
    }
    L->n_rx++;
    if (via_phone) {
        L->n_rx_phone++;
    }
    rx_dispatch(L, &p, rssi, snr_x4, now);
    if (L->peer != ADDR_NONE && p.src == L->peer && (p.flags & PF_HOPS_MASK) == 0) {
        note_heard(L, &p, via_phone, rssi, snr_x4, now);
    }
    link_eval(L, now, -1);
}

/* ========================================================================= */
/* 6. Timers and the radio interface                                          */
/* ========================================================================= */

/*
 * exhausted() -- a queued packet has sent all its copies and waited
 * (or hit its hard deadline) without the answer that would have removed
 * it. Frees the entry, then reports the failure for the exchanges that
 * expect an answer:
 *   our request   -> "unavailable" (only if nothing at all came back)
 *   our accept    -> accept failed (if they did hear it and now think
 *                    they're linked to us, their keepalive gets ENDED)
 *   our message   -> delivery failed (and everything queued behind it)
 *   our DISCOVER  -> it never got out (channel busy, duty cycle): the sweep
 *                    reports nobody found
 */
static void exhausted(link_t *L, link_ob_t *o, int64_t now)
{
    uint8_t kind = o->kind;
    uint32_t dst = o->dst;
    uint16_t id = o->id;
    o->kind = OB_FREE; /* free first: the handlers below may queue packets */
    switch (kind) {
    case OB_REQ:
        if (L->req_phase == REQ_SENDING && L->req_to == dst && L->req_id == id) {
            L->req_phase = REQ_IDLE;
            emit(L, LEV_REQ_RESULT, dst, LRES_UNAVAILABLE);
        }
        break;
    case OB_ACCEPT:
        if (L->acc_active && L->acc_to == dst) {
            L->acc_active = false;
            ex_add(L, dst, false, now);
            emit(L, LEV_ACCEPT_RESULT, dst, 0);
        }
        break;
    case OB_MSG:
        if (L->msg_inflight && L->n_msg && L->msgq[0].to == dst && L->msgq[0].id == id) {
            msg_fail_all(L, now);
        }
        break;
    case OB_DISC:
        if (L->disc_phase == DISC_SENDING && L->disc_id == id) {
            disc_report(L);
        }
        break;
    default:
        break; /* replies, notices, keepalives, relays: nothing is waiting on them */
    }
}

/*
 * link_poll() -- the engine's clock tick. In order:
 *  - queued packets whose retries ran out (or which are hopelessly late);
 *  - the link: its paths and state (link_eval: what was heard and reported
 *    ages past the lost time); our keepalive when due;
 *  - requests waiting for our user: expire them, and play the queued ones
 *    once we are no longer busy;
 *  - our own open request and old reminder-list entries expire eventually;
 *  - our sweep's listening window closes: report who answered.
 */
void link_poll(link_t *L, int64_t now)
{
    for (int i = 0; i < LINK_OB_N; i++) {
        link_ob_t *o = &L->ob[i];
        if (o->kind == OB_FREE || i == L->inflight) {
            continue;
        }
        if ((o->tries == 0 && now >= o->give_up) || now >= o->hard) {
            exhausted(L, o, now);
        }
    }

    if (L->peer != ADDR_NONE) {
        link_eval(L, now, -1); /* paths go down by time passing, too */
        if ((L->state == LS_UP || L->state == LS_LOST) && now >= L->ka_due && L->own != ADDR_NONE) {
            if (!ob_has(L, OB_KEEPALIVE)) {
                pkt_t p;
                proto_init_pkt(&p, PT_KEEPALIVE, L->own, L->peer, new_id(L));
                p.flags = PF_LONG_PREAMBLE;
                p.payload[0] = p.payload[1] = KA_NEVER; /* the report: filled in as it leaves (link_take_tx) */
                p.plen = KA_REPORT_LEN;
                ob_add(L, OB_KEEPALIVE, &p, 1, 0, 0, 0, L->tm.keepalive_ms, false, now);
            }
            L->ka_due = now + ka_interval(L); /* pushed back again when it is actually sent */
        }
    }

    bool busy = link_busy(L, now);
    for (int i = 0; i < L->n_pend;) {
        link_pend_t *pe = &L->pend[i];
        bool expired = pe->alerted ? now - pe->alerted_at >= LINK_PENDING_MS : now - pe->arrived >= LINK_QUEUED_MS;
        if (expired) {
            uint32_t from = pe->from;
            bool was_alerted = pe->alerted;
            pend_remove_at(L, i);
            if (was_alerted) {
                emit(L, LEV_REQ_GONE, from, 0);
            }
            continue; /* entry i is now the next one */
        }
        if (!pe->alerted && !busy) {
            pe->alerted = true;
            pe->alerted_at = now;
            emit(L, LEV_REQ_IN, pe->from, 0);
        }
        i++;
    }

    if ((L->req_phase == REQ_WAITING || L->req_phase == REQ_QUEUED) && now - L->req_opened >= LINK_REQ_OPEN_MS) {
        req_close(L);
    }
    for (int i = 0; i < LINK_EX_N; i++) {
        if (L->ex[i].addr != ADDR_NONE && now >= L->ex[i].until) {
            L->ex[i].addr = ADDR_NONE;
        }
    }
    if (L->disc_phase == DISC_LISTENING && now >= L->disc_end) {
        disc_report(L);
    }
}

/*
 * link_take_tx() -- choose the next packet to put on air.
 * Among queued packets with copies left whose time has come, the lowest
 * kind wins (SOS first, keepalives and pings last), then the one waiting
 * longest. Packets that would overrun the duty-cycle budget are skipped,
 * unless they are SOS traffic. A keepalive gets its report written in
 * now, so it is as fresh as it can be. See link.h for the arguments.
 */
bool link_take_tx(link_t *L, int64_t now, uint8_t *buf, int *len, bool *long_preamble)
{
    if (L->own == ADDR_NONE || L->inflight >= 0) {
        return false;
    }
    int best = -1;
    for (int i = 0; i < LINK_OB_N; i++) {
        link_ob_t *o = &L->ob[i];
        if (o->kind == OB_FREE || o->tries == 0 || o->due > now) {
            continue;
        }
        if (!o->exempt) {
            uint32_t air = pkt_airtime_ms(L, o->len, (o->pkt[1] & PF_LONG_PREAMBLE) != 0);
            if (!dc_allow(&L->dc, now, air)) {
                continue; /* over budget: it waits (and fails at its hard deadline) */
            }
        }
        if (best >= 0) {
            const link_ob_t *b = &L->ob[best];
            if (o->kind > b->kind || (o->kind == b->kind && o->due >= b->due)) {
                continue;
            }
        }
        best = i;
    }
    if (best < 0) {
        return false;
    }
    link_ob_t *o = &L->ob[best];
    if (o->kind == OB_KEEPALIVE && o->len >= PROTO_HDR_LEN + KA_REPORT_LEN) {
        /* Our report, as of this moment: how long since we heard the
         * partner over LoRa, and through the phones. */
        o->pkt[PROTO_HDR_LEN] = heard_byte(L->lora_heard, now);
        o->pkt[PROTO_HDR_LEN + 1] = heard_byte(L->relay_heard, now);
    }
    memcpy(buf, o->pkt, o->len);
    *len = o->len;
    *long_preamble = (o->pkt[1] & PF_LONG_PREAMBLE) != 0;
    o->tries--;
    L->inflight = (int8_t)best;
    L->inflight_ms = pkt_airtime_ms(L, o->len, *long_preamble);
    return true;
}

/*
 * link_tx_done() -- the packet handed out by link_take_tx() has left.
 *  - Its airtime is counted against the duty cycle.
 *  - A keepalive: the next one is a full period away. (Other packets no
 *    longer push it back, since v0.6.0: the partner needs our report
 *    every period, whatever else we send.)
 *  - Our DISCOVER: the sweep's listening window starts now.
 *  - Copies left: the next one waits gap + jitter. None left: either wait
 *    for the answer (wait_ms) or forget the packet.
 *  - Not sent (radio trouble): the copy is retried shortly.
 */
void link_tx_done(link_t *L, int64_t now, bool sent)
{
    if (L->inflight < 0) {
        return;
    }
    link_ob_t *o = &L->ob[L->inflight];
    L->inflight = -1;
    if (sent) {
        dc_add(&L->dc, now, L->inflight_ms);
        L->n_tx++;
    }
    if (o->kind == OB_FREE) {
        return; /* answered or cancelled while it was on air */
    }
    if (!sent) {
        o->tries++;
        o->due = now + TX_RETRY_MS;
        return;
    }
    if (o->kind == OB_KEEPALIVE && L->peer != ADDR_NONE) {
        L->ka_due = now + ka_interval(L); /* only a keepalive carries the report: others don't push it back */
    }
    if (o->kind == OB_DISC && L->disc_phase == DISC_SENDING && o->id == L->disc_id) {
        /* Everyone in range heard it end at this moment, and times its answer
         * slot from it: listen through the last slot, plus stragglers. */
        L->disc_phase = DISC_LISTENING;
        L->disc_end = now + DISC_TURN_MS + (int64_t)DISC_SLOTS * L->disc_slot_ms + L->tm.disc_late_ms;
    }
    if (o->tries == 0) {
        if (o->wait_ms == 0) {
            o->kind = OB_FREE;
        } else {
            o->give_up = now + o->wait_ms;
        }
    } else {
        o->due = now + o->gap_ms + rnd_upto(L, L->tm.retry_jitter_ms);
    }
}

/*
 * link_next_due() -- the earliest moment link_take_tx() or link_poll()
 * could have something to do. Pure; used to decide how long to sleep.
 */
int64_t link_next_due(const link_t *L, int64_t now)
{
    int64_t t = INT64_MAX;
#define EARLIER(x)      \
    do {                \
        if ((x) < t) {  \
            t = (x);    \
        }               \
    } while (0)
    for (int i = 0; i < LINK_OB_N; i++) {
        const link_ob_t *o = &L->ob[i];
        if (o->kind == OB_FREE || i == L->inflight) {
            continue;
        }
        EARLIER(o->tries ? o->due : o->give_up);
        EARLIER(o->hard);
    }
    if (L->peer != ADDR_NONE && (L->state == LS_UP || L->state == LS_LOST)) {
        EARLIER(L->ka_due);
        /* A path's state changes when something heard or reported, or its
         * "checking" time, runs out (only future moments count: one in the
         * past has happened already). */
        const int64_t lost = L->tm.lost_ms;
        int64_t ends[5] = {L->lora_heard, L->relay_heard, L->lora_since, L->relay_since, LINK_NEVER};
        if (L->rep_at != LINK_NEVER) {
            uint8_t r = L->rep_lora_s < L->rep_relay_s ? L->rep_relay_s : L->rep_lora_s;
            ends[4] = L->rep_at - (int64_t)r * 1000;
        }
        for (int i = 0; i < 5; i++) {
            if (ends[i] != LINK_NEVER && ends[i] + lost > now) {
                EARLIER(ends[i] + lost);
            }
        }
        if (L->rep_at != LINK_NEVER) {
            uint8_t r = L->rep_lora_s < L->rep_relay_s ? L->rep_lora_s : L->rep_relay_s;
            if (L->rep_at - (int64_t)r * 1000 + lost > now) {
                EARLIER(L->rep_at - (int64_t)r * 1000 + lost);
            }
        }
    }
    for (int i = 0; i < L->n_pend; i++) {
        const link_pend_t *pe = &L->pend[i];
        if (pe->alerted) {
            EARLIER(pe->alerted_at + LINK_PENDING_MS);
        } else {
            EARLIER(pe->arrived + LINK_QUEUED_MS);
            if (L->last_msg != LINK_NEVER) {
                EARLIER(L->last_msg + LINK_BUSY_MS);
            }
        }
    }
    if (L->req_phase == REQ_WAITING || L->req_phase == REQ_QUEUED) {
        EARLIER(L->req_opened + LINK_REQ_OPEN_MS);
    }
    if (L->disc_phase == DISC_LISTENING) {
        EARLIER(L->disc_end);
    }
#undef EARLIER
    return t < now ? now : t;
}

/* link_get_event() -- take the oldest event, if any. */
bool link_get_event(link_t *L, link_evt_t *e)
{
    if (!L->ev_n) {
        return false;
    }
    *e = L->ev[L->ev_head];
    L->ev_head = (uint8_t)((L->ev_head + 1) % LINK_EV_N);
    L->ev_n--;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Queries                                                                    */
/* ------------------------------------------------------------------------- */

/* link_busy() -- a message to / from our partner in the last 10 minutes
 * while the link is up (spec §7); requests then get BUSY and wait. */
bool link_busy(const link_t *L, int64_t now)
{
    return L->peer != ADDR_NONE && L->state == LS_UP && L->last_msg != LINK_NEVER &&
           now - L->last_msg < LINK_BUSY_MS;
}

/* link_duty_used_ms() -- airtime used in the last hour, ms. */
uint32_t link_duty_used_ms(const link_t *L, int64_t now)
{
    return dc_used_ms(&L->dc, now);
}

/* link_add_airtime() -- see link.h. */
void link_add_airtime(link_t *L, int64_t now, uint32_t ms)
{
    dc_add(&L->dc, now, ms);
}

/* link_new_id() -- see link.h. */
uint16_t link_new_id(link_t *L)
{
    return new_id(L);
}

/* link_path_state() -- see link.h. */
uint8_t link_path_state(const link_t *L, uint8_t path, int64_t now)
{
    return path_state(L, path > LP_RELAY ? LP_RELAY : path, now);
}

/* link_heard_ago_s() -- see link.h. */
int32_t link_heard_ago_s(const link_t *L, uint8_t path, int64_t now)
{
    int64_t t = path == LP_LORA ? L->lora_heard : L->relay_heard;
    return (L->peer == ADDR_NONE || t == LINK_NEVER) ? -1 : (int32_t)((now - t) / 1000);
}

/* link_they_heard_s() -- see link.h. */
int32_t link_they_heard_s(const link_t *L, uint8_t path, int64_t now)
{
    uint8_t r = path == LP_LORA ? L->rep_lora_s : L->rep_relay_s;
    if (L->peer == ADDR_NONE || L->rep_at == LINK_NEVER || r == KA_NEVER) {
        return -1;
    }
    return (int32_t)(r + (now - L->rep_at) / 1000);
}

/* Printable names for the console and logs. */
const char *link_state_name(uint8_t s)
{
    static const char *const n[] = {"none", "connected", "LOST", "ended"};
    return s < 4 ? n[s] : "?";
}

/* link_req_phase_name() -- our outgoing request's phase (link_req_phase_t). */
const char *link_req_phase_name(uint8_t p)
{
    static const char *const n[] = {"none", "sending", "waiting for their answer", "queued (they're busy)"};
    return p < 4 ? n[p] : "?";
}

/* link_result_name() -- the outcome of a request (LRES_*). */
const char *link_result_name(uint8_t r)
{
    static const char *const n[] = {"accepted", "busy", "declined", "unavailable"};
    return r < 4 ? n[r] : "?";
}

/* link_event_name() -- an event kind (LEV_*), as the net log prints it. */
const char *link_event_name(uint8_t kind)
{
    static const char *const n[] = {"-",           "MSG",      "MSG_SENT",   "MSG_FAILED", "REQ_IN",
                                    "REQ_GONE",    "REQ_RESULT", "ACCEPT_RESULT", "LOST",   "RESTORED",
                                    "ENDED",       "UNPAIRED", "SOS",        "SOS_ACK",    "SOS_CANCEL",
                                    "PONG",        "DISC_DONE", "PATH"};
    return kind < sizeof(n) / sizeof(n[0]) ? n[kind] : "?";
}

/* link_path_state_name() -- a path_state_t as one word (link.h). */
const char *link_path_state_name(uint8_t s)
{
    static const char *const n[] = {"none", "checking", "connected", "they-cant-hear", "cant-hear-them", "lost", "off"};
    return s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

/* link_path_name() -- "LoRa" or "relay". */
const char *link_path_name(uint8_t path)
{
    return path == LP_LORA ? "LoRa" : "relay";
}

/* link_lost_why_name() -- a lost_why_t as one word (link.h). */
const char *link_lost_why_name(uint8_t why)
{
    static const char *const n[] = {"silence", "no-receipt", "one-way"};
    return why < sizeof(n) / sizeof(n[0]) ? n[why] : "?";
}
