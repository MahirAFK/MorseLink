/*
 * MorseLink — packet layer (spec §6).
 *
 * Everything that is pure bookkeeping about packets, with no radio and no
 * ESP-IDF in it, so it runs unchanged in the host tests:
 *
 *   - the 10-byte header and the payload of each packet type
 *     (encode / decode / validate);
 *   - LoRa airtime, the time a packet occupies the channel;
 *   - the duty-cycle budget (10 % of any rolling hour, SOS exempt);
 *   - duplicate suppression: a ring of recently seen (source, message ID);
 *   - message IDs that never restart at 0 after a reboot.
 *
 * Header layout (big-endian, 10 bytes):
 *
 *   byte 0     version (high nibble) | type (low nibble)
 *   byte 1     flags: bits 0-1 hop count (SOS relays), bit 2 sent with the
 *              long "wake-up" preamble, bit 3 relayed via the phone (BLE
 *              step), bits 4-7 reserved (0)
 *   bytes 2-4  source address (24-bit, spec §4)
 *   bytes 5-7  destination address (24-bit; 0xFFFFFF = everyone)
 *   bytes 8-9  message ID: retries of one packet reuse it, so receivers can
 *              drop the copies they have already seen
 *   bytes 10.. payload (see the table under pkt_type_t)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define PROTO_VERSION 1
#define PROTO_HDR_LEN 10
#define PROTO_MAX_PAYLOAD 112 /* a 100-character message plus room to spare */
#define PROTO_MAX_PKT (PROTO_HDR_LEN + PROTO_MAX_PAYLOAD)

/* Packet types: 4 bits. 0 is never sent, 14 and 15 are spare.
 *
 *   type          payload                                  answered by
 *   DISCOVER      slots (1), slot length in 10 ms (1)      PRESENCE (each discoverable device, in a random slot)
 *   PRESENCE      sender's dBm (1), sweep tag (1)          -   (tag = low byte of the DISCOVER's ID)
 *   LINK_REQ      none                                     LINK_REPLY
 *   LINK_REPLY    status (1), ref = request's ID (2)       PAIR_CONFIRM if ACCEPT
 *   PAIR_CONFIRM  ref = the ACCEPT's ID (2)                -
 *   KEEPALIVE     none                                     LINK_REPLY ENDED if not linked
 *   MSG           the text, 1..100 characters              MSG_RECEIPT
 *   MSG_RECEIPT   ref = the message's ID (2)               -
 *   SOS           none (Phase 2: a position)               SOS_ACK (by a person)
 *   SOS_ACK       none                                     -
 *   SOS_CANCEL    none                                     -
 *   UNPAIR        none                                     -
 *   PING          kind (1), RSSI (1), SNR x4 (1), dBm (1)  PING kind=pong (bench tool)
 */
typedef enum {
    PT_NONE = 0,
    PT_DISCOVER = 1,
    PT_PRESENCE = 2,
    PT_LINK_REQ = 3,
    PT_LINK_REPLY = 4,
    PT_PAIR_CONFIRM = 5,
    PT_KEEPALIVE = 6,
    PT_MSG = 7,
    PT_MSG_RECEIPT = 8,
    PT_SOS = 9,
    PT_SOS_ACK = 10,
    PT_SOS_CANCEL = 11,
    PT_UNPAIR = 12,
    PT_PING = 13,
    PT__COUNT
} pkt_type_t;

/* Header flags (byte 1). */
#define PF_HOPS_MASK 0x03      /* how many times an SOS-family packet has been relayed */
#define PF_LONG_PREAMBLE 0x04  /* sent with RADIO_PREAMBLE_LONG (Phase 2: wakes a sleeping receiver) */
#define PF_VIA_PHONE 0x08      /* arrived through the phone relay (BLE step) */
#define PF_KNOWN_MASK 0x0F     /* the bits defined so far */

/* LINK_REPLY status byte. */
typedef enum {
    LR_RECEIVED = 1, /* "your request reached me": stops the requester's retries, silent */
    LR_BUSY = 2,     /* "I'm in a conversation": the request waits in my queue */
    LR_ACCEPT = 3,   /* "yes": the requester links and answers PAIR_CONFIRM */
    LR_DECLINE = 4,  /* "no" */
    LR_ENDED = 5,    /* "we are not linked (any more)": the link moved on, or
                        an accept arrived for a request that is no longer wanted */
} link_reply_t;

/* PING payload byte 0. */
#define PING_KIND_PING 0
#define PING_KIND_PONG 1

/* A decoded packet. The payload is copied in, so the struct can outlive the
 * receive buffer. */
typedef struct {
    uint8_t type;   /* pkt_type_t */
    uint8_t flags;  /* PF_* bits */
    uint32_t src;   /* 24-bit address of the device that created the packet */
    uint32_t dst;   /* 24-bit address it is for, or ADDR_BROADCAST */
    uint16_t id;    /* message ID */
    uint8_t plen;   /* payload length */
    uint8_t payload[PROTO_MAX_PAYLOAD];
} pkt_t;

typedef enum {
    PROTO_OK = 0,
    PROTO_ERR_SHORT,    /* fewer bytes than a header */
    PROTO_ERR_LONG,     /* more than PROTO_MAX_PKT */
    PROTO_ERR_VERSION,  /* someone else's protocol, or a future version */
    PROTO_ERR_TYPE,     /* unknown or unused type code */
    PROTO_ERR_ADDR,     /* source or destination is not a device address */
    PROTO_ERR_PAYLOAD,  /* payload too short or too long for its type */
} proto_err_t;

/* Short printable names, for logs. */
const char *proto_type_name(uint8_t type);
const char *proto_err_str(proto_err_t e);
const char *proto_reply_name(uint8_t status);

/* True if `a` is a 3- or 4-character device address (spec §4). */
bool proto_addr_valid(uint32_t a);

/* Build a packet. Returns its length, or -1 if `p` is malformed (payload too
 * long, address out of range) or `out_size` is too small. No side effects. */
int proto_encode(const pkt_t *p, uint8_t *out, int out_size);

/* Parse and validate a received packet into `out`. Returns PROTO_OK, or the
 * reason it was rejected (out is then undefined). No side effects. */
proto_err_t proto_decode(const uint8_t *buf, int len, pkt_t *out);

/* Fill in a packet header (payload empty). Convenience for the link engine. */
void proto_init_pkt(pkt_t *p, uint8_t type, uint32_t src, uint32_t dst, uint16_t id);

/* Big-endian 16-bit helpers for payload fields such as "ref". */
void proto_put16(uint8_t *b, uint16_t v);
uint16_t proto_get16(const uint8_t *b);

/* ------------------------------------------------------------------------- */
/* Airtime                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t sf;      /* spreading factor 6..12 */
    uint32_t bw_hz;  /* bandwidth */
    uint8_t cr;      /* coding rate 4/(4+cr), cr = 1..4 */
} lora_params_t;

/* Time a packet of `len` bytes occupies the channel, in microseconds:
 * explicit header, CRC on, low-data-rate optimisation when a symbol lasts
 * more than 16 ms (Semtech's formula, SX1276 datasheet §4.1.1.7). */
uint32_t lora_airtime_us(int len, const lora_params_t *p, int preamble_syms);

/* True if this SF/BW combination needs low-data-rate optimisation. */
bool lora_needs_ldro(const lora_params_t *p);

/* ------------------------------------------------------------------------- */
/* Duty cycle                                                                 */
/* ------------------------------------------------------------------------- */

/* Airtime per minute for the last RADIO_DUTY_WINDOW_MIN minutes. */
typedef struct {
    uint32_t used_ms[RADIO_DUTY_WINDOW_MIN];
    int64_t minute[RADIO_DUTY_WINDOW_MIN]; /* which absolute minute each bucket holds */
} dc_t;

void dc_init(dc_t *d);
/* The budget for one window, in ms (10 % of an hour = 360 000). */
uint32_t dc_budget_ms(void);
/* Airtime used in the window that ends at `now_ms`. */
uint32_t dc_used_ms(const dc_t *d, int64_t now_ms);
/* Would `airtime_ms` more still fit in the budget? */
bool dc_allow(const dc_t *d, int64_t now_ms, uint32_t airtime_ms);
/* Record a transmission (SOS too: it is exempt from the limit, not from the count). */
void dc_add(dc_t *d, int64_t now_ms, uint32_t airtime_ms);

/* ------------------------------------------------------------------------- */
/* Duplicate suppression                                                      */
/* ------------------------------------------------------------------------- */

#define DEDUP_N 48                      /* remembered (source, ID) pairs */
#define DEDUP_KEEP_MS (10 * 60 * 1000)  /* ... for at most this long */

typedef struct {
    uint32_t src;
    uint16_t id;
    int64_t t; /* when it was last seen */
} dedup_ent_t;

typedef struct {
    dedup_ent_t e[DEDUP_N];
    uint8_t head; /* next slot to overwrite */
    uint8_t n;    /* valid entries */
} dedup_t;

void dedup_init(dedup_t *d);
/* Has (src, id) been seen in the last DEDUP_KEEP_MS? If not, remember it now.
 * Returns true for a duplicate. */
bool dedup_check(dedup_t *d, uint32_t src, uint16_t id, int64_t now_ms);

/* ------------------------------------------------------------------------- */
/* Message IDs                                                                */
/* ------------------------------------------------------------------------- */

/* IDs are reserved in blocks: flash stores "the first ID of the next block",
 * and on boot the device jumps straight to it. A device that restarted from 0
 * would have its fresh packets dropped as duplicates by everyone who heard
 * the old ones. Writing once per block instead of once per packet keeps the
 * flash wear negligible. */
#define MSGID_BLOCK 1000

typedef void (*msgid_save_fn)(void *ctx, uint16_t next_block);

typedef struct {
    uint16_t next;  /* next ID to hand out */
    uint16_t limit; /* first ID of the next (unreserved) block */
    msgid_save_fn save;
    void *ctx;
} msgid_t;

/* Start from the stored value and reserve one block (calls `save` once). */
void msgid_init(msgid_t *m, uint16_t stored, msgid_save_fn save, void *ctx);
/* A fresh, non-zero ID. Calls `save` when a new block has to be reserved. */
uint16_t msgid_next(msgid_t *m);
