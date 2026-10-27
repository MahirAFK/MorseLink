/*
 * MorseLink — packet layer (see proto.h for the format).
 *
 * Pure C: no ESP-IDF, no allocation, no globals. Every function either
 * transforms its arguments or updates the state struct it is given.
 */
#include "proto.h"

#include <string.h>

#include "morse.h"

/* ------------------------------------------------------------------------- */
/* Names (for logs)                                                           */
/* ------------------------------------------------------------------------- */

/*
 * proto_type_name() -- printable name of a packet type.
 * In: type, a pkt_type_t value (anything else gives "?").
 * Returns a static string. No side effects.
 */
const char *proto_type_name(uint8_t type)
{
    static const char *const names[PT__COUNT] = {
        "-",        "DISCOVER", "PRESENCE",    "LINK_REQ", "LINK_REPLY", "PAIR_CONFIRM", "KEEPALIVE",
        "MSG",      "RECEIPT",  "SOS",         "SOS_ACK",  "SOS_CANCEL", "UNPAIR",       "PING",
    };
    return type < PT__COUNT ? names[type] : "?";
}

/*
 * proto_err_str() -- printable reason a packet was rejected.
 * In: e, a proto_err_t. Returns a static string. No side effects.
 */
const char *proto_err_str(proto_err_t e)
{
    switch (e) {
    case PROTO_OK:
        return "ok";
    case PROTO_ERR_SHORT:
        return "too short";
    case PROTO_ERR_LONG:
        return "too long";
    case PROTO_ERR_VERSION:
        return "other protocol/version";
    case PROTO_ERR_TYPE:
        return "unknown type";
    case PROTO_ERR_ADDR:
        return "bad address";
    case PROTO_ERR_PAYLOAD:
        return "bad payload length";
    }
    return "?";
}

/*
 * proto_reply_name() -- printable name of a LINK_REPLY status byte.
 * In: status, a link_reply_t. Returns a static string. No side effects.
 */
const char *proto_reply_name(uint8_t status)
{
    switch (status) {
    case LR_RECEIVED:
        return "RECEIVED";
    case LR_BUSY:
        return "BUSY";
    case LR_ACCEPT:
        return "ACCEPT";
    case LR_DECLINE:
        return "DECLINE";
    case LR_ENDED:
        return "ENDED";
    }
    return "?";
}

/* ------------------------------------------------------------------------- */
/* Encode / decode                                                            */
/* ------------------------------------------------------------------------- */

/*
 * proto_addr_valid() -- is `a` a real device address?
 * In: a, a 24-bit wire address. Returns true for any 3- or 4-character ID
 * (spec §4); false for broadcast, "none" and the unused space above them.
 */
bool proto_addr_valid(uint32_t a)
{
    char id[ID_MAX_LEN + 1];
    return addr_to_id(a, id); /* the ID codec knows exactly which values are IDs */
}

/*
 * proto_put16() / proto_get16() -- big-endian 16-bit field access.
 * In: b points at 2 bytes. put16 writes v there; get16 returns the value.
 */
void proto_put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

/* proto_get16() -- the reading half of the pair above. */
uint16_t proto_get16(const uint8_t *b)
{
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

/* Write / read a 24-bit address, most significant byte first. */
static void put24(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 16);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)v;
}

/* get24() -- the reading half of put24(). */
static uint32_t get24(const uint8_t *b)
{
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
}

/*
 * payload_ok() -- is `plen` a sensible payload length for `type`?
 * Each type has a minimum (the fields it must carry) and a maximum. The
 * maxima leave room for fields a later version may append, so an older
 * device still accepts the packet and ignores the extra bytes.
 */
static bool payload_ok(uint8_t type, int plen)
{
    switch (type) {
    case PT_LINK_REPLY:
        return plen >= 3 && plen <= 8; /* status + ref */
    case PT_PAIR_CONFIRM:
    case PT_MSG_RECEIPT:
        return plen >= 2 && plen <= 8; /* ref */
    case PT_MSG:
        return plen >= 1 && plen <= MSG_MAX_CHARS; /* the text itself */
    case PT_PING:
        return plen >= 4 && plen <= 8; /* kind, rssi, snr, dBm */
    case PT_DISCOVER:
    case PT_PRESENCE:
        return plen >= 2 && plen <= 16; /* DISCOVER: slots, slot length; PRESENCE: dBm, sweep tag */
    case PT_LINK_REQ:
    case PT_KEEPALIVE:
    case PT_SOS:
    case PT_SOS_ACK:
    case PT_SOS_CANCEL:
    case PT_UNPAIR:
        return plen <= 16; /* nothing required yet */
    default:
        return false;
    }
}

/*
 * proto_init_pkt() -- start a packet: header fields set, flags 0, no payload.
 * Out: *p is overwritten.
 */
void proto_init_pkt(pkt_t *p, uint8_t type, uint32_t src, uint32_t dst, uint16_t id)
{
    memset(p, 0, sizeof(*p));
    p->type = type;
    p->src = src;
    p->dst = dst;
    p->id = id;
}

/*
 * proto_encode() -- serialise `p` into `out` (see proto.h for the layout).
 * Returns the number of bytes written, or -1 if the packet is malformed or
 * does not fit (all checks happen first, so nothing is written then).
 */
int proto_encode(const pkt_t *p, uint8_t *out, int out_size)
{
    int len = PROTO_HDR_LEN + p->plen;
    if (p->plen > PROTO_MAX_PAYLOAD || len > out_size || p->type == PT_NONE || p->type >= PT__COUNT) {
        return -1;
    }
    if (!proto_addr_valid(p->src) || !(proto_addr_valid(p->dst) || p->dst == ADDR_BROADCAST)) {
        return -1;
    }
    out[0] = (uint8_t)((PROTO_VERSION << 4) | (p->type & 0x0F)); /* version | type */
    out[1] = p->flags;
    put24(&out[2], p->src);
    put24(&out[5], p->dst);
    proto_put16(&out[8], p->id);
    if (p->plen) {
        memcpy(&out[PROTO_HDR_LEN], p->payload, p->plen);
    }
    return len;
}

/*
 * proto_decode() -- parse and validate a received packet.
 * In:  buf/len, the bytes the radio delivered.
 * Out: *out holds the packet when PROTO_OK is returned.
 * Checks, in order: length, version, type, addresses, payload length. Our
 * own packets are not filtered here (the link engine does that).
 */
proto_err_t proto_decode(const uint8_t *buf, int len, pkt_t *out)
{
    if (len < PROTO_HDR_LEN) {
        return PROTO_ERR_SHORT;
    }
    if (len > PROTO_MAX_PKT) {
        return PROTO_ERR_LONG;
    }
    if ((buf[0] >> 4) != PROTO_VERSION) {
        return PROTO_ERR_VERSION;
    }
    out->type = buf[0] & 0x0F;
    if (out->type == PT_NONE || out->type >= PT__COUNT) {
        return PROTO_ERR_TYPE;
    }
    out->flags = buf[1];
    out->src = get24(&buf[2]);
    out->dst = get24(&buf[5]);
    out->id = proto_get16(&buf[8]);
    /* The source must be a real device; the destination a device or everyone. */
    if (!proto_addr_valid(out->src) || !(proto_addr_valid(out->dst) || out->dst == ADDR_BROADCAST)) {
        return PROTO_ERR_ADDR;
    }
    out->plen = (uint8_t)(len - PROTO_HDR_LEN);
    if (!payload_ok(out->type, out->plen)) {
        return PROTO_ERR_PAYLOAD;
    }
    memcpy(out->payload, &buf[PROTO_HDR_LEN], out->plen);
    return PROTO_OK;
}

/* ------------------------------------------------------------------------- */
/* Airtime                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * symbol_ns() -- duration of one LoRa symbol, 2^SF / BW, in nanoseconds.
 * For SF9 at 125 kHz that is exactly 4 096 000 ns (4.096 ms).
 */
static uint64_t symbol_ns(const lora_params_t *p)
{
    return ((uint64_t)1 << p->sf) * 1000000000ull / p->bw_hz;
}

/*
 * lora_needs_ldro() -- low-data-rate optimisation is mandatory when a
 * symbol lasts longer than 16 ms (SF11/SF12 at 125 kHz). Both ends must
 * agree on it, so the radio driver uses this same test.
 */
bool lora_needs_ldro(const lora_params_t *p)
{
    return symbol_ns(p) > 16000000ull;
}

/*
 * lora_airtime_us() -- how long a packet occupies the channel.
 * In: len payload bytes as handed to the radio (our header included),
 *     p modem settings, preamble_syms the programmed preamble length.
 * Returns microseconds. Pure.
 *
 *   Tpreamble = (Npreamble + 4.25) * Tsym
 *   Npayload  = 8 + max(ceil((8*PL - 4*SF + 28 + 16*CRC - 20*IH)
 *                             / (4*(SF - 2*DE))) * (CR + 4), 0)
 * with CRC = 1 (on), IH = 0 (explicit header), DE = low-data-rate opt.
 * Example: 10 bytes at SF9/125 kHz/CR4/5 -> 23 payload symbols, 144.4 ms.
 */
uint32_t lora_airtime_us(int len, const lora_params_t *p, int preamble_syms)
{
    const int sf = p->sf;
    const int de = lora_needs_ldro(p) ? 1 : 0;
    const int crc = 1, ih = 0;
    uint64_t tsym = symbol_ns(p);
    /* (N + 4.25) * Tsym computed as (4N + 17) * Tsym / 4 to stay in integers. */
    uint64_t pre_ns = (uint64_t)(4 * preamble_syms + 17) * tsym / 4;
    int num = 8 * len - 4 * sf + 28 + 16 * crc - 20 * ih;
    int den = 4 * (sf - 2 * de);
    int blocks = num > 0 ? (num + den - 1) / den : 0; /* ceil(num / den), never negative */
    int syms = 8 + blocks * (p->cr + 4);
    uint64_t total_ns = pre_ns + (uint64_t)syms * tsym;
    return (uint32_t)(total_ns / 1000u);
}

/* ------------------------------------------------------------------------- */
/* Duty cycle                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * The window is RADIO_DUTY_WINDOW_MIN one-minute buckets. Bucket i holds the
 * airtime used during one absolute minute (now_ms / 60000), remembered in
 * minute[i] so that a bucket left over from an older pass round the ring
 * reads as empty. Accuracy: a transmission is counted for up to one minute
 * longer than strictly necessary, which only ever errs on the safe side.
 */

/* dc_init() -- empty window. Out: *d reset. */
void dc_init(dc_t *d)
{
    memset(d->used_ms, 0, sizeof(d->used_ms));
    for (int i = 0; i < RADIO_DUTY_WINDOW_MIN; i++) {
        d->minute[i] = -1000; /* "long ago" */
    }
}

/* dc_budget_ms() -- airtime allowed per window, in ms. */
uint32_t dc_budget_ms(void)
{
    return (uint32_t)RADIO_DUTY_WINDOW_MIN * 60u * 1000u * RADIO_DUTY_PERMILLE / 1000u;
}

/* dc_used_ms() -- airtime used within the window ending now. Pure. */
uint32_t dc_used_ms(const dc_t *d, int64_t now_ms)
{
    int64_t m = now_ms / 60000;
    uint32_t sum = 0;
    for (int i = 0; i < RADIO_DUTY_WINDOW_MIN; i++) {
        if (d->minute[i] > m - RADIO_DUTY_WINDOW_MIN) { /* still inside the window */
            sum += d->used_ms[i];
        }
    }
    return sum;
}

/* dc_allow() -- would another `airtime_ms` stay within the budget? Pure. */
bool dc_allow(const dc_t *d, int64_t now_ms, uint32_t airtime_ms)
{
    return dc_used_ms(d, now_ms) + airtime_ms <= dc_budget_ms();
}

/* dc_add() -- count a transmission in the current minute's bucket. */
void dc_add(dc_t *d, int64_t now_ms, uint32_t airtime_ms)
{
    int64_t m = now_ms / 60000;
    int i = (int)(m % RADIO_DUTY_WINDOW_MIN);
    if (d->minute[i] != m) { /* the bucket still holds an old minute: recycle it */
        d->minute[i] = m;
        d->used_ms[i] = 0;
    }
    d->used_ms[i] += airtime_ms;
}

/* ------------------------------------------------------------------------- */
/* Duplicate suppression                                                      */
/* ------------------------------------------------------------------------- */

/* dedup_init() -- forget everything. */
void dedup_init(dedup_t *d)
{
    memset(d, 0, sizeof(*d));
}

/*
 * dedup_check() -- the receive-side filter.
 * In: src/id of a packet just received, the current time.
 * Returns true if the same (src, id) arrived within DEDUP_KEEP_MS (a retry
 * or a relayed copy). Otherwise records it, overwriting the oldest entry
 * when the ring is full, and returns false.
 */
bool dedup_check(dedup_t *d, uint32_t src, uint16_t id, int64_t now_ms)
{
    for (int i = 0; i < d->n; i++) {
        dedup_ent_t *e = &d->e[i];
        if (e->src == src && e->id == id && now_ms - e->t < DEDUP_KEEP_MS) {
            e->t = now_ms; /* still being repeated: keep remembering it */
            return true;
        }
    }
    d->e[d->head].src = src;
    d->e[d->head].id = id;
    d->e[d->head].t = now_ms;
    d->head = (uint8_t)((d->head + 1) % DEDUP_N);
    if (d->n < DEDUP_N) {
        d->n++;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Message IDs                                                                */
/* ------------------------------------------------------------------------- */

/*
 * msgid_init() -- resume numbering after a (re)boot.
 * In: stored, the value last saved (the first ID of the block that was
 *     next in line); save/ctx, how to persist a new value.
 * Side effect: reserves [stored, stored + MSGID_BLOCK) by saving its end
 * straight away, so a crash before any ID is used still skips the block.
 */
void msgid_init(msgid_t *m, uint16_t stored, msgid_save_fn save, void *ctx)
{
    m->next = stored;
    m->limit = (uint16_t)(stored + MSGID_BLOCK); /* 16-bit wrap-around is fine */
    m->save = save;
    m->ctx = ctx;
    if (m->save) {
        m->save(m->ctx, m->limit);
    }
}

/*
 * msgid_next() -- hand out the next ID.
 * Returns a non-zero ID (0 means "no reference" in some payloads).
 * Side effect: when the reserved block runs out, reserves the next one and
 * saves its end (about once every MSGID_BLOCK packets).
 */
uint16_t msgid_next(msgid_t *m)
{
    for (;;) {
        if (m->next == m->limit) {
            m->limit = (uint16_t)(m->limit + MSGID_BLOCK);
            if (m->save) {
                m->save(m->ctx, m->limit);
            }
        }
        uint16_t id = m->next++;
        if (id != 0) {
            return id;
        }
    }
}
