/*
 * MorseLink — persistent storage (see store.h).
 */
#include "store.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "hseq.h"
#include "morse.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "store";
static const char *NS = "morselink"; /* NVS namespace for everything we keep */

/* The settings layout used by v0.1-v0.2 (SETTINGS_VERSION 1), so that it
 * can be read and converted. Identical to settings_t up to link_addr. */
typedef struct {
    uint16_t version;
    uint8_t speed_q, farnsworth_x10, overlap_ms, dash_m1_cu, m1_pct, m2_pct;
    uint8_t discoverable, loopback, rail_gating, reserved;
    uint32_t link_addr;
} settings_v1_t;

/*
 * settings_default() -- the factory settings.
 * Out: *s overwritten. Loopback is off now that the radio exists (it was on
 * by default in v0.2, when it was the only way to test messages).
 */
void settings_default(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->version = SETTINGS_VERSION;
    s->speed_q = 5;
    s->farnsworth_x10 = 10;
    s->overlap_cu = 30;  /* (agreed 24 Oct) M1 1.4U, M2 1.9U, 0.3U together */
    s->dash_m1_cu = 125;
    s->m1_pct = 100;
    s->m2_pct = 100;
    s->discoverable = 1;
    s->loopback = 0;
    s->rail_gating = 1;
    s->tx_dbm = RADIO_TX_DBM_DEFAULT; /* (the SX1276 takes at most 20: the driver clamps) */
    s->link_addr = ADDR_NONE;
    s->link_ended = 0;
    s->sf = RADIO_SF_DEFAULT;
}

/*
 * store_init() -- initialise NVS. If the partition is full or was written
 * by an incompatible version of the NVS format, it is erased and started
 * afresh (losing the ID: the console says so at boot).
 */
esp_err_t store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing (%s)", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

/*
 * load_blob() -- read blob `key` into `out` (at most `size` bytes).
 * Returns the number of bytes read, or -1 if missing or larger than size.
 */
static int load_blob(const char *key, void *out, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return -1;
    }
    size_t len = size;
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK ? (int)len : -1;
}

/* save_blob() -- write blob `key` and commit. Logs and returns any error. */
static esp_err_t save_blob(const char *key, const void *data, size_t size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, data, size);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

/* store_load_id() -- the device ID, validated; "" and false if none. */
bool store_load_id(char out[5])
{
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 5;
    esp_err_t err = nvs_get_str(h, "id", out, &len);
    nvs_close(h);
    if (err != ESP_OK || !id_valid(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/* store_save_id() -- store a (validated) device ID. */
esp_err_t store_save_id(const char *id)
{
    if (!id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "id", id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/*
 * store_load_settings() -- the stored settings, converted if necessary.
 * From version 2 (v0.3-v0.5.1; same layout): SF9, and the transmit power
 * moves to the new default, the chip's maximum (agreed 15 Oct: everything
 * so far ran at 17 dBm; `radio power` sets another level). From version 1
 * (v0.1-v0.2) the conversion keeps the reading and haptic settings, and:
 *  - turns loopback off (on by default in v0.2, it would now hide the radio);
 *  - drops the link: v0.2 links were only ever local (no radio), so the
 *    other device never agreed to them (connect again with S);
 *  - sets the transmit power and SF to the defaults.
 * The converted settings are saved straight away.
 */
bool store_load_settings(settings_t *s)
{
    union {
        settings_t v2;
        settings_v1_t v1;
    } buf;
    memset(&buf, 0, sizeof(buf));
    int len = load_blob("settings", &buf, sizeof(buf));
    if (len == (int)sizeof(settings_t) && buf.v2.version == SETTINGS_VERSION) {
        *s = buf.v2;
    } else if (len == (int)sizeof(settings_t) && buf.v2.version == 3) {
        /* v0.6.0-v0.6.3: the same layout. I 1-5 (3.0-3.4 V) becomes 1-9:
         * 5 (3.4 V) -> 9, 4 (3.3 V, the default) -> 8, 1-3 (3.0-3.2 V) -> 7
         * (about the same force); the dash takes its new shape (agreed 24
         * Oct; the overlap byte was in ms, now in hundredths of U). */
        *s = buf.v2;
        uint8_t was = s->strength;
        s->version = SETTINGS_VERSION;
        s->strength = was == 0 ? 0 : was >= 5 ? 9 : was == 4 ? 8 : 7;
        s->overlap_cu = 30;
        s->dash_m1_cu = 125;
        ESP_LOGW(TAG, "settings from v0.6.3 or older: strength I%d -> I%d (now 1-9), the dash M1 1.4U / M2 1.9U "
                      "with 0.3U together", was, s->strength);
        store_save_settings(s);
    } else if (len == (int)sizeof(settings_t) && buf.v2.version == 2) {
        *s = buf.v2;
        s->version = SETTINGS_VERSION;
        s->sf = RADIO_SF_DEFAULT;
        s->tx_dbm = RADIO_TX_DBM_DEFAULT;
        s->strength = 0;
        s->dash_style = 0;
        s->overlap_cu = 30; /* (the dash's new shape, version 4) */
        s->dash_m1_cu = 125;
        ESP_LOGW(TAG, "settings from v0.5 or older: SF%d, transmit power now %d dBm (the chip's most; "
                      "`radio power 17` for the old level)", RADIO_SF_DEFAULT, RADIO_TX_DBM_DEFAULT);
        store_save_settings(s);
    } else if (len == (int)sizeof(settings_v1_t) && buf.v1.version == 1) {
        settings_v1_t old = buf.v1;
        settings_default(s);
        s->speed_q = old.speed_q;
        s->farnsworth_x10 = old.farnsworth_x10;
        /* (the dash's overlap and split keep version 4's defaults: the old bytes meant something else) */
        s->m1_pct = old.m1_pct;
        s->m2_pct = old.m2_pct;
        s->discoverable = old.discoverable;
        s->rail_gating = old.rail_gating;
        ESP_LOGW(TAG, "settings converted from v0.2: loopback off, previous recipient cleared (connect with S)");
        store_save_settings(s);
    } else {
        settings_default(s);
        return false;
    }
    if (s->speed_q < 1 || s->speed_q > 9) {
        s->speed_q = 5;
    }
    if (s->tx_dbm < RADIO_TX_DBM_MIN || s->tx_dbm > RADIO_TX_DBM_MAX) {
        s->tx_dbm = RADIO_TX_DBM_DEFAULT;
    }
    if (s->sf < RADIO_SF_MIN || s->sf > RADIO_SF_MAX) {
        s->sf = RADIO_SF_DEFAULT;
    }
    if (s->dash_style > HDASH_M2) {
        s->dash_style = 0; /* (0 = never set: the sweep) */
    }
    if (s->strength > HSTRENGTH_MAX) {
        s->strength = 0; /* (0 = never set; a stray value counts as that) */
    }
    if (s->overlap_cu > 100) {
        s->overlap_cu = 30;
    }
    return true;
}

/* store_save_settings() -- write the settings blob. */
esp_err_t store_save_settings(const settings_t *s)
{
    return save_blob("settings", s, sizeof(*s));
}

/* store_load_calib() -- the profile, if stored, current and valid. */
bool store_load_calib(cal_profile_t *p)
{
    return load_blob("calib", p, sizeof(*p)) == (int)sizeof(*p) && p->version == CAL_PROFILE_VERSION && p->valid;
}

/* store_save_calib() -- write the calibration profile. */
esp_err_t store_save_calib(const cal_profile_t *p)
{
    return save_blob("calib", p, sizeof(*p));
}

/* store_erase_calib() -- forget the calibration (first use again). */
esp_err_t store_erase_calib(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(h, "calib");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* store_load_peers() -- contacts and blocklist; an empty table if none. */
bool store_load_peers(peer_table_t *t)
{
    if (load_blob("peers", t, sizeof(*t)) != (int)sizeof(*t) || !peers_valid(t)) {
        peers_init(t);
        return false;
    }
    return true;
}

/* store_save_peers() -- write contacts and blocklist. */
esp_err_t store_save_peers(const peer_table_t *t)
{
    return save_blob("peers", t, sizeof(*t));
}

/* store_load_msgid() -- where the next block of message IDs starts. */
bool store_load_msgid(uint16_t *next_block)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u16(h, "msgid", next_block);
    nvs_close(h);
    return err == ESP_OK;
}

/* store_save_msgid() -- called by the link engine about once every
 * MSGID_BLOCK packets (and once per boot). */
esp_err_t store_save_msgid(uint16_t next_block)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(h, "msgid", next_block);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* store_load_power_off() -- was the device switched off (F)? Read at every
 * boot, before anything else starts (power.c). */
bool store_load_power_off(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, "off", &v);
    nvs_close(h);
    return err == ESP_OK && v != 0;
}

/* store_save_power_off() -- record the switch position. Written only when
 * it changes: at F, and when the key switches the device on. */
esp_err_t store_save_power_off(bool off)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "off", off ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* store_load_ble_on() -- has a phone been accepted (O), so that BLE should
 * start at every boot? Absent means no (a new device, or after a factory
 * reset or `phone forget`). */
bool store_load_ble_on(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, "ble", &v);
    nvs_close(h);
    return err == ESP_OK && v != 0;
}

/* store_save_ble_on() -- record it (written when a phone is accepted in the
 * O window, and when the phones are forgotten). */
esp_err_t store_save_ble_on(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "ble", on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* store_load_ble_off() -- did the user switch Bluetooth off with W (v0.6.1)?
 * Absent means no. Like "ble", a factory reset clears it. */
bool store_load_ble_off(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, "bleoff", &v);
    nvs_close(h);
    return err == ESP_OK && v != 0;
}

/* store_save_ble_off() -- record it (W, `phone off` / `phone on`, and O,
 * which clears it). Written only when it changes, so a repeated W in the
 * same direction costs no flash write. */
esp_err_t store_save_ble_off(bool off)
{
    if (store_load_ble_off() == off) {
        return ESP_OK;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "bleoff", off ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------------- */
/* Run-down log (test mode)                                                   */
/* ------------------------------------------------------------------------- */

static const char *RD_NS = "rundown";

/* store_rd_begin() -- see store.h: erase the namespace, then "info" = the
 * test type and no samples yet, and "meta" = the run's details. */
esp_err_t store_rd_begin(const rd_meta_t *meta)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RD_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    uint8_t info[5] = {meta->test, 0, 0, 0, 0};
    err = nvs_set_blob(h, "info", info, sizeof info);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "meta", meta, sizeof *meta);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* store_rd_end() -- see store.h: rewrite "meta" with the end reason. */
esp_err_t store_rd_end(const char *reason)
{
    rd_meta_t m;
    if (!store_rd_meta(&m)) {
        return ESP_ERR_NOT_FOUND; /* no log, or an old one without meta */
    }
    strncpy(m.end, reason, sizeof m.end - 1);
    m.end[sizeof m.end - 1] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(RD_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, "meta", &m, sizeof m);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* store_rd_meta() -- see store.h. Only a blob of exactly the layout its
 * version had counts: version 2 (this one), or version 1 (v0.5.1: the same
 * without sf at the end), which is filled in as SF9 and then reads as
 * version 2 (so store_rd_end() rewrites it in the new layout). */
bool store_rd_meta(rd_meta_t *meta)
{
    nvs_handle_t h;
    if (nvs_open(RD_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    memset(meta, 0, sizeof *meta);
    size_t len = sizeof *meta;
    esp_err_t err = nvs_get_blob(h, "meta", meta, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    if (len == offsetof(rd_meta_t, sf) && meta->version == 1) {
        meta->version = RD_META_VERSION;
        meta->sf = 9; /* everything before v0.6.0 ran at SF9 */
        return true;
    }
    return len == sizeof *meta && meta->version == RD_META_VERSION;
}

/* store_rd_save() -- see store.h. The hour's blobs are rewritten as they
 * grow (every 10 minutes), then "info" with the new total. */
esp_err_t store_rd_save(int block, const uint16_t *samples, const uint8_t *motor, int n, uint32_t total)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RD_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    snprintf(key, sizeof key, "b%03d", block % 1000);
    err = nvs_set_blob(h, key, samples, (size_t)n * sizeof(uint16_t));
    if (err == ESP_OK && motor) {
        snprintf(key, sizeof key, "m%03d", block % 1000);
        err = nvs_set_blob(h, key, motor, (size_t)n * 2); /* M1, M2 per sample */
    }
    if (err == ESP_OK) {
        uint8_t info[5];
        size_t len = sizeof info;
        if (nvs_get_blob(h, "info", info, &len) != ESP_OK || len != sizeof info) {
            info[0] = 0xFF;
        }
        info[1] = (uint8_t)total;
        info[2] = (uint8_t)(total >> 8);
        info[3] = (uint8_t)(total >> 16);
        info[4] = (uint8_t)(total >> 24);
        err = nvs_set_blob(h, "info", info, sizeof info);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* store_rd_info() -- see store.h. */
bool store_rd_info(uint8_t *test, uint32_t *total)
{
    nvs_handle_t h;
    if (nvs_open(RD_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t info[5];
    size_t len = sizeof info;
    esp_err_t err = nvs_get_blob(h, "info", info, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof info) {
        return false;
    }
    *test = info[0];
    *total = info[1] | ((uint32_t)info[2] << 8) | ((uint32_t)info[3] << 16) | ((uint32_t)info[4] << 24);
    return true;
}

/* store_rd_load() -- see store.h. */
int store_rd_load(int block, uint16_t *samples)
{
    nvs_handle_t h;
    if (nvs_open(RD_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    char key[8];
    snprintf(key, sizeof key, "b%03d", block % 1000);
    size_t len = RD_BLOCK * sizeof(uint16_t);
    esp_err_t err = nvs_get_blob(h, key, samples, &len);
    nvs_close(h);
    return err == ESP_OK ? (int)(len / sizeof(uint16_t)) : 0;
}

/* store_rd_load_motor() -- see store.h. */
int store_rd_load_motor(int block, uint8_t *motor)
{
    nvs_handle_t h;
    if (nvs_open(RD_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    char key[8];
    snprintf(key, sizeof key, "m%03d", block % 1000);
    size_t len = RD_BLOCK * 2;
    esp_err_t err = nvs_get_blob(h, key, motor, &len);
    nvs_close(h);
    return err == ESP_OK ? (int)(len / 2) : 0;
}

/*
 * store_factory_reset() -- erase the namespace, then put back the device ID
 * and the message-ID counter (see store.h for why).
 */
esp_err_t store_factory_reset(void)
{
    char id[5];
    bool have_id = store_load_id(id);
    uint16_t msgid;
    bool have_msgid = store_load_msgid(&msgid);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    if (have_id) {
        nvs_set_str(h, "id", id);
    }
    if (have_msgid) {
        nvs_set_u16(h, "msgid", msgid);
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
