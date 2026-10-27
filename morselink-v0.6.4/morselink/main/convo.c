/*
 * MorseLink — conversation tracking (see convo.h).
 */
#include "convo.h"

#include "config.h"
#include "morse.h" /* ADDR_NONE */

/* convo_init() -- forget any conversation. */
void convo_init(convo_t *c)
{
    c->addr = ADDR_NONE;
    c->at_ms = 0;
}

/* convo_note() -- remember `addr` as the device we last exchanged something
 * with, and when. Replaces whatever was there: only the latest counts. */
void convo_note(convo_t *c, uint32_t addr, int64_t now_ms)
{
    c->addr = addr;
    c->at_ms = now_ms;
}

/* convo_active() -- same device as the last exchange, and that exchange at
 * most CONVO_QUIET_MS ago ("over 5 minutes" of silence means announce). */
bool convo_active(const convo_t *c, uint32_t addr, int64_t now_ms)
{
    if (addr == ADDR_NONE || c->addr != addr) {
        return false;
    }
    return now_ms - c->at_ms <= CONVO_QUIET_MS;
}
