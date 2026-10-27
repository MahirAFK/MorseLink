/*
 * MorseLink — discovery bookkeeping (spec §7 "Discovery"; packets in proto.h).
 *
 * `D` asks "who is around?". The link engine (link.c) does the talking on
 * air; this file holds the bookkeeping it needs, in pure C (no ESP-IDF), so
 * each piece is tested on its own in test/host:
 *
 *   - the found list: one entry per device that answered, sorted into
 *     near / mid / far by path loss, keeping arrival order inside each group;
 *   - the answer limits: a device answers the same searcher only a few times
 *     per 10 minutes (and everyone together a few more), so nobody can track
 *     it by sweeping over and over.
 *
 * How a sweep goes (timing constants in config.h):
 *
 *   searcher                         every discoverable device in range
 *   DISCOVER (slots, slot length) -> picks a random slot, waits for it,
 *                                 <- PRESENCE (its transmit power, sweep tag)
 *   listens for the whole window, then sorts what it heard and the user
 *   walks the list: ID + N / M / F each, tap to invite, double tap to skip.
 *
 * Simultaneous LoRa packets destroy each other, hence the random slots. Path
 * loss (their transmit power minus the strength we heard them at) is fairer
 * than raw signal strength when boards use different power. It still swings
 * by ±20 dB with how the boards are held, so only three coarse groups.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

/* Most devices one sweep reports. Sized so the whole list fits in one event
 * to the user interface (link.h checks this at compile time); a dozen is
 * already a long list to walk by touch. */
#define DISC_MAX 12

/* Payload lengths (bytes after the 10-byte header). Two bytes each keep both
 * packets at the 144 ms airtime of a bare header at SF9: a third byte would
 * cost a whole extra block of symbols (165 ms). */
#define DISC_DISCOVER_LEN 2 /* slots, slot length in 10 ms steps */
#define DISC_PRESENCE_LEN 2 /* the answerer's transmit power (dBm), sweep tag */

/* What an answerer accepts from a DISCOVER (anything outside is clamped), so
 * a strange or hostile sweep can't make everyone answer at the same moment
 * or keep a packet queued for minutes. */
#define DISC_SLOTS_MAX 32
#define DISC_SLOT_MIN_MS 100
#define DISC_SLOT_MAX_MS 500

typedef enum {
    DISC_NEAR = 0, /* plays as N */
    DISC_MID,      /* plays as M */
    DISC_FAR,      /* plays as F */
} disc_prox_t;

/* One device found by a sweep. 8 bytes: DISC_MAX of them fit in the space an
 * event normally uses for message text. */
typedef struct {
    uint32_t addr; /* who answered (24-bit address) */
    int16_t rssi;  /* how strongly we heard their PRESENCE, dBm */
    int8_t dbm;    /* their transmit power, from the PRESENCE */
    uint8_t prox;  /* disc_prox_t, worked out from the path loss */
} disc_entry_t;

/* The list one sweep collects. */
typedef struct {
    uint8_t n;
    disc_entry_t e[DISC_MAX];
} disc_list_t;

/* Path loss of an entry in dB: their transmit power minus the RSSI here. */
int disc_loss_db(const disc_entry_t *e);

/* Near / mid / far for a path loss (thresholds DISC_NEAR_LOSS_DB and
 * DISC_FAR_LOSS_DB in config.h). */
disc_prox_t disc_prox_of(int loss_db);

/* The Morse letter for a group: 'N', 'M' or 'F'. */
char disc_prox_letter(uint8_t prox);

/* A printable word for logs: "near", "mid" or "far". */
const char *disc_prox_name(uint8_t prox);

/* Empty the list. */
void disc_list_clear(disc_list_t *l);

/* Add a device that answered. Returns false (list unchanged) if it is
 * already listed (its first answer counts) or the list is full. */
bool disc_list_add(disc_list_t *l, uint32_t addr, int16_t rssi, int8_t dbm);

/* Put the list in walking order: near first, then mid, then far; within a
 * group, the order the answers arrived in (a stable sort). */
void disc_list_sort(disc_list_t *l);

/* ------------------------------------------------------------------------- */
/* Answer limits                                                              */
/* ------------------------------------------------------------------------- */

#define DISC_LIM_SRC_N 8 /* searchers remembered at once (the least recent drops out) */

/* One searcher we have answered: when, for the last few answers (a ring). */
typedef struct {
    uint32_t addr;                     /* ADDR_NONE: free entry */
    int64_t t[DISC_ANSWERS_PER_SRC];   /* answer times; DISC_LONG_AGO when unused */
    uint8_t head;                      /* the oldest of them: the next to overwrite */
} disc_src_t;

typedef struct {
    disc_src_t src[DISC_LIM_SRC_N];
    int64_t all[DISC_ANSWERS_TOTAL];   /* our most recent answers to anyone (a ring) */
    uint8_t all_head;                  /* the oldest of them */
} disc_limit_t;

/* "Never": far enough in the past that now - DISC_LONG_AGO can't overflow. */
#define DISC_LONG_AGO (INT64_MIN / 4)

/* Forget every answer. */
void disc_limit_init(disc_limit_t *lim);

/* May we answer a DISCOVER from `src` now? True if both limits allow it,
 * and then the answer is counted; false changes nothing. */
bool disc_limit_allow(disc_limit_t *lim, uint32_t src, int64_t now);
