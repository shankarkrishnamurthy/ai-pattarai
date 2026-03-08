/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Memory statistics — mempool, DPDK heap, TCB, hugepages.
 *
 * All queries are read-only against DPDK APIs and /sys; no new
 * per-worker instrumentation is needed.
 */
#ifndef TGEN_MEM_STATS_H
#define TGEN_MEM_STATS_H

#include <stdint.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Per-worker mempool info ──────────────────────────────────────── */
typedef struct {
    char     name[32];
    uint32_t total;       /* total mbufs in pool */
    uint32_t in_use;      /* mbufs currently allocated */
    uint32_t avail;       /* mbufs available */
} mempool_info_t;

/* ── Per-NUMA-socket DPDK heap info ───────────────────────────────── */
typedef struct {
    int      socket_id;
    uint64_t heap_size;     /* total heap bytes */
    uint64_t alloc_size;    /* allocated bytes */
    uint64_t free_size;     /* free bytes */
} heap_info_t;

/* ── Per-worker TCB store info ────────────────────────────────────── */
typedef struct {
    uint32_t active;        /* in-use connections */
    uint32_t capacity;      /* max connections */
} tcb_info_t;

/* ── Hugepage info ────────────────────────────────────────────────── */
typedef struct {
    uint64_t size_kb;       /* page size in KB (2048 or 1048576) */
    uint64_t total;
    uint64_t free;
    uint64_t in_use;
} hugepage_info_t;

#define MEM_STATS_MAX_SOCKETS  8
#define MEM_STATS_MAX_HPSIZES  4

/* ── Aggregate snapshot ───────────────────────────────────────────── */
typedef struct {
    mempool_info_t   pools[TGEN_MAX_WORKERS];
    uint32_t         n_pools;

    heap_info_t      heaps[MEM_STATS_MAX_SOCKETS];
    uint32_t         n_heaps;

    tcb_info_t       tcbs[TGEN_MAX_WORKERS];
    uint32_t         n_tcbs;

    hugepage_info_t  hugepages[MEM_STATS_MAX_HPSIZES];
    uint32_t         n_hugepages;
} mem_stats_snapshot_t;

/**
 * Populate a memory statistics snapshot.
 * Queries rte_mempool, rte_malloc, TCB stores, and /sys/kernel/mm.
 */
void mem_stats_snapshot(mem_stats_snapshot_t *snap, uint32_t n_workers);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_MEM_STATS_H */
