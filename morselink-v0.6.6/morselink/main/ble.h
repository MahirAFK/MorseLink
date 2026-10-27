/*
 * MorseLink — the BLE link to the owner's phone (spec §12; the lines it
 * carries are in phone.h).
 *
 * A Nordic UART Service (NUS), as in the owner's first BLE test: the phone
 * writes to RX (6E400002-...) and gets notifications from TX (6E400003-...);
 * the device advertises as "MorseLink-<ID>" (just "MorseLink" before an ID
 * is set).
 *
 * Security. Pairing is LE Secure Connections "Just Works" with bonding (the
 * device has no display or keypad), so the link is encrypted. What keeps
 * random phones out is the `O` window: a phone that bonds while it is open
 * (60 s) is accepted, replaces any phone paired before, and from then on
 * reconnects without asking; a phone that bonds at any other time is
 * disconnected and its keys deleted. While the window is open only a new
 * phone gets in (the paired one waits until it closes). Until a phone is
 * accepted nothing is sent to it and its lines are ignored.
 *
 * BLE is off until the first `O` (it costs power and RAM, and most of the
 * device's life no phone is around); once a phone has been accepted, it
 * starts at every boot. One phone connects at a time.
 *
 * Bench and emulator: `phone sim on` in the console pretends a phone is
 * connected (no radio at all): lines to it are logged as `phone << ...`,
 * and `phone send ...` types a line as if the phone had written it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define BLE_WINDOW_MS (60 * 1000) /* (spec) O: how long a new phone may bond */

typedef struct {
    char name[24];       /* advertised as, e.g. "MorseLink-K7Q" */
    bool started;        /* NimBLE is running */
    bool advertising;
    bool connected;      /* a phone is connected (accepted or not) */
    bool encrypted;
    bool accepted;       /* ... and may talk to us */
    bool subscribed;     /* ... and listens to our notifications */
    bool sim;            /* `phone sim on` */
    uint16_t mtu;
    int32_t window_s;    /* seconds left of the O window (0: closed) */
    int bonds;           /* phones remembered (-1: unknown, BLE off) */
    uint32_t lines_in, lines_out, lines_dropped;
} ble_status_t;

/* Create the outgoing-line queue and its task. The radio stays off. */
void ble_init(void);

/* The advertised name: "MorseLink-<own_id>" ("MorseLink" if own_id is
 * empty). Takes effect at the next advertising start. */
void ble_set_name(const char *own_id);

/* Start BLE and advertise (does nothing if it is running). ESP_OK, or the
 * reason it could not start (logged). */
esp_err_t ble_start(void);

/* Stop BLE completely (before deep sleep). Safe if it isn't running. */
void ble_stop(void);

/* Bench power (v0.6.1): advertise without accepting connections (false:
 * step 3a, "advertising, not connectable": no phone can connect), or as
 * usual (true, the default). Takes effect at once if BLE is advertising, and
 * at the next start otherwise. */
void ble_set_connectable(bool on);

/* Wait (at most timeout_ms) until the lines queued for the phone have gone
 * out: before switching BLE off, so the phone still gets the last word (W's
 * EV BLE_OFF, bench power's BN lines). Returns at once with no phone. */
void ble_flush(int timeout_ms);

/* O: start BLE if needed and accept the next phone that bonds within
 * BLE_WINDOW_MS. The UI hears UIP_PAIRED if one does. */
esp_err_t ble_open_window(void);

/* True while a new phone may bond. */
bool ble_window_open(void);

/* A phone (or the console's pretend phone) is connected, accepted and
 * listening: lines sent now reach it. */
bool ble_phone_ready(void);

/* Send one line (phone.h, without '\n') to the phone. Never blocks: when no
 * phone is ready, or the queue is full, the line is dropped (and counted). */
void ble_send_line(const char *line);

/* How many more lines ble_send_line() would take right now without
 * dropping any (the pretend phone takes any number). Test mode keeps its log
 * lines back while this is low. */
int ble_tx_space(void);

/* Console: pretend a phone is connected (on), or not. */
void ble_sim(bool on);

/* Console: handle `line` as if the phone had written it. */
void ble_sim_line(const char *line);

/* Factory reset or `phone forget`: forget every phone. Stops BLE (if it
 * runs); it stays off, now and at boot, until the next O. */
void ble_forget_phones(void);

void ble_get_status(ble_status_t *s);
