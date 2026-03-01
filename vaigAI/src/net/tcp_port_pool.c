/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Per-lcore ephemeral TCP port pool.
 *
 * Design
 * ------
 * Each worker has a flat bitmap of TGEN_EPHEM_CNT bits (one per port
 * in [10000, 60000)).  Allocation is a simple linear scan starting at
 * a per-worker cursor, so the amortised cost is O(1) under normal load.
 *
 * Per-IP independence (§3.3)
 * --------------------------
 * The spec calls for per-IP port pools. We implement this with a small
 * open-addressing hash table (64 slots per worker).  Each slot stores a
 * separate bitmap + cursor for one src_ip.  If the table is full we fall
 * back to the shared pool (harmless for correctness).
 *
 * TIME_WAIT hold-off
 * ------------------
 * Freed ports are pushed onto a ring.  tcp_port_pool_tick() moves them
 * to available once TGEN_TCP_TIMEWAIT_MS has elapsed.
 */

#include "tcp_port_pool.h"
#include "../common/util.h"
#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_atomic.h>
#include <string.h>

#define RTE_LOGTYPE_TGEN_PP RTE_LOGTYPE_USER4

/* TIME_WAIT hold-off in milliseconds (default 2× MSL = 4 s). */
#ifndef TGEN_TCP_TIMEWAIT_MS
# define TGEN_TCP_TIMEWAIT_MS 4000u
#endif

/* ------------------------------------------------------------------ */
/* Per-IP slot                                                           */
/* ------------------------------------------------------------------ */
#define BITMAP_WORDS ((TGEN_EPHEM_CNT + 63) / 64)

typedef struct {
    uint32_t  src_ip;           /* 0 → slot free */
    uint32_t  cursor;           /* next scan position */
    uint64_t  map[BITMAP_WORDS];/* 1 = available */
} ip_pool_t;

/* ------------------------------------------------------------------ */
/* TIME_WAIT ring entry                                                  */
/* ------------------------------------------------------------------ */
#define TW_RING_SIZE 65536u     /* must be power of 2 */
#define TW_RING_MASK (TW_RING_SIZE - 1u)

typedef struct {
    uint32_t  src_ip;
    uint16_t  port;             /* host byte order */
    uint64_t  release_tsc;      /* TSC at which to re-enable */
} tw_entry_t;

/* ------------------------------------------------------------------ */
/* Per-worker state                                                     */
/* ------------------------------------------------------------------ */
#define N_IP_SLOTS 64u

typedef struct {
    ip_pool_t  ip_pools[N_IP_SLOTS];
    ip_pool_t  shared;              /* fallback when ip_pools full */

    /* TIME_WAIT ring (SPSC — same lcore writes & reads) */
    tw_entry_t tw_ring[TW_RING_SIZE];
    uint32_t   tw_head;
    uint32_t   tw_tail;

    uint64_t   tw_hold_tsc;         /* TGEN_TCP_TIMEWAIT_MS in TSC cycles */

    /* stat */
    uint64_t   port_exhaustion_events;
} worker_pool_t;

/* ------------------------------------------------------------------ */
/* Global array                                                         */
/* ------------------------------------------------------------------ */
static worker_pool_t *g_pools[TGEN_MAX_WORKERS];

/* ------------------------------------------------------------------ */
/* Helpers — bitmap                                                     */
/* ------------------------------------------------------------------ */
static inline void
bm_set(uint64_t *map, uint32_t bit)
{
    map[bit >> 6] |=  (1ull << (bit & 63));
}

static inline void
bm_clear(uint64_t *map, uint32_t bit)
{
    map[bit >> 6] &= ~(1ull << (bit & 63));
}

static inline int
bm_test(const uint64_t *map, uint32_t bit)
{
    return (map[bit >> 6] >> (bit & 63)) & 1u;
}

/** Find the next set bit at or after 'start', wrapping once. */
static int
bm_find_next(const uint64_t *map, uint32_t start, uint32_t *out)
{
    for (uint32_t i = 0; i < TGEN_EPHEM_CNT; i++) {
        uint32_t pos = (start + i) % TGEN_EPHEM_CNT;
        if (bm_test(map, pos)) {
            *out = pos;
            return 0;
        }
    }
    return -1; /* exhausted */
}

/* ------------------------------------------------------------------ */
/* Helpers — per-IP slot lookup                                         */
/* ------------------------------------------------------------------ */
static ip_pool_t *
ip_pool_get(worker_pool_t *wp, uint32_t src_ip)
{
    if (src_ip == 0)
        return &wp->shared;

    /* FNV-1a scramble → slot */
    uint32_t h = src_ip ^ 0x811c9dc5u;
    h = (h ^ (h >> 16)) * 0x45d9f3bu;
    h ^= (h >> 16);
    uint32_t start = h % N_IP_SLOTS;

    for (uint32_t i = 0; i < N_IP_SLOTS; i++) {
        uint32_t s = (start + i) % N_IP_SLOTS;
        if (g_pools[0] == NULL) break; /* safety */
        ip_pool_t *p = &wp->ip_pools[s];
        if (p->src_ip == src_ip)
            return p;
        if (p->src_ip == 0) {
            /* Empty slot: claim it */
            p->src_ip = src_ip;
            memset(p->map, 0xff, sizeof(p->map));
            p->cursor = 0;
            return p;
        }
    }
    /* Table full — use shared pool */
    return &wp->shared;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
int
tcp_port_pool_init(uint32_t n_workers)
{
    extern uint64_t g_tsc_hz;

    if (n_workers == 0 || n_workers > TGEN_MAX_WORKERS)
        n_workers = TGEN_MAX_WORKERS;

    for (uint32_t w = 0; w < n_workers; w++) {
        worker_pool_t *wp = rte_zmalloc_socket("port_pool",
                sizeof(worker_pool_t), RTE_CACHE_LINE_SIZE, SOCKET_ID_ANY);
        if (!wp) {
            RTE_LOG(ERR, TGEN_PP, "OOM worker port pool %u\n", w);
            return -ENOMEM;
        }
        /* Mark all ports available in each IP slot */
        memset(wp->shared.map, 0xff, sizeof(wp->shared.map));
        for (uint32_t s = 0; s < N_IP_SLOTS; s++)
            memset(wp->ip_pools[s].map, 0xff, sizeof(wp->ip_pools[s].map));

        wp->tw_hold_tsc = (uint64_t)TGEN_TCP_TIMEWAIT_MS * g_tsc_hz / 1000u;
        g_pools[w] = wp;
    }
    return 0;
}

void
tcp_port_pool_fini(void)
{
    for (uint32_t w = 0; w < TGEN_MAX_WORKERS; w++) {
        rte_free(g_pools[w]);
        g_pools[w] = NULL;
    }
}

void
tcp_port_pool_reset(uint32_t worker_idx)
{
    worker_pool_t *wp = g_pools[worker_idx];
    if (!wp) return;
    /* Mark all ports available but preserve cursor to avoid port reuse */
    memset(wp->shared.map, 0xff, sizeof(wp->shared.map));
    /* Keep wp->shared.cursor and src_ip as-is */
    for (uint32_t s = 0; s < N_IP_SLOTS; s++) {
        memset(wp->ip_pools[s].map, 0xff, sizeof(wp->ip_pools[s].map));
        /* Keep ip_pools[s].cursor and src_ip as-is */
    }
    /* Drain TIME_WAIT ring */
    wp->tw_head = 0;
    wp->tw_tail = 0;
}

int
tcp_port_alloc(uint32_t worker_idx, uint32_t src_ip, uint16_t *port)
{
    worker_pool_t *wp = g_pools[worker_idx];
    ip_pool_t     *ip = ip_pool_get(wp, src_ip);
    uint32_t       bit;

    if (bm_find_next(ip->map, ip->cursor, &bit) < 0) {
        wp->port_exhaustion_events++;
        RTE_LOG(WARNING, TGEN_PP,
                "Port exhaustion worker=%u src_ip=%u\n", worker_idx, src_ip);
        return -ENOBUFS;
    }

    bm_clear(ip->map, bit);
    ip->cursor = (bit + 1) % TGEN_EPHEM_CNT;
    *port = (uint16_t)(TGEN_EPHEM_LO + bit);
    return 0;
}

void
tcp_port_free(uint32_t worker_idx, uint32_t src_ip, uint16_t port)
{
    extern uint64_t g_tsc_hz;
    worker_pool_t *wp = g_pools[worker_idx];

    if (port < TGEN_EPHEM_LO || port >= TGEN_EPHEM_HI)
        return;

    /* Push to TIME_WAIT ring */
    uint32_t next_tail = (wp->tw_tail + 1) & TW_RING_MASK;
    if (next_tail == wp->tw_head) {
        /* Ring full — release immediately (unusual under normal load) */
        ip_pool_t *ip = ip_pool_get(wp, src_ip);
        bm_set(ip->map, port - TGEN_EPHEM_LO);
        return;
    }

    tw_entry_t *e = &wp->tw_ring[wp->tw_tail];
    e->src_ip      = src_ip;
    e->port        = port;
    e->release_tsc = rte_rdtsc() + wp->tw_hold_tsc;
    wp->tw_tail    = next_tail;
}

void
tcp_port_pool_tick(uint32_t worker_idx, uint64_t now_tsc)
{
    worker_pool_t *wp = g_pools[worker_idx];

    while (wp->tw_head != wp->tw_tail) {
        tw_entry_t *e = &wp->tw_ring[wp->tw_head];
        if (now_tsc < e->release_tsc)
            break; /* ring is FIFO — rest still in hold-off */
        ip_pool_t *ip = ip_pool_get(wp, e->src_ip);
        bm_set(ip->map, e->port - TGEN_EPHEM_LO);
        wp->tw_head = (wp->tw_head + 1) & TW_RING_MASK;
    }
}
