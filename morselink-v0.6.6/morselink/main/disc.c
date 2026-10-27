/*
 * MorseLink — discovery bookkeeping (see disc.h for the whole story).
 *
 * Pure C: no ESP-IDF, no allocation, no globals. Every function either
 * reads its arguments or updates the struct it is given.
 */
#include "disc.h"

#include <string.h>

#include "morse.h" /* ADDR_NONE */

/* ------------------------------------------------------------------------- */
/* Proximity                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * disc_loss_db() -- how much signal was lost between them and us, in dB.
 * In: e, a found device. Returns their transmit power minus the RSSI we
 * measured. Bigger means further away (or more in the way). Pure.
 * Example: sent at 17 dBm, heard at -60 dBm -> 77 dB.
 */
int disc_loss_db(const disc_entry_t *e)
{
    return (int)e->dbm - (int)e->rssi;
}

/*
 * disc_prox_of() -- which group a path loss falls in.
 * In: loss_db (see disc_loss_db). Returns DISC_NEAR below
 * DISC_NEAR_LOSS_DB, DISC_FAR from DISC_FAR_LOSS_DB up, DISC_MID between.
 * Pure.
 */
disc_prox_t disc_prox_of(int loss_db)
{
    if (loss_db < DISC_NEAR_LOSS_DB) {
        return DISC_NEAR;
    }
    if (loss_db >= DISC_FAR_LOSS_DB) {
        return DISC_FAR;
    }
    return DISC_MID;
}

/*
 * disc_prox_letter() -- the letter played after a device's ID.
 * In: prox, a disc_prox_t. Returns 'N', 'M' or 'F' ('?' for anything else).
 */
char disc_prox_letter(uint8_t prox)
{
    static const char letters[] = {'N', 'M', 'F'};
    return prox < sizeof(letters) ? letters[prox] : '?';
}

/*
 * disc_prox_name() -- the group as a word, for the console log.
 * In: prox, a disc_prox_t. Returns a static string.
 */
const char *disc_prox_name(uint8_t prox)
{
    static const char *const names[] = {"near", "mid", "far"};
    return prox < 3 ? names[prox] : "?";
}

/* ------------------------------------------------------------------------- */
/* The found list                                                             */
/* ------------------------------------------------------------------------- */

/* disc_list_clear() -- no devices. Out: *l emptied. */
void disc_list_clear(disc_list_t *l)
{
    memset(l, 0, sizeof(*l));
}

/*
 * disc_list_add() -- note a device that answered our sweep.
 * In:  addr who it is; rssi how strongly we heard it (dBm); dbm the transmit
 *      power it reported.
 * Out: appended to *l, with its group worked out from the path loss.
 * Returns false, leaving the list as it was, when the device is already
 * listed (its first answer counts: answers aren't repeated, so a second
 * one would be a stray copy) or the list is full.
 */
bool disc_list_add(disc_list_t *l, uint32_t addr, int16_t rssi, int8_t dbm)
{
    for (int i = 0; i < l->n; i++) {
        if (l->e[i].addr == addr) {
            return false;
        }
    }
    if (l->n >= DISC_MAX) {
        return false;
    }
    disc_entry_t *e = &l->e[l->n++];
    e->addr = addr;
    e->rssi = rssi;
    e->dbm = dbm;
    e->prox = (uint8_t)disc_prox_of(disc_loss_db(e));
    return true;
}

/*
 * disc_list_sort() -- walking order: near, mid, far.
 * An insertion sort on the group alone: it is stable, so devices in the same
 * group keep the order their answers arrived in (spec: RSSI swings too much
 * for a finer ranking to mean anything). At most DISC_MAX entries, so the
 * quadratic worst case is trivial.
 */
void disc_list_sort(disc_list_t *l)
{
    for (int i = 1; i < l->n; i++) {
        disc_entry_t x = l->e[i];
        int j = i - 1;
        while (j >= 0 && l->e[j].prox > x.prox) { /* strictly greater: equal groups don't swap */
            l->e[j + 1] = l->e[j];
            j--;
        }
        l->e[j + 1] = x;
    }
}

/* ------------------------------------------------------------------------- */
/* Answer limits                                                              */
/* ------------------------------------------------------------------------- */
/*
 * Both limits are "at most K answers in any DISC_ANSWER_WINDOW_MS". Keeping
 * the times of the last K answers in a ring makes the test simple: another
 * answer is allowed exactly when the oldest of those K (the one it would
 * replace) is at least a window old.
 */

/* src_reset() -- turn a searcher entry into a fresh one for `addr`. */
static void src_reset(disc_src_t *s, uint32_t addr)
{
    s->addr = addr;
    for (int k = 0; k < DISC_ANSWERS_PER_SRC; k++) {
        s->t[k] = DISC_LONG_AGO;
    }
    s->head = 0;
}

/* disc_limit_init() -- forget every answer. Out: *lim reset. */
void disc_limit_init(disc_limit_t *lim)
{
    for (int i = 0; i < DISC_LIM_SRC_N; i++) {
        src_reset(&lim->src[i], ADDR_NONE); /* ADDR_NONE: free (0 would be the real ID "222") */
    }
    for (int k = 0; k < DISC_ANSWERS_TOTAL; k++) {
        lim->all[k] = DISC_LONG_AGO;
    }
    lim->all_head = 0;
}

/* newest() -- the most recent answer time recorded in a searcher entry. */
static int64_t newest(const disc_src_t *s)
{
    int64_t t = DISC_LONG_AGO;
    for (int k = 0; k < DISC_ANSWERS_PER_SRC; k++) {
        if (s->t[k] > t) {
            t = s->t[k];
        }
    }
    return t;
}

/*
 * src_entry() -- the entry for searcher `addr`: its own if we have one,
 * otherwise a free one, otherwise the one we answered least recently
 * (recycled). Never NULL.
 */
static disc_src_t *src_entry(disc_limit_t *lim, uint32_t addr)
{
    disc_src_t *free_slot = NULL, *oldest = &lim->src[0];
    for (int i = 0; i < DISC_LIM_SRC_N; i++) {
        disc_src_t *s = &lim->src[i];
        if (s->addr == addr) {
            return s;
        }
        if (s->addr == ADDR_NONE && !free_slot) {
            free_slot = s;
        }
        if (newest(s) < newest(oldest)) {
            oldest = s;
        }
    }
    disc_src_t *s = free_slot ? free_slot : oldest;
    src_reset(s, addr);
    return s;
}

/*
 * disc_limit_allow() -- the answering side's gate (see disc.h).
 * In: src, the searcher; now, ms.
 * Returns true when this searcher has had fewer than DISC_ANSWERS_PER_SRC
 * answers, and everyone together fewer than DISC_ANSWERS_TOTAL, in the last
 * DISC_ANSWER_WINDOW_MS. Side effects only when it returns true: the answer
 * is recorded in both rings. (Looking up an unknown searcher may recycle the
 * least recently answered entry; with DISC_LIM_SRC_N entries that only
 * matters to someone cycling through many addresses, and the total limit
 * still holds them.)
 */
bool disc_limit_allow(disc_limit_t *lim, uint32_t src, int64_t now)
{
    disc_src_t *s = src_entry(lim, src);
    bool src_ok = now - s->t[s->head] >= DISC_ANSWER_WINDOW_MS;          /* oldest of its last K answers */
    bool all_ok = now - lim->all[lim->all_head] >= DISC_ANSWER_WINDOW_MS; /* oldest of everyone's last K */
    if (!src_ok || !all_ok) {
        return false;
    }
    s->t[s->head] = now;
    s->head = (uint8_t)((s->head + 1) % DISC_ANSWERS_PER_SRC);
    lim->all[lim->all_head] = now;
    lim->all_head = (uint8_t)((lim->all_head + 1) % DISC_ANSWERS_TOTAL);
    return true;
}
