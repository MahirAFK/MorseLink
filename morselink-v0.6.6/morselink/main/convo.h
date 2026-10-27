/*
 * MorseLink — "are we in a conversation?" (agreed 8 Oct).
 *
 * A received message normally plays attention + the sender's ID + the text.
 * When the two devices have been talking recently, the attention pattern and
 * the ID are only in the way, so the text plays on its own. "Recently" means
 * the last exchange with that same device was at most CONVO_QUIET_MS
 * (5 minutes) ago. An exchange is:
 *   - connecting (S / P accepted, or our A confirmed): so the first message
 *     within 5 minutes of pairing plays bare, but one arriving later, with
 *     nothing said since, gets the full announcement;
 *   - a message we received from them;
 *   - a message of ours they confirmed (its receipt arrived).
 *
 * Only one conversation is remembered: the most recent one. A message from
 * anyone else is announced in full (and becomes the conversation), so the
 * wearer is never left guessing who a bare message came from. After a
 * restart nothing is remembered, so the first message is announced.
 *
 * Plain C, no ESP-IDF: the host tests use it too. Times are milliseconds on
 * any clock that only moves forward (time since boot on the device).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t addr; /* the device of the last exchange; ADDR_NONE if none yet */
    int64_t at_ms; /* when it happened */
} convo_t;

/* convo_init() -- nobody yet (as after a restart). */
void convo_init(convo_t *c);

/* convo_note() -- an exchange with `addr` happened at now_ms. */
void convo_note(convo_t *c, uint32_t addr, int64_t now_ms);

/* convo_active() -- would a message from `addr` arriving at now_ms be part of
 * an active conversation (play the text alone)? True only if the last
 * exchange was with that same device, at most CONVO_QUIET_MS ago. */
bool convo_active(const convo_t *c, uint32_t addr, int64_t now_ms);
