/*
 * MorseLink — link protocol engine (spec §7, §10).
 *
 * Everything between "the user did something" and "these bytes go on air":
 * connecting (S / P, A / N), the walkie-talkie link with its keepalives,
 * messages with their silent delivery receipts, unpairing and SOS relaying.
 *
 * Pure C with no radio, no RTOS and no clock of its own: the caller passes
 * the time into every function and moves bytes in and out. One engine is
 * one device, so the host tests run several of them against a simulated
 * radio channel, and the loopback test mode runs a second one as a
 * pretend partner.
 *
 * How it is driven (net.c does this on the device):
 *
 *   user action        -> link_request() / link_accept() / link_send_msg() ...
 *   packet received    -> link_on_rx()
 *   radio is free      -> link_take_tx() gives the next packet, if any is due;
 *                         link_tx_done() when it has left
 *   every few tens of ms -> link_poll() runs the timers
 *   after any of these -> link_get_event() until it returns false, and pass
 *                         the events to the user interface
 *
 * The rules it implements (decided 3 and 6 October):
 *
 *   Connecting. S/P sends LINK_REQ, retried for about 10 s. The other side
 *   answers at once: RECEIVED (its user is told and decides with A / N),
 *   BUSY (it had a message on its link in the last 10 minutes, so the request
 *   waits in its queue and is played later), or ACCEPT straight away if we
 *   are already its link or it was asking for us too. Its user's A sends
 *   ACCEPT, repeated until our PAIR_CONFIRM arrives; N sends DECLINE. No
 *   answer at all within the retries means "unavailable". A request stays
 *   open after RECEIVED or BUSY until it is answered, until we ask someone
 *   else, or until it expires; an ACCEPT for a request that is no longer
 *   open is answered ENDED ("not wanted any more").
 *
 *   The link. A device is attached to one partner (its peer). Each side
 *   sends the other a KEEPALIVE every ~30 s (at SF9/10; longer at SF11/12,
 *   link_timing), whatever else goes out, and every keepalive carries a
 *   short report: "I last heard you N s ago over LoRa, M s ago through the
 *   phones". When the link moves to someone else, the old partner is told
 *   (LINK_REPLY ENDED). Any device whose keepalive arrives although it is
 *   not our partner is told again, so the two ends always end up agreeing,
 *   even after a restart; the exceptions are a device we are just
 *   accepting and the responders to an SOS of ours that is still going on.
 *
 *   Paths (agreed 15 Oct, the connection-lost fix). A link can work over
 *   LoRa, over the phones' relay (phone.h), or both, and each has its own
 *   status (path_state_t): connected only when it is proven both ways
 *   within the lost time (90 s at SF9: we heard them on it, and their
 *   latest report says they heard us on it); one-way when only one
 *   direction works; lost when neither; "off" for the relay while no phone
 *   is connected here. The link as a whole (LS_UP / LS_LOST) is up while
 *   at least one path is connected, so sending keeps working through the
 *   relay when LoRa is gone. Every move from up to lost is reported, once,
 *   with its reason (LEV_LINK_LOST, lost_why_t): silence (nothing heard on
 *   any path), no-receipt (a message failed), one-way (we hear them but
 *   they can't hear us). It is restored only with proof both ways: a fresh
 *   report from them, so a partner we can hear but who can't hear us never
 *   shows as connected. A partner on older firmware sends no reports: then
 *   hearing them counts as both ways, as before. Path changes are events
 *   too (LEV_PATH), for the phone and the test logs.
 *
 *   Messages. Each MSG is answered by a MSG_RECEIPT, which neither user
 *   feels. No receipt after the retries: the message fails, its text goes
 *   back to the user's draft, and the link counts as lost. Messages are
 *   accepted from anyone who isn't blocked; duplicates are re-receipted but
 *   not delivered twice.
 *
 *   SOS. SOS, SOS_ACK and SOS_CANCEL ignore the blocklist and the duty
 *   cycle, and every device relays each one once (hop limit 2) after a
 *   random pause, so they reach a little beyond direct range.
 *
 *   Discovery (D). DISCOVER goes to everyone, saying how many answer slots
 *   there are and how long each is. Every discoverable device (not in
 *   silent mode, within its answer limits, see disc.h) answers PRESENCE in
 *   a random slot, quoting its transmit power. The searcher listens for the
 *   whole window, then reports everyone it heard in one event, sorted into
 *   near / mid / far. Inviting someone from that list is an ordinary
 *   request (link_request), so nothing else changes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "disc.h"
#include "morse.h"
#include "peers.h"
#include "proto.h"

/* ------------------------------------------------------------------------- */
/* States and results                                                         */
/* ------------------------------------------------------------------------- */

typedef enum {
    LS_NONE = 0, /* no partner */
    LS_UP,       /* at least one path (LoRa or the relay) works both ways */
    LS_LOST,     /* no path works both ways: sending is refused until one does again */
    LS_ENDED,    /* the partner told us they moved on: sending is refused */
} link_state_t;

/* The two ways a link can carry packets (agreed 15 Oct: shown separately). */
typedef enum {
    LP_LORA = 0,  /* the radio */
    LP_RELAY = 1, /* the phones and MQTT (phone.h) */
} link_path_t;

/* One path's status, as this device sees it (path_state of link.c). */
typedef enum {
    PS_NONE = 0,  /* no partner, or the link ended */
    PS_CHECKING,  /* just started (the link, or a new radio setting): not proven yet; at most the lost time */
    PS_UP,        /* both ways: we heard them on it, and they report they heard us on it, within the lost time */
    PS_THEY_CANT, /* one way: we hear them, but they can't hear us */
    PS_WE_CANT,   /* one way: they hear us (their report came another way), but we can't hear them */
    PS_LOST,      /* neither way */
    PS_OFF,       /* the relay only: no phone connected here (or a test is using it) */
} path_state_t;

/* Why the link was lost (LEV_LINK_LOST's arg). */
typedef enum {
    LOST_SILENCE = 0, /* nothing heard from them on any path for the lost time */
    LOST_NO_RECEIPT,  /* one of our messages got no receipt */
    LOST_ONE_WAY,     /* we hear them (or they hear us), but not both ways on any path */
} lost_why_t;

typedef enum {
    REQ_IDLE = 0, /* no request of ours is open */
    REQ_SENDING,  /* LINK_REQ going out; nothing heard back yet */
    REQ_WAITING,  /* they answered RECEIVED: their user is deciding */
    REQ_QUEUED,   /* they answered BUSY: we wait in their queue */
} req_phase_t;

typedef enum {
    LRES_ACCEPTED = 0, /* OK + their ID (a new contact: OK + slot digit) */
    LRES_BUSY,         /* error + BUSY */
    LRES_DECLINED,     /* error + NO */
    LRES_UNAVAILABLE,  /* error + their ID */
} link_result_t;

/* ------------------------------------------------------------------------- */
/* Events for the user interface                                              */
/* ------------------------------------------------------------------------- */

typedef enum {
    LEV_MSG = 1,       /* addr sent us `text` */
    LEV_MSG_SENT,      /* our message to addr was received (its receipt arrived) */
    LEV_MSG_FAILED,    /* no receipt from addr: `text` is everything not delivered; arg = 0: every
                          message that was waiting failed, 1: only the one link_send_msg() just refused */
    LEV_REQ_IN,        /* addr asks to connect: tell the user, who answers A / N */
    LEV_REQ_GONE,      /* addr's request expired or no longer applies */
    LEV_REQ_RESULT,    /* our request to addr ended: arg = link_result_t */
    LEV_ACCEPT_RESULT, /* our accept of addr: arg = 1 linked, 0 failed */
    LEV_LINK_LOST,     /* the link to our partner addr is down (no path works both ways): arg = lost_why_t */
    LEV_LINK_RESTORED, /* ... and up again (a path works both ways) */
    LEV_LINK_ENDED,    /* our partner addr moved on to someone else */
    LEV_UNPAIRED,      /* addr forgot us (UNPAIR); arg = 1 if they were our link */
    LEV_SOS,           /* SOS from addr; arg = 1 if addressed to us alone */
    LEV_SOS_ACK,       /* addr acknowledged our SOS */
    LEV_SOS_CANCEL,    /* addr cancelled their SOS */
    LEV_PONG,          /* addr answered our PING (bench / range test) */
    LEV_DISC_DONE,     /* our sweep (link_discover) ended: arg devices found, listed in `found` */
    LEV_PATH,          /* a path's status changed: arg = link_path_t << 4 | path_state_t (LINK_EV_PATH / _STATE) */
} link_ev_kind_t;

/* LEV_PATH's arg, taken apart. */
#define LINK_EV_PATH(arg) ((uint8_t)(arg) >> 4)
#define LINK_EV_STATE(arg) ((uint8_t)(arg)&0x0F)

typedef struct {
    uint8_t kind;     /* link_ev_kind_t */
    uint8_t arg;      /* see the kinds above */
    uint32_t addr;    /* the other device */
    int16_t rssi;     /* packets: signal strength here, dBm (0 if not from a packet) */
    int8_t snr_x4;    /* ... and signal-to-noise ratio here, in 0.25 dB steps */
    int16_t far_rssi; /* LEV_PONG: how they heard our PING */
    int8_t far_snr_x4;
    int8_t far_dbm;   /* LEV_PONG: their transmit power */
    union {           /* (anonymous: written e->text / e->found) */
        char text[MSG_MAX_CHARS + 1];  /* LEV_MSG, LEV_MSG_FAILED: the text */
        disc_entry_t found[DISC_MAX];  /* LEV_DISC_DONE: `arg` devices, in walking order (near first) */
    };
} link_evt_t;

/* The found list shares the text's space, so events don't grow. */
_Static_assert(sizeof(disc_entry_t) * DISC_MAX <= MSG_MAX_CHARS + 1, "discovery list must fit in an event");

/* ------------------------------------------------------------------------- */
/* Timing: everything that has to grow with the airtime (agreed 15 Oct)      */
/* ------------------------------------------------------------------------- */

/*
 * At SF9 these are exactly the constants in config.h (KEEPALIVE_MS,
 * RETRY_GAP_MS, ...); a slower spreading factor stretches them so that,
 * e.g., a receipt still fits in the pause between two copies of a message,
 * and keepalives stay a small share of the airtime. link_timing() computes
 * them from the radio setting; the engine keeps its own copy (link_t.tm),
 * and net.c, the UI and test mode use the same function.
 */
typedef struct {
    uint32_t keepalive_ms;     /* between keepalives (KEEPALIVE_MS at SF9/10; RADIO_KA_*_MS at SF11/12) */
    uint32_t ka_jitter_ms;     /* ... minus up to this much at random */
    uint32_t lost_ms;          /* a path not proven for this long is down (3 keepalive periods) */
    uint32_t quick_ka_gap_ms;  /* an extra keepalive (to prove a path again quickly) at most this often */
    uint16_t retry_gap_ms;     /* between copies of a request / accept / message (from the end of one) */
    uint16_t retry_jitter_ms;  /* ... plus up to this much at random */
    uint16_t reply_wait_ms;    /* after the last copy: how long the answer is awaited */
    uint32_t hard_ms;          /* a request / accept / message is abandoned this long after it was queued */
    uint32_t reply_hard_ms;    /* an answer (receipt, confirm) that can't leave by then is useless */
    uint32_t relay_hard_ms;    /* someone's SOS we pass on: dropped if it can't leave by then */
    uint32_t lbt_max_ms;       /* net.c: the longest we hold a packet back for someone else's */
    uint32_t pong_spread_ms;   /* PONGs to one PING spread over this */
    uint32_t sos_relay_max_ms; /* the random pause before passing someone's SOS on, at most */
    uint32_t disc_send_hard_ms; /* a DISCOVER that can't leave by then ends the sweep */
    uint32_t disc_late_ms;     /* the searcher listens this long after the last slot */
    uint32_t max_pkt_ms;       /* the longest packet (a 100-character message) */
} link_timing_t;

/* The timing for radio setting `lp` (pure; see above). */
void link_timing(const lora_params_t *lp, link_timing_t *t);

/* How long a sweep (D) takes at this setting, from the DISCOVER leaving to
 * the list (16 answer slots plus the stragglers' time). */
uint32_t link_disc_window_ms(const lora_params_t *lp);

/* A keepalive's payload: the two "heard you" bytes (LoRa, relay). */
#define KA_REPORT_LEN 2
#define KA_NEVER 255 /* in a report: never heard (since the link started), or 255 s ago or more */

/* ------------------------------------------------------------------------- */
/* Engine state (treat as private; read only through the functions below)    */
/* ------------------------------------------------------------------------- */

#define LINK_OB_N 16  /* packets waiting to be (re)transmitted */
#define LINK_EV_N 16  /* events waiting for the user interface */
#define LINK_EX_N 6   /* devices we have recently reminded "ended" / "unpaired" */

/* One entry of the transmit queue: a packet and its retry plan. */
typedef struct {
    uint8_t kind;      /* ob_kind_t in link.c; 0 = free */
    uint8_t tries;     /* copies still to send */
    uint8_t exempt;    /* SOS family: ignores the duty-cycle limit */
    uint8_t len;       /* bytes in pkt */
    uint16_t gap_ms;   /* pause between copies (plus jitter and airtime) */
    uint16_t wait_ms;  /* after the last copy, how long an answer is awaited */
    int64_t due;       /* earliest time for the next copy */
    int64_t give_up;   /* tries == 0: end of the wait for an answer */
    int64_t hard;      /* abandon at this time even with copies left (0 = never) */
    uint32_t dst;      /* destination, for matching answers */
    uint16_t id;       /* message ID, for matching answers */
    uint8_t pkt[PROTO_MAX_PKT];
} link_ob_t;

/* A request waiting for our user's A / N. */
typedef struct {
    uint32_t from;
    uint16_t ref;       /* ID of their latest LINK_REQ: our answers quote it */
    bool alerted;       /* our user has been told about it */
    int64_t arrived;
    int64_t alerted_at;
} link_pend_t;

/* An outgoing message. msgq[0] is the one being delivered. */
typedef struct {
    uint32_t to;
    uint16_t id;
    char text[MSG_MAX_CHARS + 1];
} link_msg_t;

/* A device that still sends us keepalives although it isn't our partner
 * (it missed our ENDED or UNPAIR notice, or we restarted): it is reminded,
 * at most every 10 s. This table remembers when it was last reminded, and
 * whether the reminder is UNPAIR (we forgot them) rather than ENDED. */
typedef struct {
    uint32_t addr;
    int64_t until;      /* forget the entry after this */
    int64_t last_reply; /* last reminder sent to it (rate limit) */
    bool unpaired;      /* we forgot them (X), rather than moved on */
} link_ex_t;

/* A device that acknowledged our SOS. Acknowledging makes us its partner
 * without the usual handshake (spec §10), while we may keep our own
 * partner, so its keepalives must not be answered "ended" while our SOS
 * goes on. Each SOS resend extends this; cancelling the SOS ends it. */
typedef struct {
    uint32_t addr;
    int64_t until;
} link_sosrel_t;

#define LINK_SOSREL_N 4
#define LINK_SOSREL_MS (10 * 60 * 1000) /* our SOS resends come at least every 2 minutes */

/* Our own sweep (link_discover). */
typedef enum {
    DISC_IDLE = 0,  /* no sweep of ours */
    DISC_SENDING,   /* our DISCOVER is queued (it may wait for the channel) */
    DISC_LISTENING, /* it has left: collecting PRESENCE answers until disc_end */
} disc_phase_t;

typedef struct {
    /* who we are */
    uint32_t own;          /* our address; ADDR_NONE disables the engine */
    lora_params_t lora;    /* for airtime */
    link_timing_t tm;      /* the timing for that setting (link_timing) */
    int8_t tx_dbm;         /* reported in PING / PONG */
    uint32_t rng;          /* jitter generator state */

    /* the link */
    uint32_t peer;         /* partner, ADDR_NONE if none */
    uint8_t state;         /* link_state_t */
    int64_t last_heard;    /* last packet the partner itself sent (any destination, either path) */
    int64_t last_msg;      /* last message to/from the partner (busy); LINK_NEVER if none */
    int64_t ka_due;        /* when our next keepalive is due */
    int16_t peer_rssi;     /* the partner's last packet over LoRa, as heard here */
    int8_t peer_snr_x4;

    /* the link's paths (agreed 15 Oct; path_state in link.c) */
    int64_t link_since;    /* when the link (re)started: the first lost time counts as "checking" */
    int64_t lora_heard;    /* last packet from the partner itself over LoRa (LINK_NEVER: none since the start) */
    int64_t relay_heard;   /* ... through the phones */
    int64_t lora_since;    /* LoRa (re)started: the link, or a new radio setting */
    int64_t relay_since;   /* the relay (re)started: the link, or our phone coming back */
    bool relay_on;         /* a phone is connected here to relay (link_set_relay) */
    bool rep_ever;         /* the partner has sent a report since the link started (firmware v0.6.0 or later) */
    int64_t rep_at;        /* when their latest report arrived (LINK_NEVER: none) */
    uint8_t rep_lora_s;    /* that report: seconds since they last heard us over LoRa (KA_NEVER: never) */
    uint8_t rep_relay_s;   /* ... through the phones */
    int64_t lora_rep_from; /* reports that arrived before this say nothing about LoRa (a new radio setting) */
    int64_t fail_at;       /* our last message to the partner without a receipt (LINK_NEVER: none since the start) */
    int64_t quick_ka_at;   /* our last extra keepalive (rate limit: tm.quick_ka_gap_ms) */
    uint8_t path_st[2];    /* path_state_t of LoRa and the relay, as last reported (LEV_PATH) */
    uint8_t lost_why;      /* lost_why_t of the latest loss */

    /* our open request */
    uint8_t req_phase;     /* req_phase_t */
    uint32_t req_to;
    uint16_t req_id;
    int64_t req_opened;

    /* our accept awaiting its PAIR_CONFIRM */
    bool acc_active;
    uint32_t acc_to;
    uint16_t acc_id;

    /* requests waiting for our user */
    link_pend_t pend[LINK_PENDING_MAX];
    uint8_t n_pend;

    /* outgoing messages */
    link_msg_t msgq[MSG_QUEUE_MAX + 1];
    uint8_t n_msg;
    bool msg_inflight;     /* msgq[0] has been handed to the transmit queue */

    link_ex_t ex[LINK_EX_N];
    link_sosrel_t sosrel[LINK_SOSREL_N];

    uint32_t blocked[BLOCK_MAX]; /* copy of the user's blocklist */
    uint8_t n_blocked;

    /* discovery: answering other people's sweeps */
    bool discoverable;      /* false in silent mode (Z): DISCOVER is never answered */
    disc_limit_t disc_lim;  /* how often we have answered whom (disc.h) */

    /* discovery: our own sweep */
    uint8_t disc_phase;     /* disc_phase_t */
    uint16_t disc_id;       /* our DISCOVER's message ID; its low byte tags the answers */
    uint16_t disc_slot_ms;  /* slot length we asked for (as sent: whole 10 ms steps) */
    int64_t disc_end;       /* DISC_LISTENING: when the window closes */
    disc_list_t disc_found; /* who answered so far */

    /* transmit side */
    link_ob_t ob[LINK_OB_N];
    int8_t inflight;        /* ob[] index on air now, or -1 */
    uint32_t inflight_ms;   /* its airtime */
    dc_t dc;
    msgid_t ids;

    /* receive side */
    dedup_t dd;

    /* events */
    link_evt_t ev[LINK_EV_N];
    uint8_t ev_head, ev_n;

    /* counters, for the console */
    uint32_t n_rx, n_tx, n_dup, n_bad, n_ev_dropped;
    uint32_t n_disc_answered, n_disc_refused; /* DISCOVERs answered / left unanswered (silent or limited) */
    uint32_t n_rx_phone;                      /* packets that came through the phone relay */
} link_t;

#define LINK_NEVER INT64_MIN

/* ------------------------------------------------------------------------- */
/* Set-up                                                                     */
/* ------------------------------------------------------------------------- */

/* Reset everything. own = our address (ADDR_NONE: inactive until an ID is
 * set). seed = any random number (jitter). msgid_stored / save / ctx: the
 * message-ID block saved in flash and how to save the next one (proto.h).
 * Calls `save` once (the first block is reserved immediately). */
void link_init(link_t *L, uint32_t own, const lora_params_t *lp, uint32_t seed, uint16_t msgid_stored,
               msgid_save_fn save, void *save_ctx, int64_t now);

/* Our transmit power, quoted in PING / PONG packets. */
void link_set_tx_dbm(link_t *L, int8_t dbm);

/* Replace the copy of the blocklist (packets from these are ignored, except SOS). */
void link_set_blocklist(link_t *L, const uint32_t *list, int n);

/* Silent mode (Z) off = discoverable: answer other devices' sweeps. On by
 * default (link_init). */
void link_set_discoverable(link_t *L, bool on);

/* Is a phone here to relay (connected, and not in use by a test)? Off: the
 * relay path shows "off" and doesn't count for the link. net.c keeps this
 * up to date; it starts off. */
void link_set_relay(link_t *L, bool on, int64_t now);

/* A new radio setting (SF): the timing is recomputed, and LoRa starts
 * "checking" again (packets heard on the old setting no longer count). */
void link_set_lora(link_t *L, const lora_params_t *lp, int64_t now);

/* ------------------------------------------------------------------------- */
/* User actions                                                               */
/* ------------------------------------------------------------------------- */

/* Make `peer` our partner without asking them: the link restored at boot, or
 * one made by an SOS (spec §10). A previous partner is told the link ended.
 * ADDR_NONE drops the link. */
void link_set_peer(link_t *L, uint32_t peer, int64_t now);

/* S / P: ask `to` to connect. Supersedes any request still open. The answer
 * arrives as LEV_REQ_RESULT (unless it is still open when superseded).
 * Returns false (and emits LEV_REQ_RESULT unavailable) if it can't be sent. */
bool link_request(link_t *L, uint32_t to, int64_t now);

/* A: accept the pending request from `from`. LEV_ACCEPT_RESULT follows.
 * Returns false (and emits a failed LEV_ACCEPT_RESULT) if there is none. */
bool link_accept(link_t *L, uint32_t from, int64_t now);

/* N: decline the pending request from `from` (blocking is the caller's
 * business). Returns false if there was none. No event. */
bool link_decline(link_t *L, uint32_t from, int64_t now);

/* X: we forgot `addr`; tell them (UNPAIR) and drop anything to do with them. */
void link_unpair(link_t *L, uint32_t addr, int64_t now);

/* Send `text` to our partner, who must be `to`: the user heard `to`
 * announced, so if the link has moved meanwhile the message must not go to
 * someone else. LEV_MSG_SENT or LEV_MSG_FAILED follows. Returns false (after
 * emitting LEV_MSG_FAILED) if `to` isn't our partner, the link isn't up, or
 * too many messages are already waiting. */
bool link_send_msg(link_t *L, uint32_t to, const char *text, int64_t now);

/* The same, also accepted while the link is lost (LS_LOST): test mode's
 * point test (v0.6.1) keeps sending at the edge of range, where the link
 * comes and goes, because the delivery there is the measurement. The user's
 * messages never go this way (sending is refused while the link is lost). */
bool link_send_test_msg(link_t *L, uint32_t to, const char *text, int64_t now);

/* SOS family. dst = one device, or ADDR_BROADCAST for a general SOS. */
void link_send_sos(link_t *L, uint32_t dst, int64_t now);
void link_send_sos_cancel(link_t *L, uint32_t dst, int64_t now);
void link_send_sos_ack(link_t *L, uint32_t dst, int64_t now);

/* Bench tool: broadcast a PING; every device in range answers with a PONG
 * saying how well it heard us (LEV_PONG). */
void link_ping(link_t *L, int64_t now);

/* D: who is around? Broadcasts a DISCOVER and collects the answers for about
 * 3.5 s after it has left; then LEV_DISC_DONE lists them (arg = how many,
 * possibly 0). A sweep still running is replaced (only the new one
 * reports). Returns false (after emitting LEV_DISC_DONE with 0) if it can't
 * even be queued: no ID, or the transmit queue is full. */
bool link_discover(link_t *L, int64_t now);

/* ------------------------------------------------------------------------- */
/* Radio side                                                                 */
/* ------------------------------------------------------------------------- */

/* A packet arrived: buf/len as received, with its RSSI (dBm) and SNR. */
void link_on_rx(link_t *L, const uint8_t *buf, int len, int16_t rssi, int8_t snr_x4, int64_t now);

/* The same for a packet that may have come through the phone relay
 * (via_phone = true; phone.h): such a packet still proves its sender is
 * reachable, but says nothing about the radio, so it never changes the
 * partner's signal readings, and discovery and ping packets are ignored
 * (they measure the radio). Everything else is handled exactly as if it had
 * come over LoRa; a copy that arrives both ways is acted on once. */
void link_on_rx_via(link_t *L, const uint8_t *buf, int len, int16_t rssi, int8_t snr_x4, bool via_phone,
                    int64_t now);

/* The radio is free: is a packet due? Copies it to buf (PROTO_MAX_PKT bytes)
 * and sets *len and *long_preamble. Only one packet is handed out at a time:
 * call link_tx_done() before asking again. */
bool link_take_tx(link_t *L, int64_t now, uint8_t *buf, int *len, bool *long_preamble);

/* The packet from link_take_tx() has left (sent = true) or could not be
 * sent (false: it will be tried again shortly). */
void link_tx_done(link_t *L, int64_t now, bool sent);

/* Run the timers: retries running out, keepalives, lost links, request
 * expiry. Call at least every 100 ms, and after link_on_rx(). */
void link_poll(link_t *L, int64_t now);

/* Earliest time at which link_take_tx() or link_poll() may have something to
 * do (INT64_MAX if nothing is scheduled). Lets the caller sleep. */
int64_t link_next_due(const link_t *L, int64_t now);

/* Take the oldest pending event. False when there are none. */
bool link_get_event(link_t *L, link_evt_t *e);

/* ------------------------------------------------------------------------- */
/* Queries                                                                    */
/* ------------------------------------------------------------------------- */

/* Busy = a message sent to or received from our partner in the last 10 min
 * while the link is up (spec §7). */
bool link_busy(const link_t *L, int64_t now);

/* A path's status right now (path_state_t; PS_NONE without a link). */
uint8_t link_path_state(const link_t *L, uint8_t path, int64_t now);

/* Seconds since we last heard the partner on `path` (-1: not since the
 * link started). */
int32_t link_heard_ago_s(const link_t *L, uint8_t path, int64_t now);

/* Seconds since the partner last heard us on `path`, from their latest
 * report, aged to now (-1: no report, or "never"). */
int32_t link_they_heard_s(const link_t *L, uint8_t path, int64_t now);

/* Airtime used in the duty-cycle window, ms, and the budget. */
uint32_t link_duty_used_ms(const link_t *L, int64_t now);

/* Count `ms` of transmitting the engine didn't do itself (bench power's
 * carrier) against the duty cycle. */
void link_add_airtime(link_t *L, int64_t now, uint32_t ms);

/* A fresh message ID from the engine's own sequence, for a packet sent
 * outside the engine (test mode's relay copies, ntest.c): one numbering per
 * device, so no two packets ever share an ID. */
uint16_t link_new_id(link_t *L);

const char *link_state_name(uint8_t s);
const char *link_req_phase_name(uint8_t p);
const char *link_result_name(uint8_t r);
const char *link_event_name(uint8_t kind);

/* A path_state_t as one word, for the phone's lines and the logs:
 * none, checking, connected, they-cant-hear, cant-hear-them, lost, off. */
const char *link_path_state_name(uint8_t s);
/* "LoRa" / "relay". */
const char *link_path_name(uint8_t path);
/* A lost_why_t as one word: silence, no-receipt, one-way. */
const char *link_lost_why_name(uint8_t why);
