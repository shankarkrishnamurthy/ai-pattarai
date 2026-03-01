/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP TCB store — per-lcore open-addressing hash table.
 */
#include "tcp_tcb.h"
#include "../core/core_assign.h"
#include "../common/util.h"

#include <string.h>
#include <rte_malloc.h>
#include <rte_log.h>

tcb_store_t g_tcb_stores[TGEN_MAX_WORKERS];

/* ── 4-tuple hash ─────────────────────────────────────────────────────────── */
static inline uint32_t tuple_hash(uint32_t s_ip, uint16_t s_port,
                                   uint32_t d_ip, uint16_t d_port)
{
    uint64_t k = ((uint64_t)s_ip << 32) ^ (uint64_t)d_ip ^
                 ((uint64_t)s_port << 16) ^ d_port;
    /* Murmurhash finaliser mix */
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

/* ── Initialise a single store ───────────────────────────────────────────── */
int tcb_store_init(tcb_store_t *store, uint32_t capacity, int socket_id)
{
    memset(store, 0, sizeof(*store));

    /* TCBs: pre-allocated flat array */
    store->tcbs = rte_zmalloc_socket("tcb_array",
                      sizeof(tcb_t) * capacity, CACHE_LINE_SIZE, socket_id);
    if (!store->tcbs) {
        RTE_LOG(ERR, TCP, "TCB: failed to allocate %u TCBs\n", capacity);
        return -1;
    }

    /* Hash table: next power of 2 >= 2*capacity for load factor ~0.5 */
    store->ht_size = (uint32_t)tgen_next_pow2_u64((uint64_t)capacity * 2);
    store->ht_mask = store->ht_size - 1;
    store->ht = rte_zmalloc_socket("tcb_ht",
                    sizeof(int32_t) * store->ht_size, CACHE_LINE_SIZE, socket_id);
    if (!store->ht) {
        RTE_LOG(ERR, TCP, "TCB: failed to allocate HT (%u slots)\n",
                store->ht_size);
        rte_free(store->tcbs);
        return -1;
    }
    memset(store->ht, -1, sizeof(int32_t) * store->ht_size);

    store->capacity = capacity;
    store->count    = 0;
    return 0;
}

/* ── Alloc ────────────────────────────────────────────────────────────────── */
tcb_t *tcb_alloc(tcb_store_t *store,
                  uint32_t s_ip, uint16_t s_port,
                  uint32_t d_ip, uint16_t d_port)
{
    if (store->count >= store->capacity) return NULL;

    /* Find free TCB slot (linear scan — pool should have free slots) */
    tcb_t *tcb = NULL;
    uint32_t idx = 0;
    for (uint32_t i = 0; i < store->capacity; i++) {
        if (!store->tcbs[i].in_use) {
            tcb  = &store->tcbs[i];
            idx  = i;
            break;
        }
    }
    if (!tcb) return NULL;

    memset(tcb, 0, sizeof(*tcb));
    tcb->src_ip   = s_ip;
    tcb->src_port = s_port;
    tcb->dst_ip   = d_ip;
    tcb->dst_port = d_port;
    tcb->in_use   = true;

    /* Insert into hash table (linear probing) */
    uint32_t h = tuple_hash(s_ip, s_port, d_ip, d_port) & store->ht_mask;
    for (uint32_t k = 0; k < store->ht_size; k++) {
        uint32_t slot = (h + k) & store->ht_mask;
        if (store->ht[slot] == -1) {
            store->ht[slot] = (int32_t)idx;
            break;
        }
    }
    store->count++;
    return tcb;
}

/* ── Lookup ───────────────────────────────────────────────────────────────── */
tcb_t *tcb_lookup(tcb_store_t *store,
                   uint32_t s_ip, uint16_t s_port,
                   uint32_t d_ip, uint16_t d_port)
{
    uint32_t h = tuple_hash(s_ip, s_port, d_ip, d_port) & store->ht_mask;
    for (uint32_t k = 0; k < store->ht_size; k++) {
        uint32_t slot = (h + k) & store->ht_mask;
        int32_t  idx  = store->ht[slot];
        if (idx == -1) return NULL; /* empty slot → not found */
        if (idx < 0)   continue;   /* tombstone */
        tcb_t *t = &store->tcbs[(uint32_t)idx];
        if (t->in_use &&
            t->src_ip == s_ip && t->src_port == s_port &&
            t->dst_ip == d_ip && t->dst_port == d_port)
            return t;
    }
    return NULL;
}

/* ── Free ─────────────────────────────────────────────────────────────────── */
void tcb_free(tcb_store_t *store, tcb_t *tcb)
{
    if (!tcb || !tcb->in_use) return;

    uint32_t s_ip   = tcb->src_ip,  d_ip   = tcb->dst_ip;
    uint16_t s_port = tcb->src_port, d_port = tcb->dst_port;

    memset(tcb, 0, sizeof(*tcb));
    store->count--;

    /* Remove from hash table (mark as tombstone = -2) */
    uint32_t h = tuple_hash(s_ip, s_port, d_ip, d_port) & store->ht_mask;
    uint32_t idx_in_array = (uint32_t)(tcb - store->tcbs);
    for (uint32_t k = 0; k < store->ht_size; k++) {
        uint32_t slot = (h + k) & store->ht_mask;
        if (store->ht[slot] == (int32_t)idx_in_array) {
            store->ht[slot] = -2; /* tombstone */
            return;
        }
        if (store->ht[slot] == -1) return; /* not found */
    }
}

/* ── Reset all TCBs ───────────────────────────────────────────────────────── */
void tcb_store_reset(tcb_store_t *store)
{
    if (!store->tcbs) return;
    for (uint32_t i = 0; i < store->capacity; i++)
        memset(&store->tcbs[i], 0, sizeof(store->tcbs[i]));
    store->count = 0;
    /* Clear hash table */
    for (uint32_t i = 0; i < store->ht_size; i++)
        store->ht[i] = -1;
}

/* ── Init all workers ─────────────────────────────────────────────────────── */
int tcb_stores_init(uint32_t max_connections_per_core)
{
    uint32_t n = g_core_map.num_workers;
    for (uint32_t w = 0; w < n; w++) {
        int socket = (int)g_core_map.socket_of_lcore[g_core_map.worker_lcores[w]];
        if (tcb_store_init(&g_tcb_stores[w], max_connections_per_core, socket) < 0)
            return -1;
        RTE_LOG(INFO, TCP,
            "TCB store[%u]: %u slots, socket=%d\n",
            w, max_connections_per_core, socket);
    }
    return 0;
}

void tcb_stores_destroy(void)
{
    for (uint32_t w = 0; w < TGEN_MAX_WORKERS; w++) {
        if (g_tcb_stores[w].tcbs) {
            rte_free(g_tcb_stores[w].tcbs);
            g_tcb_stores[w].tcbs = NULL;
        }
        if (g_tcb_stores[w].ht) {
            rte_free(g_tcb_stores[w].ht);
            g_tcb_stores[w].ht = NULL;
        }
    }
}
