/*
 * MorseLink — user interaction state machine.
 *
 * One task owns all interaction state: the key, the decoder and calibrator,
 * the draft, command mode, playback sequencing, queued alerts, SOS, the
 * contact table and the user's view of the link. Everything else talks to
 * it through the ui_post_*() functions below, which only queue an event and
 * return (safe from any task; not from interrupts).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "keyer.h"
#include "net.h"

/* Console commands handled inside the UI task (they touch its state). */
typedef enum {
    CON_STATUS = 0,
    CON_PLAY_TEXT, /* text: render it in Morse */
    CON_TONE,      /* a = tone_t */
    CON_RX,        /* a = src addr, text: pretend a message arrived */
    CON_SIM_SOS,   /* a = src addr, b = targeted: pretend an SOS arrived */
    CON_SIM_SOS_CANCEL, /* a = src addr */
    CON_SET_ID,    /* text = the new device ID */
    CON_PEER_ADD,  /* text = ID */
    CON_PEER_DEL,  /* a = slot 1-9 */
    CON_LOOPBACK,  /* a = 0/1 */
    CON_CAL_LOG,   /* a = 0/1: verbose keying / playback log */
    CON_SPEED,     /* a = 1-9 */
    CON_HAPTIC,    /* a = HP_* parameter, b = value */
    CON_CALIBRATE,
    CON_FACTORY,
    CON_RAW,       /* a = m1 level, b = m2 level */
    CON_TX_POWER,  /* a = dBm */
    CON_POWER_OFF, /* switch off, as F does */
    CON_PHONE_STATUS, /* `phone`: the phone part of the status */
    CON_PHONE_PAIR,   /* `phone pair`: as O, the 60 s pairing window */
    CON_PHONE_FORGET, /* `phone forget`: forget the paired phone */
    CON_TEST_START,   /* text = test parameters (ttest.h); a = 1 if the phone asked */
    CON_TEST_STOP,    /* a = 1 if the phone asked */
    CON_SF,           /* a = spreading factor 9-12 (`radio sf`; as G 1-4) */
    CON_BENCH,        /* a = 1 `bench power`, 2 `bench motor` (b = motor 1 / 2), 0 `bench stop` (bench.h);
                         with a = 1 or 0, b = 1: the phone asked (BENCH POWER / STOP) */
    CON_PHONE_ONOFF,  /* a = 1 `phone on`, 0 `phone off`: as W (Bluetooth on / off, stored) */
} con_op_t;

/* CON_HAPTIC parameters. */
enum { HP_OVERLAP = 0, HP_SPLIT, HP_M1, HP_M2, HP_FARNSWORTH, HP_GATING, HP_STRENGTH /* v0.6.2: as I */, HP_DASH /* v0.6.3: as H */ };

/* What the BLE side reports (ui_post_phone). */
typedef enum {
    UIP_READY = 0,     /* a phone (or the console's pretend phone) takes lines now: send HI, ST, DR */
    UIP_HELLO,         /* the phone said HI: send HI, ST, DR again */
    UIP_PAIRED,        /* a phone was accepted while the O window was open: play OK */
    UIP_WINDOW_CLOSED, /* the O window ran out and no phone came: play error */
    UIP_GONE,          /* the phone went away */
} uip_t;

/* Create the UI task. Call after net_init(). */
void ui_start(void);

/* Thread-safe entry points. */
void ui_post_key(const key_evt_t *k);      /* from the keyer task */
void ui_post_play_done(uint32_t tag);      /* from the haptics player (esp_timer task) */
void ui_post_net(const net_evt_t *e);      /* from the net task: something arrived or finished */
bool ui_post_console(con_op_t op, int32_t a, int32_t b, const char *text); /* from the console */
void ui_post_phone(uint8_t what);          /* from the BLE side: uip_t; never blocks */
void ui_post_test_cmd(bool start, const char *args); /* from the phone: TEST START / STOP; never blocks */
void ui_post_bench_cmd(bool start);        /* from the phone: BENCH POWER / STOP (protocol 3); never blocks */
void ui_post_test(bool on, uint8_t test);  /* from the net task: test mode on / off (test: tt_test_t) */
