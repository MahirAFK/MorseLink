/*
 * MorseLink — contact table and blocklist (spec §5).
 *
 * Nine slots, numbered 1-9 (one Morse digit each). A slot keeps its number
 * until the contact is forgotten; slots never reorder. Pure C.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PEER_SLOTS 9
#define BLOCK_MAX 16
#define PEERS_VERSION 1

typedef struct {
    uint8_t used;
    uint8_t verified;    /* Phase 2: key comparison done */
    uint8_t reserved[2];
    uint32_t addr;       /* 24-bit wire address (see morse.h) */
    uint8_t shared_key[16]; /* Phase 2; zero in Phase 1 */
} peer_rec_t;

typedef struct {
    uint16_t version;
    uint8_t n_blocked;
    uint8_t reserved;
    peer_rec_t slot[PEER_SLOTS];
    uint32_t blocked[BLOCK_MAX];
} peer_table_t;

void peers_init(peer_table_t *t);
bool peers_valid(const peer_table_t *t);
int peers_count(const peer_table_t *t);
int peers_find(const peer_table_t *t, uint32_t addr);     /* slot index 0-8, or -1 */
uint32_t peers_addr(const peer_table_t *t, int idx);       /* ADDR_NONE if empty */
/* Existing contact keeps its slot; otherwise the lowest free slot. -1 if full. */
int peers_add(peer_table_t *t, uint32_t addr);
bool peers_remove(peer_table_t *t, int idx);

bool peers_is_blocked(const peer_table_t *t, uint32_t addr);
bool peers_block(peer_table_t *t, uint32_t addr);   /* oldest entry drops out when full */
bool peers_unblock(peer_table_t *t, uint32_t addr);

/* Resolve a command argument: a slot digit "1"-"9" or a 3/4-char ID.
 * Returns the address (ADDR_NONE if invalid) and the slot (-1 if not a contact). */
uint32_t peers_resolve(const peer_table_t *t, const char *arg, int *slot_out);
