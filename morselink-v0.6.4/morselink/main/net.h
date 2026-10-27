/*
 * MorseLink — messaging interface used by the user interface.
 *
 * The UI says what the user wants ("connect to K7Q", "send this text",
 * "accept that request") and hears back, through ui_post_net(), what came
 * of it and what arrived. Behind this interface (net.c):
 *
 *   - the link protocol engine (link.c), which decides what goes on air;
 *   - the LoRa radio (radio.c);
 *   - or, for testing on one board, the loopback network: a pretend
 *     partner, running its own copy of the protocol engine, that accepts
 *     requests, echoes messages back, acknowledges SOS and answers sweeps
 *     (with two more pretend devices further off). Loopback is a console
 *     option (`loopback on`) and replaces the radio while on.
 *   - the phone relay (phone.h, ble.h): while the owner's phone is
 *     connected, every packet this device sends to another device also goes
 *     to the phone, which publishes it on MQTT; packets the phone receives
 *     from MQTT come back down and are handled as if the radio had heard
 *     them. A copy that arrives both ways is acted on once.
 *
 * All functions are safe to call from any task: they queue a command for the
 * network task and return at once. Results arrive as events (net_evt_t,
 * which is the link engine's event type; see link.h for the kinds).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "link.h"

typedef link_evt_t net_evt_t;

typedef enum {
    NET_OK = 0,
    NET_ERR_NO_TRANSPORT, /* no radio found and loopback off: nothing to send it over */
    NET_ERR_QUEUE,        /* the network task's command queue is full */
} net_result_t;

/* A snapshot of the network side, for `status` and `radio` in the console. */
typedef struct {
    bool radio_ok;        /* a LoRa radio (SX1276 or SX1262) answered at boot */
    bool loopback;        /* the loopback network replaces the radio */
    uint32_t own;         /* our address (ADDR_NONE: no ID set) */
    int tx_dbm;           /* transmit power */
    int tx_dbm_max;       /* the most this chip can do (22 SX1262, 20 SX1276) */
    uint8_t sf;           /* spreading factor in use (9..12) */
    uint32_t peer;        /* link partner, ADDR_NONE if none */
    uint8_t state;        /* link_state_t */
    int32_t heard_ago_s;  /* seconds since the partner was last heard, either path (-1: never) */
    int16_t peer_rssi;    /* the partner's last packet over LoRa, as heard here */
    int8_t peer_snr_x4;
    uint8_t path_st[2];   /* path_state_t of LoRa [LP_LORA] and the relay [LP_RELAY] (link.h) */
    int32_t heard_s[2];   /* seconds since we heard the partner on each path (-1: not since the link started) */
    int32_t they_s[2];    /* seconds since they heard us on each path, by their latest report (-1: unknown) */
    bool reports;         /* the partner sends reports (v0.6.0 or later) */
    uint8_t lost_why;     /* lost_why_t of the latest loss */
    uint8_t bench;        /* net_bench_t: bench power holds the radio (NET_BENCH_OFF: no) */
    bool busy;            /* a message on the link in the last 10 min */
    uint8_t req_phase;    /* our open request: req_phase_t */
    uint32_t req_to;
    uint8_t n_pend;       /* requests waiting for our user's A / N */
    uint8_t n_msg;        /* our messages waiting for their receipts */
    uint32_t duty_used_ms;   /* airtime used in the last hour */
    uint32_t duty_budget_ms; /* ... out of this */
    uint32_t pkts_tx, pkts_rx, pkts_dup, pkts_bad, pkts_crc;
    uint32_t phone_in, phone_out; /* packets that came through / went to the phone relay */
    uint32_t disc_answered;  /* other people's sweeps (D) we answered */
    uint32_t disc_refused;   /* ... and left unanswered (silent mode, answer limits) */
    int noise_dbm;        /* channel noise right now */
    uint32_t stack_free;  /* the net task's unused stack, bytes (health check) */
} net_status_t;

/* Start the network task and look for the radio. Call after pmic_init()
 * (the radio is powered from LDO2). */
void net_init(void);

/* Our address (from the stored ID); ADDR_NONE keeps the network idle.
 * Changing it starts the protocol engine afresh (the link is then set
 * again with net_set_link). */
void net_set_identity(uint32_t own);

/* Loopback test network on/off (see above). */
void net_set_loopback(bool on);
bool net_loopback(void);

/* Is there anything to send over (a radio, or loopback)? */
bool net_has_transport(void);

/* Make `peer` the link partner without a handshake: restoring the stored
 * link at boot, or a link made by an SOS (spec §10). ADDR_NONE drops it.
 * A previous partner is told the link ended. */
void net_set_link(uint32_t peer);

/* The user's blocklist (requests and messages from these are ignored). */
void net_set_blocklist(const uint32_t *list, int n);

/* The user's contacts: a general SOS also goes to each of their phones
 * through ours (phone.h, the relay). */
void net_set_contacts(const uint32_t *list, int n);

/* Test mode (ntest.h, docs/TEST_PLAN.md); the UI calls these, after its
 * own checks. Start with "test=... paths=... role=... session=... seed=..."
 * (ttest.h); from_phone: loopback must be off. */
void net_test_start(const char *args, bool from_phone);
void net_test_refuse(const char *reason);   /* the UI said no: a REFUSED line */
void net_test_stop(const char *reason);     /* "app", "console", "hold" */
void net_test_battery(int mv, int pct, bool charging); /* for STAT lines and the run-down log */

/* A packet the phone received from MQTT (ble.c): handled as if the radio had
 * heard it (link_on_rx_via). Safe from any task. */
void net_phone_rx(const uint8_t *pkt, int len);

/* Transmit power, dBm (RADIO_TX_DBM_MIN .. the chip's maximum). */
void net_set_tx_power(int dbm);

/* Bench power (bench.h): the radio held for a measurement step. While held
 * the engine transmits nothing and the radio's health check pauses;
 * NET_BENCH_OFF gives it back (receiving, at the usual power). A carrier's
 * time counts against the duty cycle. */
typedef enum { NET_BENCH_OFF = 0, NET_BENCH_STANDBY, NET_BENCH_LISTEN, NET_BENCH_CARRIER } net_bench_t;
void net_bench(int mode, int dbm);

/* Spreading factor (RADIO_SF_MIN .. RADIO_SF_MAX): applied between
 * packets; the link engine's timing follows it (link_set_lora), and LoRa
 * shows "checking" until the partner is heard on the new setting. */
void net_set_sf(int sf);

/* S / P: ask `to` to connect. Result: LEV_REQ_RESULT. */
void net_link_request(uint32_t to);
/* A / N on the request from `from`. A: result LEV_ACCEPT_RESULT. */
void net_link_accept(uint32_t from);
void net_link_decline(uint32_t from);
/* X: tell `addr` we forgot them, and drop the link if it was them. */
void net_unpair(uint32_t addr);

/* Send `text` to `dst`, which must be the current link partner (it is the
 * ID the user just heard announced). NET_OK means queued: LEV_MSG_SENT or
 * LEV_MSG_FAILED follows. Anything else: nothing was queued. */
net_result_t net_send_text(uint32_t dst, const char *text);

/* SOS family (dst: one device, or ADDR_BROADCAST). */
void net_send_sos(uint32_t dst);
void net_send_sos_cancel(uint32_t dst);
void net_send_sos_ack(uint32_t dst);

/* D: who is around? Result, about 4 s later: LEV_DISC_DONE with the list
 * (arg = how many, possibly 0). */
void net_discover(void);
/* Silent mode (Z) off = true: answer other devices' sweeps. */
void net_set_discoverable(bool on);

/* Bench tools (console). */
void net_ping(void);                 /* who hears us, and how well? */
void net_set_log(bool on);           /* log every packet sent and received */
void net_sim_request(uint32_t from); /* loopback: the pretend partner `from` asks us to connect */
void net_sim_deaf(bool on);          /* loopback: the pretend partner stops hearing us (and we it) */
void net_sim_discover(void);         /* loopback: the pretend partner sweeps (D); its result is logged */

void net_get_status(net_status_t *st);
