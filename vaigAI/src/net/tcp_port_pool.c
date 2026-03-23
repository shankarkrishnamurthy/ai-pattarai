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

/** Find the next set bit at or after 'start', wrapping once.
 *  Uses __builtin_ctzll for word-granularity scanning (O(words) vs O(bits)). */
static int
bm_find_next(const uint64_t *map, uint32_t start, uint32_t *out)
{
    uint32_t total_words = BITMAP_WORDS;

    /* Start word and bit offset within that word */
    uint32_t w = start >> 6;
    uint32_t b = start & 63;

    /* Phase 1: scan from 'start' to end of bitmap */
    /* First partial word: mask off bits below 'b' */
    uint64_t masked = map[w] & (~0ULL << b);
    if (masked) {
        uint32_t pos = (w << 6) + (uint32_t)__builtin_ctzll(masked);
        if (pos < TGEN_EPHEM_CNT) { *out = pos; return 0; }
    }
    for (uint32_t i = w + 1; i < total_words; i++) {
        if (map[i]) {
            uint32_t pos = (i << 6) + (uint32_t)__builtin_ctzll(map[i]);
            if (pos < TGEN_EPHEM_CNT) { *out = pos; return 0; }
        }
    }

    /* Phase 2: wrap around from beginning to 'start' word */
    for (uint32_t i = 0; i <= w; i++) {
        uint64_t word = map[i];
        if (i == w) word &= ~(~0ULL << b); /* mask off bits at/above 'b' */
        if (word) {
            uint32_t pos = (i << 6) + (uint32_t)__builtin_ctzll(word);
            if (pos < TGEN_EPHEM_CNT) { *out = pos; return 0; }
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
        if (wp == NULL) break; /* safety */
        ip_pool_t *p = &wp->ip_pools[s];
        if (p->src_ip == src_ip)
            return p;
        if (p->src_ip == 0) {
            /* Empty slot: claim it — inherit shared pool's bitmap
             * so RSS queue affinity filtering is preserved. */
            p->src_ip = src_ip;
            memcpy(p->map, wp->shared.map, sizeof(p->map));
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
    /* Mark all ports available */
    memset(wp->shared.map, 0xff, sizeof(wp->shared.map));
    wp->shared.cursor = 0;
    /* Clear per-IP pools so they reinitialize from shared.map
     * (which may have RSS filtering applied after this reset). */
    for (uint32_t s = 0; s < N_IP_SLOTS; s++) {
        wp->ip_pools[s].src_ip = 0;
        memset(wp->ip_pools[s].map, 0, sizeof(wp->ip_pools[s].map));
        wp->ip_pools[s].cursor = 0;
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
tcp_port_free_immediate(uint32_t worker_idx, uint32_t src_ip, uint16_t port)
{
    worker_pool_t *wp = g_pools[worker_idx];
    if (port < TGEN_EPHEM_LO || port >= TGEN_EPHEM_HI)
        return;
    ip_pool_t *ip = ip_pool_get(wp, src_ip);
    bm_set(ip->map, port - TGEN_EPHEM_LO);
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

/* ------------------------------------------------------------------ */
/* RSS queue affinity filter                                            */
/* ------------------------------------------------------------------ */
#include <rte_thash.h>
#include <rte_ethdev.h>

void
tcp_port_pool_apply_rss_filter(uint32_t n_workers,
                               uint32_t src_ip, uint32_t dst_ip,
                               uint16_t dst_port,
                               const uint8_t *rss_key,
                               uint8_t rss_key_len,
                               uint16_t n_rxq)
{
    if (n_rxq <= 1 || n_workers <= 1)
        return;  /* no filtering needed */

    (void)rss_key_len;

    /* Query the actual RETA from port 0 to match NIC behavior exactly */
    uint16_t port_id = 0;
    struct rte_eth_dev_info dev_info;
    int ri = rte_eth_dev_info_get(port_id, &dev_info); (void)ri;
    uint16_t reta_size = dev_info.reta_size;
    if (reta_size == 0) reta_size = 128; /* fallback */

    /* Read the RETA from device */
    uint16_t n_reta_groups = (reta_size + RTE_ETH_RETA_GROUP_SIZE - 1) /
                             RTE_ETH_RETA_GROUP_SIZE;
    struct rte_eth_rss_reta_entry64 reta_conf[8]; /* max 512 entries */
    memset(reta_conf, 0, sizeof(reta_conf));
    for (uint16_t i = 0; i < n_reta_groups && i < 8; i++)
        reta_conf[i].mask = UINT64_MAX;

    int ret = rte_eth_dev_rss_reta_query(port_id, reta_conf, reta_size);
    bool have_reta = (ret == 0);

    RTE_LOG(INFO, TGEN_PP,
            "RSS filter: reta_size=%u, n_rxq=%u, have_reta=%d\n",
            reta_size, n_rxq, have_reta);

    uint32_t ports_per_worker[TGEN_MAX_WORKERS];
    memset(ports_per_worker, 0, sizeof(ports_per_worker));

    for (uint32_t bit = 0; bit < TGEN_EPHEM_CNT; bit++) {
        uint16_t sport = (uint16_t)(TGEN_EPHEM_LO + bit);

        /* rte_softrss() takes host byte order values + original key.
         * src_ip/dst_ip are stored as network byte order (from inet_pton),
         * so convert to host byte order for the hash. */
        uint32_t tuple[3];
        tuple[0] = rte_be_to_cpu_32(src_ip);     /* host byte order */
        tuple[1] = rte_be_to_cpu_32(dst_ip);     /* host byte order */
        tuple[2] = ((uint32_t)sport << 16) | (uint32_t)dst_port;

        uint32_t hash = rte_softrss(tuple, 3, rss_key);

        /* Map hash to queue using RETA (matches NIC behavior) */
        uint16_t reta_idx = hash & (reta_size - 1);
        uint16_t target_q;
        if (have_reta) {
            uint16_t group = reta_idx / RTE_ETH_RETA_GROUP_SIZE;
            uint16_t entry = reta_idx % RTE_ETH_RETA_GROUP_SIZE;
            target_q = reta_conf[group].reta[entry];
        } else {
            target_q = reta_idx % n_rxq;
        }

        if (target_q >= n_workers)
            target_q = target_q % n_workers;

        /* Clear this port from every worker except the target */
        for (uint32_t w = 0; w < n_workers; w++) {
            worker_pool_t *wp = g_pools[w];
            if (!wp) continue;
            if (w == target_q) {
                ports_per_worker[w]++;
                bm_set(wp->shared.map, bit);
            } else {
                bm_clear(wp->shared.map, bit);
            }
            /* Also filter per-IP pools */
            for (uint32_t s = 0; s < N_IP_SLOTS; s++) {
                if (wp->ip_pools[s].src_ip == 0) continue;
                if (w == target_q)
                    bm_set(wp->ip_pools[s].map, bit);
                else
                    bm_clear(wp->ip_pools[s].map, bit);
            }
        }
    }

    for (uint32_t w = 0; w < n_workers; w++) {
        RTE_LOG(INFO, TGEN_PP, "RSS filter: worker %u → %u ports\n",
                w, ports_per_worker[w]);
    }
}
