/*
 * MorseLink — the BLE link to the owner's phone (see ble.h for what it does,
 * phone.h for the lines it carries).
 *
 * Built on NimBLE, like the owner's first BLE test (a Nordic UART Service on
 * ESP-IDF's bleprph example), with bonding and the O window added.
 *
 * Who runs what:
 *   - NimBLE's host task runs the GAP and GATT callbacks below: connections,
 *     pairing, encryption, and the phone writing to RX. Each line the phone
 *     writes is parsed there and handed on straight away (a relayed packet
 *     to the net task, a hello to the UI), so nothing slow runs in it.
 *   - The "ble_tx" task sends the lines ble_send_line() queued, as
 *     notifications cut to the connection's MTU, and once a second looks
 *     after the O window and connections that never get accepted.
 * The connection state below is written by the host task and read by
 * others; a spinlock keeps each read consistent. NimBLE's own API is
 * thread-safe.
 *
 * Accepting a phone (spec §12):
 *   - every connection is asked to encrypt at once (our security request):
 *     a phone we have a bond with re-encrypts with it, a new one pairs
 *     ("Just Works", LE Secure Connections, bonding);
 *   - when encryption is up, outside the O window: a bond that existed when
 *     the phone connected is accepted; a new one is deleted again and the
 *     phone disconnected;
 *   - while the O window is open it is the other way round: a new bond is
 *     accepted (it replaces every other: one phone per device), closes the
 *     window, and the user hears OK; the paired phone is turned away, so
 *     the one connection stays free for the new phone (it reconnects by
 *     itself when the window ends). 60 s without a new phone: error;
 *   - a known phone that lost its keys and pairs from scratch counts as
 *     new (NimBLE's "repeat pairing": allowed only while the window is open);
 *   - a connection not accepted within BLE_UNACCEPTED_MS is dropped, so a
 *     stranger can't hold the only connection.
 */
#include "ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs.h"
#include "phone.h"
#include "sdkconfig.h"
#include "store.h"
#include "ui.h"

#if !CONFIG_BT_NIMBLE_ENABLED
#error "BLE is off in this build's sdkconfig. Run  idf.py set-target esp32  once (or delete the file sdkconfig) so the settings in sdkconfig.defaults are picked up."
#endif

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void); /* NimBLE's NVS bond store (no header declares it) */

static const char *TAG = "ble";

#define TXQ_LEN 16                   /* lines waiting to be sent to the phone */
#define BLE_UNACCEPTED_MS 30000      /* a connection not accepted by then is dropped */
#define NOTIFY_RETRY_MS 20           /* NimBLE's buffers were full: try again this soon */
#define NOTIFY_GIVE_UP_MS 2000       /* ... for at most this long, then drop the line */
#define BONDS_MAX 4                  /* bonds looked at (the store keeps CONFIG_BT_NIMBLE_MAX_BONDS) */
#define ADV_ITVL_MIN 0x0140          /* advertising every 200-500 ms (0.625 ms units): */
#define ADV_ITVL_MAX 0x0320          /* the phone finds us within a second, at little cost */
#define NIMBLE_BOND_NS "nimble_bond" /* NimBLE's NVS namespace for bonds (ble_store_nvs.c) */

/* One line on its way to the phone (with room for the '\n' we add). */
typedef struct {
    char s[PHONE_LINE_MAX + 2];
} line_t;

/* ------------------------------------------------------------------------- */
/* Nordic UART Service (UUIDs written backwards, as NimBLE stores them)       */
/* ------------------------------------------------------------------------- */

/* Service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E */
static const ble_uuid128_t NUS_SVC = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3,
                                                      0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
/* RX 6E400002-...: the phone writes here */
static const ble_uuid128_t NUS_RX = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3,
                                                     0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
/* TX 6E400003-...: we notify here */
static const ble_uuid128_t NUS_TX = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3,
                                                     0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

static struct {
    portMUX_TYPE lock;
    QueueHandle_t txq;
    char name[24];               /* advertised name: "MorseLink-K7Q" */
    volatile bool started;       /* NimBLE is running */
    volatile bool synced;        /* host and controller in step: we may advertise */
    uint8_t own_addr_type;
    uint16_t tx_handle;          /* value handle of TX, for notifications */
    /* the connection (at most one) */
    uint16_t conn;               /* BLE_HS_CONN_HANDLE_NONE: nobody */
    int64_t conn_since;          /* when it connected (ms) */
    bool encrypted, accepted, subscribed;
    uint16_t mtu;
    ble_addr_t known[BONDS_MAX]; /* bonds that existed when this phone connected */
    int n_known;
    int n_bonds;                 /* bonds in the store, last time we looked (-1: unknown) */
    /* O */
    int64_t window_until;        /* ms; 0: closed */
    /* bench power's step 3a: advertising, not connectable (ble_set_connectable) */
    volatile bool nonconn;
    volatile bool sending;       /* tx_task is sending a line right now (ble_flush waits for it) */
    /* the console's pretend phone */
    volatile bool sim;
    /* line assembly from RX writes (host task only; the pretend phone feeds
     * whole lines and doesn't use it) */
    phone_rx_t rx;
    uint32_t lines_in, lines_out, lines_dropped;
} B = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .name = "MorseLink",
    .conn = BLE_HS_CONN_HANDLE_NONE,
    .mtu = 23,
    .n_bonds = -1,
};

/* now_ms() -- milliseconds since boot. */
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ready_now() -- can a line reach the phone right now? (The pretend phone
 * always can; a real one must be connected, accepted and subscribed.) */
static bool ready_now(void)
{
    bool r;
    portENTER_CRITICAL(&B.lock);
    r = B.sim || (B.conn != BLE_HS_CONN_HANDLE_NONE && B.accepted && B.subscribed);
    portEXIT_CRITICAL(&B.lock);
    return r;
}

/* became_ready() -- after a change that may have made the phone ready: if
 * it wasn't before (`was`) and is now, tell the UI, which sends HI, ST and
 * DR. */
static void became_ready(bool was)
{
    if (!was && ready_now()) {
        ESP_LOGI(TAG, "phone ready (MTU %u)", B.mtu);
        ui_post_phone(UIP_READY);
    }
}

/* count_bonds() -- refresh B.n_bonds from the store (host task, or before
 * NimBLE runs: then unknown). */
static void count_bonds(void)
{
    ble_addr_t a[BONDS_MAX];
    int n = 0;
    B.n_bonds = ble_store_util_bonded_peers(a, &n, BONDS_MAX) == 0 ? n : -1;
}

/* ------------------------------------------------------------------------- */
/* Lines from the phone                                                       */
/* ------------------------------------------------------------------------- */

/*
 * handle_line() -- one whole line from the phone (or the console's pretend
 * phone). HI asks for a fresh HI, ST and DR (the UI sends them); RL carries
 * a packet from MQTT, which goes to the net task as if the radio had heard
 * it; TEST START / TEST STOP and BENCH POWER / BENCH STOP go to the UI,
 * which owns starting a test or the meter session. Anything else is
 * ignored, so a newer app can send more.
 */
static void handle_line(const char *line)
{
    phone_in_t in; /* (~130 bytes; NimBLE's host task has 4 KB of stack) */
    phone_parse(line, &in);
    B.lines_in++;
    switch (in.kind) {
    case PHONE_IN_HI:
        ESP_LOGI(TAG, "phone >> HI");
        ui_post_phone(UIP_HELLO);
        break;
    case PHONE_IN_RELAY:
        net_phone_rx(in.pkt, in.len); /* (`radio log on` logs it, decoded) */
        break;
    case PHONE_IN_TEST_START:
        ESP_LOGI(TAG, "phone >> TEST START %s", in.args);
        ui_post_test_cmd(true, in.args);
        break;
    case PHONE_IN_TEST_STOP:
        ESP_LOGI(TAG, "phone >> TEST STOP");
        ui_post_test_cmd(false, "");
        break;
    case PHONE_IN_BENCH_POWER:
        ESP_LOGI(TAG, "phone >> BENCH POWER");
        ui_post_bench_cmd(true);
        break;
    case PHONE_IN_BENCH_STOP:
        ESP_LOGI(TAG, "phone >> BENCH STOP");
        ui_post_bench_cmd(false);
        break;
    case PHONE_IN_BAD:
        ESP_LOGW(TAG, "phone >> a malformed line, ignored: %.40s", line);
        break;
    default:
        break; /* a line only a newer app knows: ignored */
    }
}

/*
 * nus_access() -- GATT callback for both NUS characteristics. Only RX is
 * writable (TX just notifies). Bytes from an accepted phone go through the
 * line assembler; each completed line is handled at once. Writes need an
 * encrypted link (BLE_GATT_CHR_F_WRITE_ENC), which makes the phone pair or
 * re-encrypt first; until the phone is accepted, its bytes are dropped.
 */
static int nus_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    static uint8_t buf[512]; /* host task only; bigger than any write at our MTU */
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &n) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (!B.accepted || conn != B.conn) {
        B.lines_dropped++;
        return 0;
    }
    for (uint16_t i = 0; i < n; i++) {
        if (phone_rx_byte(&B.rx, buf[i])) {
            handle_line(B.rx.buf);
        }
    }
    return 0;
}

/* The GATT table: one service, RX (write, encrypted) and TX (notify). */
static const struct ble_gatt_svc_def SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &NUS_RX.u,
                    .access_cb = nus_access,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
                },
                {
                    .uuid = &NUS_TX.u,
                    .access_cb = nus_access,
                    .val_handle = &B.tx_handle,
                    .flags = BLE_GATT_CHR_F_NOTIFY,
                },
                {0},
            },
    },
    {0},
};

/* ------------------------------------------------------------------------- */
/* Advertising and connections                                                */
/* ------------------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *ev, void *arg);

/*
 * advertise() -- be visible and connectable as B.name (when NimBLE is in
 * step and nobody is connected). The name goes in the advertisement, the
 * NUS UUID in the scan response (both don't fit in 31 bytes), so an app can
 * find us by either. During bench power's step 3a (B.nonconn) the same
 * advertisement goes out but no connection is accepted (a scannable,
 * non-connectable advertisement): the meter sees advertising alone.
 */
static void advertise(void)
{
    if (!B.started || !B.synced || B.conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
        return; /* (not while ble_stop() is taking NimBLE down, either) */
    }
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof f);
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)B.name;
    f.name_len = (uint8_t)strlen(B.name);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc == 0) {
        struct ble_hs_adv_fields r;
        memset(&r, 0, sizeof r);
        r.uuids128 = &NUS_SVC;
        r.num_uuids128 = 1;
        r.uuids128_is_complete = 1;
        rc = ble_gap_adv_rsp_set_fields(&r);
    }
    if (rc == 0) {
        struct ble_gap_adv_params p;
        memset(&p, 0, sizeof p);
        p.conn_mode = B.nonconn ? BLE_GAP_CONN_MODE_NON : BLE_GAP_CONN_MODE_UND;
        p.disc_mode = BLE_GAP_DISC_MODE_GEN;
        p.itvl_min = ADV_ITVL_MIN;
        p.itvl_max = ADV_ITVL_MAX;
        rc = ble_gap_adv_start(B.own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising failed (%d)", rc);
    }
}

/* drop() -- end the connection (we decided it isn't welcome). */
static void drop(uint16_t conn)
{
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

/* was_known() -- did a bond with this identity exist when the phone connected? */
static bool was_known(const ble_addr_t *id)
{
    for (int i = 0; i < B.n_known; i++) {
        if (ble_addr_cmp(&B.known[i], id) == 0) {
            return true;
        }
    }
    return false;
}

/* forget_others() -- delete every bond except `keep`'s: one phone per device. */
static void forget_others(const ble_addr_t *keep)
{
    ble_addr_t a[BONDS_MAX];
    int n = 0;
    if (ble_store_util_bonded_peers(a, &n, BONDS_MAX) != 0) {
        return;
    }
    for (int i = 0; i < n; i++) {
        if (ble_addr_cmp(&a[i], keep) != 0) {
            ble_store_util_delete_peer(&a[i]);
        }
    }
}

/*
 * on_encrypted() -- encryption came up (or failed) on `conn`: decide whether
 * this phone is accepted (see the top of the file).
 */
static void on_encrypted(uint16_t conn, int status)
{
    struct ble_gap_conn_desc d;
    if (status != 0) {
        ESP_LOGW(TAG, "pairing / encryption failed (%d)%s", status,
                 ble_window_open() ? "" : ": if the phone forgot this device, use O and pair again");
        drop(conn);
        return;
    }
    if (B.accepted && conn == B.conn) {
        return; /* encrypted again on the same connection (a key refresh): still accepted */
    }
    if (ble_gap_conn_find(conn, &d) != 0) {
        return;
    }
    if (!d.sec_state.bonded) {
        ESP_LOGW(TAG, "the phone encrypted without bonding: not accepted");
        drop(conn);
        return;
    }
    bool window = ble_window_open();
    bool known = was_known(&d.peer_id_addr);
    if (!known && !window) {
        ESP_LOGW(TAG, "a new phone paired while the O window was closed: refused (its keys deleted)");
        ble_store_util_delete_peer(&d.peer_id_addr);
        count_bonds();
        drop(conn);
        return;
    }
    if (known && window) {
        /* The one connection stays free for the new phone the owner is
         * pairing (the paired phone would otherwise win it straight back
         * by reconnecting). It reconnects by itself once the window ends. */
        ESP_LOGW(TAG, "the paired phone waits: the O window is open for a new one");
        drop(conn);
        return;
    }
    if (!known) { /* new, while the window is open: the owner's new phone */
        forget_others(&d.peer_id_addr); /* one phone per device */
        B.known[0] = d.peer_id_addr;    /* known from now on, for the rest of this connection too */
        B.n_known = 1;
        portENTER_CRITICAL(&B.lock);
        B.window_until = 0;
        portEXIT_CRITICAL(&B.lock);
        store_save_ble_on(true); /* from now on BLE starts at every boot */
        ESP_LOGI(TAG, "phone paired: it reconnects by itself from now on");
        ui_post_phone(UIP_PAIRED);
    }
    count_bonds();
    bool was = ready_now();
    portENTER_CRITICAL(&B.lock);
    B.encrypted = B.accepted = true;
    portEXIT_CRITICAL(&B.lock);
    became_ready(was);
}

/*
 * gap_event() -- NimBLE's GAP callback (host task): connections coming and
 * going, encryption, subscriptions, MTU, a known phone pairing again.
 * Returns 0 except where NimBLE expects a decision (repeat pairing).
 */
static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc d;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            advertise(); /* the attempt failed: be visible again */
            return 0;
        }
        portENTER_CRITICAL(&B.lock);
        B.conn = ev->connect.conn_handle;
        B.conn_since = now_ms();
        B.encrypted = B.accepted = B.subscribed = false;
        B.mtu = 23;
        portEXIT_CRITICAL(&B.lock);
        B.n_known = 0;
        if (ble_store_util_bonded_peers(B.known, &B.n_known, BONDS_MAX) != 0) {
            B.n_known = 0;
        }
        phone_rx_init(&B.rx);
        ESP_LOGI(TAG, "a phone connected%s", ble_window_open() ? " (pairing window open)" : "");
        ble_gap_security_initiate(ev->connect.conn_handle); /* re-encrypt with a bond, or pair */
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        bool was = ready_now();
        portENTER_CRITICAL(&B.lock);
        B.conn = BLE_HS_CONN_HANDLE_NONE;
        B.encrypted = B.accepted = B.subscribed = false;
        portEXIT_CRITICAL(&B.lock);
        ESP_LOGI(TAG, "phone disconnected (reason 0x%x)", ev->disconnect.reason);
        if (was && !B.sim) {
            ui_post_phone(UIP_GONE);
        }
        advertise();
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        on_encrypted(ev->enc_change.conn_handle, ev->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == B.tx_handle) {
            bool was = ready_now();
            portENTER_CRITICAL(&B.lock);
            B.subscribed = ev->subscribe.cur_notify != 0;
            portEXIT_CRITICAL(&B.lock);
            became_ready(was);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        B.mtu = ev->mtu.value;
        ESP_LOGI(TAG, "MTU %u", B.mtu);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* A phone we have a bond with wants to pair from scratch: it lost its
         * keys (e.g. "forget device" on the phone). Only the owner may decide
         * that, with the O window open. */
        if (ble_window_open() && ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
            ble_store_util_delete_peer(&d.peer_id_addr);
            B.n_known = 0; /* it pairs as a new phone now */
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        ESP_LOGW(TAG, "a known phone wants to pair again: press O on the device first");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    default:
        return 0;
    }
}

/* on_sync() -- NimBLE is ready: pick our address and start advertising. */
static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &B.own_addr_type);
    B.synced = true;
    count_bonds();
    ESP_LOGI(TAG, "on as \"%s\" (%d phone%s remembered)", B.name, B.n_bonds, B.n_bonds == 1 ? "" : "s");
    advertise();
}

/* on_reset() -- the host and controller fell out of step; NimBLE resyncs by
 * itself (on_sync again). */
static void on_reset(int reason)
{
    B.synced = false;
    ESP_LOGW(TAG, "BLE host reset (reason %d)", reason);
}

/* host_task() -- NimBLE's host loop; returns only when nimble_port_stop() runs. */
static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------------- */
/* Sending lines                                                              */
/* ------------------------------------------------------------------------- */

/*
 * send_now() -- one line to the phone as notifications of at most MTU - 3
 * bytes (the phone joins them up again at the '\n'). If NimBLE's buffers
 * are full, each piece is retried for up to NOTIFY_GIVE_UP_MS; a line that
 * can't be finished (gone, or stuck) is dropped and counted.
 */
static void send_now(char *s)
{
    size_t len = strlen(s);
    s[len++] = '\n';
    size_t off = 0;
    int64_t t0 = now_ms();
    while (off < len) {
        uint16_t conn, mtu;
        portENTER_CRITICAL(&B.lock);
        bool ok = B.conn != BLE_HS_CONN_HANDLE_NONE && B.accepted && B.subscribed;
        conn = B.conn;
        mtu = B.mtu;
        portEXIT_CRITICAL(&B.lock);
        if (!ok) {
            B.lines_dropped++;
            return;
        }
        size_t chunk = len - off;
        if (chunk > (size_t)(mtu - 3)) {
            chunk = (size_t)(mtu - 3);
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s + off, (uint16_t)chunk);
        int rc = om ? ble_gatts_notify_custom(conn, B.tx_handle, om) : BLE_HS_ENOMEM; /* (om always consumed) */
        if (rc == 0) {
            off += chunk;
            continue;
        }
        if ((rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) || now_ms() - t0 > NOTIFY_GIVE_UP_MS) {
            B.lines_dropped++;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_MS)); /* buffers full: let some go out */
    }
    B.lines_out++;
}

/*
 * housekeeping() -- once a second: close the O window when its time is up
 * (error for the user if no phone came), and drop a connection that hasn't
 * been accepted within BLE_UNACCEPTED_MS.
 */
static void housekeeping(void)
{
    int64_t now = now_ms();
    bool expired = false;
    uint16_t stale = BLE_HS_CONN_HANDLE_NONE;
    portENTER_CRITICAL(&B.lock);
    if (B.window_until && now >= B.window_until) {
        B.window_until = 0;
        expired = true;
    }
    if (B.conn != BLE_HS_CONN_HANDLE_NONE && !B.accepted && now - B.conn_since > BLE_UNACCEPTED_MS) {
        stale = B.conn;
    }
    portEXIT_CRITICAL(&B.lock);
    if (expired) {
        ESP_LOGW(TAG, "pairing window closed: no phone paired");
        ui_post_phone(UIP_WINDOW_CLOSED);
    }
    if (stale != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "a phone stayed connected without being accepted: disconnected");
        drop(stale);
    }
}

/* tx_task() -- sends queued lines; wakes at least once a second for
 * housekeeping(). */
static void tx_task(void *arg)
{
    (void)arg;
    static line_t ln; /* (only this task uses it) */
    for (;;) {
        if (xQueueReceive(B.txq, &ln, pdMS_TO_TICKS(1000)) == pdTRUE) {
            B.sending = true;
            send_now(ln.s);
            B.sending = false;
        }
        if (B.started) {
            housekeeping();
        }
    }
}

/* ========================================================================= */
/* API                                                                        */
/* ========================================================================= */

/* ble_init() -- see ble.h: the line queue and its task; the radio stays off. */
void ble_init(void)
{
    B.txq = xQueueCreate(TXQ_LEN, sizeof(line_t));
    xTaskCreatePinnedToCore(tx_task, "ble_tx", 3072, NULL, 5, NULL, 0);
}

/* ble_set_name() -- see ble.h. If we are advertising, start again under the
 * new name. */
void ble_set_name(const char *own_id)
{
    if (own_id && own_id[0]) {
        snprintf(B.name, sizeof B.name, "MorseLink-%s", own_id);
    } else {
        strcpy(B.name, "MorseLink"); /* no ID yet */
    }
    if (B.started && B.synced) {
        ble_svc_gap_device_name_set(B.name);
        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
            advertise();
        }
    }
}

/*
 * ble_start() -- see ble.h. NimBLE (controller and host) comes up, the NUS
 * service is registered, bonds are loaded from NVS, and advertising starts
 * once host and controller are in step (on_sync).
 */
esp_err_t ble_start(void)
{
    if (B.started) {
        return ESP_OK;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE didn't start: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO; /* no display, no keyboard: "Just Works" */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;                       /* LE Secure Connections */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID; /* their IRK: we
                                       recognise the phone behind its changing private address */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(SVCS);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(SVCS);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE service registration failed (%d)", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(B.name);
    ble_store_config_init();
    B.started = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

/*
 * ble_stop() -- see ble.h. First nothing new may start (no notification,
 * no housekeeping, no advertising: `started` goes false) and one already
 * being sent gets a moment to finish; then NimBLE disconnects the phone
 * and shuts down, which waits for its host task to end. The UI hears
 * UIP_GONE if a phone was ready.
 */
void ble_stop(void)
{
    if (!B.started) {
        return;
    }
    bool was = ready_now();
    portENTER_CRITICAL(&B.lock);
    B.started = false;
    B.accepted = B.subscribed = false;
    B.window_until = 0;
    portEXIT_CRITICAL(&B.lock);
    vTaskDelay(pdMS_TO_TICKS(50)); /* a notification on its way (ble_tx) finishes */
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    }
    portENTER_CRITICAL(&B.lock);
    B.synced = false;
    B.conn = BLE_HS_CONN_HANDLE_NONE;
    B.encrypted = false;
    portEXIT_CRITICAL(&B.lock);
    if (was && !B.sim) {
        ui_post_phone(UIP_GONE);
    }
    ESP_LOGI(TAG, "off");
}

/* ble_set_connectable() -- see ble.h. A change while advertising restarts
 * the advertisement in the new mode (NimBLE's API is thread-safe). */
void ble_set_connectable(bool on)
{
    if (B.nonconn == !on) {
        return;
    }
    B.nonconn = !on;
    if (B.started && B.synced && ble_gap_adv_active()) {
        ble_gap_adv_stop();
        advertise();
    }
    ESP_LOGI(TAG, "advertising %s", on ? "connectable again" : "without accepting connections (bench power)");
}

/* ble_flush() -- see ble.h: polls the queue and the line in progress every
 * 10 ms. */
void ble_flush(int timeout_ms)
{
    int64_t end = now_ms() + timeout_ms;
    while (B.txq && !B.sim && ready_now() && (uxQueueMessagesWaiting(B.txq) > 0 || B.sending) && now_ms() < end) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*
 * ble_open_window() -- see ble.h. A phone connected right now is
 * disconnected, so that the new one can connect (there is room for only
 * one); the paired phone is let back in once the window has closed.
 */
esp_err_t ble_open_window(void)
{
    esp_err_t err = ble_start();
    if (err != ESP_OK) {
        return err;
    }
    uint16_t conn;
    portENTER_CRITICAL(&B.lock);
    B.window_until = now_ms() + BLE_WINDOW_MS;
    conn = B.conn;
    portEXIT_CRITICAL(&B.lock);
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        drop(conn);
    }
    ESP_LOGI(TAG, "pairing window open for %d s: connect to \"%s\" from the phone", BLE_WINDOW_MS / 1000, B.name);
    return ESP_OK;
}

/* ble_window_open() -- see ble.h. */
bool ble_window_open(void)
{
    bool open;
    portENTER_CRITICAL(&B.lock);
    open = B.window_until != 0 && now_ms() < B.window_until;
    portEXIT_CRITICAL(&B.lock);
    return open;
}

/* ble_phone_ready() -- see ble.h. */
bool ble_phone_ready(void)
{
    return ready_now();
}

/*
 * ble_send_line() -- see ble.h. The pretend phone gets the line in the log
 * straight away; a real one through the queue and tx_task.
 */
void ble_send_line(const char *line)
{
    if (B.sim) {
        ESP_LOGI(TAG, "phone << %s", line);
        B.lines_out++;
        return;
    }
    if (!ready_now() || !B.txq) {
        B.lines_dropped++;
        return;
    }
    line_t ln;
    strncpy(ln.s, line, PHONE_LINE_MAX);
    ln.s[PHONE_LINE_MAX] = '\0';
    if (xQueueSend(B.txq, &ln, 0) != pdTRUE) {
        B.lines_dropped++;
    }
}

/* ble_tx_space() -- see ble.h. */
int ble_tx_space(void)
{
    if (B.sim) {
        return TXQ_LEN;
    }
    return B.txq ? (int)uxQueueSpacesAvailable(B.txq) : 0;
}

/* ble_sim() -- see ble.h. Switching on tells the UI a phone is ready (so it
 * sends HI, ST and DR); off, that it is gone. */
void ble_sim(bool on)
{
    bool was = B.sim;
    B.sim = on;
    if (on && !was) {
        ESP_LOGI(TAG, "pretend phone connected: lines to it appear as \"phone << ...\"");
        ui_post_phone(UIP_READY);
    } else if (!on && was) {
        ESP_LOGI(TAG, "pretend phone gone");
        ui_post_phone(UIP_GONE);
    }
}

/* ble_sim_line() -- see ble.h. Ignored unless the pretend phone is on. */
void ble_sim_line(const char *line)
{
    if (!B.sim) {
        ESP_LOGW(TAG, "phone sim is off: line ignored");
        return;
    }
    char buf[PHONE_LINE_MAX + 1];
    strncpy(buf, line, PHONE_LINE_MAX);
    buf[PHONE_LINE_MAX] = '\0';
    handle_line(buf);
}

/*
 * ble_forget_phones() -- see ble.h. BLE is stopped first (a connected phone
 * is dropped), so nothing uses the bond store while NimBLE's NVS namespace
 * is erased underneath it. BLE then stays off, now and at boot, until O.
 */
void ble_forget_phones(void)
{
    ble_stop();
    nvs_handle_t h;
    if (nvs_open(NIMBLE_BOND_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    B.n_bonds = 0;
    store_save_ble_on(false);
    ESP_LOGI(TAG, "paired phone forgotten: BLE is off until the next O");
}

/* ble_get_status() -- see ble.h: a consistent snapshot for the console. */
void ble_get_status(ble_status_t *s)
{
    memset(s, 0, sizeof(*s));
    int64_t now = now_ms();
    portENTER_CRITICAL(&B.lock);
    s->started = B.started;
    s->connected = B.conn != BLE_HS_CONN_HANDLE_NONE;
    s->encrypted = B.encrypted;
    s->accepted = B.accepted;
    s->subscribed = B.subscribed;
    s->mtu = B.mtu;
    s->window_s = B.window_until && now < B.window_until ? (int32_t)((B.window_until - now + 999) / 1000) : 0;
    portEXIT_CRITICAL(&B.lock);
    s->advertising = B.started && B.synced && ble_gap_adv_active();
    strncpy(s->name, B.name, sizeof(s->name) - 1);
    s->sim = B.sim;
    s->bonds = B.started ? B.n_bonds : -1;
    s->lines_in = B.lines_in;
    s->lines_out = B.lines_out;
    s->lines_dropped = B.lines_dropped;
}
