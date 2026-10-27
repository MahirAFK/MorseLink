/*
 * MorseLink — persistent storage (NVS, the ESP32's key-value store in flash).
 *
 * What is kept, under the namespace "morselink":
 *   "id"        the device ID (3 characters)          kept by factory reset
 *   "msgid"     next block of message IDs (proto.h)    kept by factory reset
 *   "off"       1 = switched off with F (power.h); absent or 0 = switched on
 *   "ble"       1 = a phone was accepted (O): BLE starts at every boot (ble.h)
 *   "bleoff"    1 = the user switched Bluetooth off with W (v0.6.1): it stays off,
 *               restarts included, until W again (or O)
 *   "settings"  settings_t below
 *   "calib"     the keying calibration profile (calib.h)
 *   "peers"     contacts and blocklist (peers.h)
 *
 * And under its own namespace "rundown" (test mode, docs/TEST_PLAN.md
 * §2.3): the battery samples of the last run-down test, kept until the next
 * one starts (a factory reset leaves them too):
 *   "info"      test type and the number of samples
 *   "b000"...   one blob per hour: 60 samples, one a minute
 *
 * Flash writes stall both CPU cores for tens of milliseconds, so callers only
 * save when something actually changed, and the calibration profile is
 * written while the key is resting (see ui.c).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "calib.h"
#include "esp_err.h"
#include "peers.h"

/* Version 2 (v0.3, radio): tx_dbm took the old reserved byte, link_ended was
 * added. Version 3 (v0.6.0): sf took a reserved byte, and the transmit power
 * moved to the chip's maximum once (agreed 15 Oct). Version 4 (v0.6.4, same
 * layout): `strength` is 1-9 (was 1-5), the dash overlap is in hundredths of
 * U (was ms), and the dash's new shape (agreed 24 Oct) replaces the old
 * default. Older settings are converted on load (see store.c). */
#define SETTINGS_VERSION 4

typedef struct {
    uint16_t version;
    uint8_t speed_q;        /* reading speed 1-9 (Q) */
    uint8_t farnsworth_x10; /* 10 = standard spacing */
    uint8_t overlap_cu;     /* dash: M1 and M2 both on, hundredths of U (version 4; was ms) */
    uint8_t dash_m1_cu;     /* dash: where M1 hands over to M2 (the middle of the overlap), hundredths of U */
    uint8_t m1_pct, m2_pct; /* motor intensity */
    uint8_t discoverable;   /* Z toggles; 1 = answers discovery */
    uint8_t loopback;       /* unused since 13 Oct, always 0: loopback is no longer saved
                               (left on, it hid a lost partner); kept for the layout */
    uint8_t rail_gating;    /* switch DCDC1 off between vibrations */
    int8_t tx_dbm;          /* radio transmit power */
    uint32_t link_addr;     /* current link partner, ADDR_NONE if none */
    uint8_t link_ended;     /* the partner moved on: keep the ID (for "error + ID")
                               but don't reconnect at boot */
    uint8_t sf;             /* spreading factor 9-12 (G 1-4; since version 3) */
    uint8_t strength;       /* vibration strength 1-9 (I; 1-5 in v0.6.2-v0.6.3, in a former reserved byte):
                               0 = never set = 8, full drive, DCDC1 left as the board starts it (3.3 V) */
    uint8_t dash_style;     /* how a dash feels, 1-2 (H; v0.6.3, the last former reserved byte): 0 = never set
                               = 1, the sweep M1 -> M2; 2 = M2 only */
} settings_t;

/* Fill in the defaults (loopback off, the chip's maximum power, SF9, no
 * link, ...). */
void settings_default(settings_t *s);

/* Open NVS, erasing it if its format is from an incompatible IDF version. */
esp_err_t store_init(void);

/* Device ID: false / empty string if none is stored or it is invalid. */
bool store_load_id(char out[5]);
esp_err_t store_save_id(const char *id);

/* Settings: false (and defaults in *s) if none are stored. */
bool store_load_settings(settings_t *s);
esp_err_t store_save_settings(const settings_t *s);

/* Calibration profile: false if none (or an old format). */
bool store_load_calib(cal_profile_t *p);
esp_err_t store_save_calib(const cal_profile_t *p);
esp_err_t store_erase_calib(void);

/* Contacts and blocklist: false (and an empty table) if none. */
bool store_load_peers(peer_table_t *t);
esp_err_t store_save_peers(const peer_table_t *t);

/* Message IDs: the first ID of the next unused block (proto.h, msgid_t).
 * Load returns false if nothing is stored yet (a brand-new device). */
bool store_load_msgid(uint16_t *next_block);
esp_err_t store_save_msgid(uint16_t next_block);

/* The power switch (power.h): true if the user switched the device off.
 * Nothing stored (a new device, or after a factory reset) means on. */
bool store_load_power_off(void);
esp_err_t store_save_power_off(bool off);

/* BLE at boot (ble.h): true once a phone has been accepted with O; false
 * after `phone forget` or a factory reset (nothing stored means false). */
bool store_load_ble_on(void);
esp_err_t store_save_ble_on(bool on);

/* W (v0.6.1): true while the user wants Bluetooth off (stored, so it stays
 * off through restarts); W again, `phone on` or O clears it. Bluetooth runs
 * at boot only if a phone is paired (ble_on) and this is false. */
bool store_load_ble_off(void);
esp_err_t store_save_ble_off(bool off);

/*
 * Run-down log (test mode; docs/TEST_PLAN.md §11 and §14), one sample a
 * minute, under its own namespace "rundown":
 *   info          the test type and the sample count (since v0.5.0)
 *   meta          the run's details for the dump's header (since v0.5.1;
 *                 a log written by v0.5.0 has none)
 *   b000, b001..  each hour's battery samples, RD_SAMPLE: bits 0-14 mV,
 *                 bit 15 charging
 *   m000, m001..  rundown-m only: each hour's motor on-time, two bytes per
 *                 sample (M1, then M2), in RD_MOTOR_UNIT_MS steps (0-240)
 * Each hour's blob takes 6 NVS entries of 32 bytes (with the m### blob, 12):
 * a 3-day run takes about 430 of the ~630 entries the 24 KB partition can
 * use, a 12-hour rundown-m night about 145.
 */
#define RD_BLOCK 60                 /* samples per stored blob: an hour */
#define RD_SAMPLE(mv, chg) ((uint16_t)(((mv) & 0x7FFF) | ((chg) ? 0x8000 : 0)))
#define RD_MOTOR_UNIT_MS 250        /* motor on-time steps: a minute is 240 of them, so one byte holds it */
#define RD_META_VERSION 2           /* bump when rd_meta_t changes (2: + sf, v0.6.0) */

/* The run's details, written when it starts (all fields single bytes or
 * text, so the stored layout has no padding). */
typedef struct {
    uint8_t version;    /* RD_META_VERSION */
    uint8_t test;       /* tt_test_t */
    uint8_t paths;      /* tt_paths_t */
    uint8_t role;       /* tt_role_t */
    int8_t tx_dbm;      /* transmit power during the run */
    uint8_t unit_lo;    /* reading speed: the unit in ms (low byte, then high byte) */
    uint8_t unit_hi;
    uint8_t m1_pct;     /* motor intensity trims, % */
    uint8_t m2_pct;
    uint8_t motors;     /* 1: the m### blobs are kept (rundown-m) */
    char own[8];        /* this board's ID */
    char peer[8];       /* its partner's */
    char session[24];   /* the session name ("console" from the console) */
    char fw[16];        /* firmware version that recorded it */
    char end[12];       /* how it ended (app / console / hold); "" if it never
                           stopped: the battery ran out or the board restarted */
    uint8_t sf;         /* spreading factor during the run (version 2; a version-1
                           log, from v0.5.1, was always SF9: store_rd_meta fills it in) */
} rd_meta_t;

/* Start a run-down log: erase the old one, store the test type and meta. */
esp_err_t store_rd_begin(const rd_meta_t *meta);
/* Store hour `block` (n samples so far) and the total sample count; motor:
 * NULL, or 2 * n bytes of motor on-time (rundown-m). */
esp_err_t store_rd_save(int block, const uint16_t *samples, const uint8_t *motor, int n, uint32_t total);
/* Note how the run ended (meta.end). */
esp_err_t store_rd_end(const char *reason);
/* The last run-down: its test type and sample count. False if there is none. */
bool store_rd_info(uint8_t *test, uint32_t *total);
/* The last run-down's details. False if there are none (no log, or one
 * written by firmware older than v0.5.1). A v0.5.1 log (version 1) is
 * read too, as version 2 with sf = 9. */
bool store_rd_meta(rd_meta_t *meta);
/* Read hour `block` into samples[] (at most RD_BLOCK). Returns how many. */
int store_rd_load(int block, uint16_t *samples);
/* Read hour `block`'s motor on-time into motor[] (at most 2 * RD_BLOCK
 * bytes). Returns how many samples it covers (0: none stored). */
int store_rd_load_motor(int block, uint8_t *motor);

/* Erase everything except the device ID and the message-ID counter (a
 * counter restarting from scratch would get fresh packets dropped as
 * duplicates by devices that heard the old ones). */
esp_err_t store_factory_reset(void);
