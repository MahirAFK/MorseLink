/*
 * MorseLink — test mode, the part that runs in the net task (ntest.c; the
 * plan is docs/TEST_PLAN.md, the schedule and texts ttest.h).
 *
 * net.c calls every function here, from the net task only, so nothing is
 * locked. Other tasks reach it through net.h (net_test_*), which queues a
 * command for the net task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "link.h"

/* Once, before the net task runs: the engine to drive, how to read the
 * channel's noise floor (for the STAT lines), and how to switch the radio
 * for a point test's steps (v0.6.1): set_radio(sf, dbm) asks the net task
 * for that spreading factor and power, applied between packets and never
 * saved, and returns the power the chip will use (clamped to its maximum). */
void ntest_init(link_t *eng, int (*noise_dbm)(void), int (*set_radio)(int sf, int dbm));

/* Start a test with "test=... paths=... role=... session=... seed=..."
 * (ttest.h), at this board's transmit power and spreading factor (the pace
 * follows the SF). Refused, with a REFUSED line saying why, unless this
 * board is linked and the link is up (LoRa itself, for a test that sends
 * over LoRa: reason lora-lost), no test runs already, and (when the phone
 * asks) loopback is off. On success: a START line, and the UI hears test
 * mode is on (ui_post_test). A point test (plan=...) then switches to its
 * first step's setting at once (a STEP line); tx_dbm and sf are what it
 * puts back at the end. */
void ntest_start(const char *args, bool from_phone, bool loopback, int tx_dbm, int sf, int64_t now);

/* The UI refused a start (e.g. our SOS is active): a REFUSED line. */
void ntest_refuse(const char *reason, int64_t now);

/* Stop the test ("app", "console", "hold"...). New messages stop at once;
 * the STOP line follows when the copies still out have their results (at
 * most NT_DRAIN_MS later). The UI hears test mode is off. A point test's
 * radio setting goes back to the board's own at once (STEP i=0 ... restored=1). */
void ntest_stop(const char *reason, int64_t now);

/* The latest battery reading (the UI's, every 30 s), for STAT lines and the
 * run-down log. mv / pct < 0: no reading. */
void ntest_battery(int mv, int pct, bool charging);

/* Run every time round the net task's loop: the schedule, receipt
 * timeouts, STAT lines, run-down samples, and sending queued log lines to
 * the phone. */
void ntest_tick(int64_t now);

/* A test is running (or finishing): packets aren't mirrored to the phone,
 * and packets from the phone come here instead of to the engine. Safe to
 * read from any task. */
bool ntest_active(void);

/* Hooks: a packet the engine transmits (air_ms its airtime, lbt_ms how long
 * it waited for a clear channel); one received over the radio; a damaged
 * one. They only log (TX, RX, RXBAD) while a test runs. */
void ntest_on_tx(const uint8_t *pkt, int len, uint32_t air_ms, int64_t lbt_ms, int tx_dbm, int64_t now);
void ntest_on_rx(const uint8_t *pkt, int len, int16_t rssi, int8_t snr_x4, int64_t now);
void ntest_on_rx_bad(int64_t now);

/* A packet from the phone. True if test mode took it (then the engine must
 * not see it); false when no test runs. */
bool ntest_on_phone_rx(const uint8_t *pkt, int len, int64_t now);

/* An engine event on its way to the UI. True if it is test mode's (then the
 * UI must not see it): message results and arrivals while a test runs, and
 * test messages that arrive after it. */
bool ntest_on_event(const link_evt_t *e, int64_t now);
