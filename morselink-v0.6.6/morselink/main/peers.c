/*
 * MorseLink — contact table and blocklist (see peers.h).
 */
#include "peers.h"

#include <string.h>

#include "morse.h"

void peers_init(peer_table_t *t)
{
    memset(t, 0, sizeof(*t));
    t->version = PEERS_VERSION;
}

bool peers_valid(const peer_table_t *t)
{
    if (t->version != PEERS_VERSION || t->n_blocked > BLOCK_MAX) {
        return false;
    }
    for (int i = 0; i < PEER_SLOTS; i++) {
        if (t->slot[i].used && t->slot[i].addr >= ADDR_NONE) {
            return false;
        }
    }
    return true;
}

int peers_count(const peer_table_t *t)
{
    int n = 0;
    for (int i = 0; i < PEER_SLOTS; i++) {
        n += t->slot[i].used ? 1 : 0;
    }
    return n;
}

int peers_find(const peer_table_t *t, uint32_t addr)
{
    for (int i = 0; i < PEER_SLOTS; i++) {
        if (t->slot[i].used && t->slot[i].addr == addr) {
            return i;
        }
    }
    return -1;
}

uint32_t peers_addr(const peer_table_t *t, int idx)
{
    if (idx < 0 || idx >= PEER_SLOTS || !t->slot[idx].used) {
        return ADDR_NONE;
    }
    return t->slot[idx].addr;
}

int peers_add(peer_table_t *t, uint32_t addr)
{
    int i = peers_find(t, addr);
    if (i >= 0) {
        return i;
    }
    for (i = 0; i < PEER_SLOTS; i++) {
        if (!t->slot[i].used) {
            memset(&t->slot[i], 0, sizeof(t->slot[i]));
            t->slot[i].used = 1;
            t->slot[i].addr = addr;
            return i;
        }
    }
    return -1;
}

bool peers_remove(peer_table_t *t, int idx)
{
    if (idx < 0 || idx >= PEER_SLOTS || !t->slot[idx].used) {
        return false;
    }
    memset(&t->slot[idx], 0, sizeof(t->slot[idx]));
    return true;
}

bool peers_is_blocked(const peer_table_t *t, uint32_t addr)
{
    for (int i = 0; i < t->n_blocked; i++) {
        if (t->blocked[i] == addr) {
            return true;
        }
    }
    return false;
}

bool peers_block(peer_table_t *t, uint32_t addr)
{
    if (peers_is_blocked(t, addr)) {
        return true;
    }
    if (t->n_blocked == BLOCK_MAX) {
        memmove(t->blocked, t->blocked + 1, (BLOCK_MAX - 1) * sizeof(t->blocked[0]));
        t->n_blocked--;
    }
    t->blocked[t->n_blocked++] = addr;
    return true;
}

bool peers_unblock(peer_table_t *t, uint32_t addr)
{
    for (int i = 0; i < t->n_blocked; i++) {
        if (t->blocked[i] == addr) {
            memmove(&t->blocked[i], &t->blocked[i + 1], (size_t)(t->n_blocked - i - 1) * sizeof(t->blocked[0]));
            t->n_blocked--;
            return true;
        }
    }
    return false;
}

uint32_t peers_resolve(const peer_table_t *t, const char *arg, int *slot_out)
{
    *slot_out = -1;
    if (!arg || !*arg) {
        return ADDR_NONE;
    }
    if (arg[0] >= '1' && arg[0] <= '9' && arg[1] == '\0') {
        int idx = arg[0] - '1';
        uint32_t a = peers_addr(t, idx);
        if (a != ADDR_NONE) {
            *slot_out = idx;
        }
        return a;
    }
    uint32_t a = id_to_addr(arg);
    if (a != ADDR_NONE) {
        *slot_out = peers_find(t, a);
    }
    return a;
}
