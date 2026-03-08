/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Memory statistics snapshot implementation.
 */
#include "mem_stats.h"
#include "../core/mempool.h"
#include "../core/core_assign.h"
#include "../net/tcp_tcb.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_lcore.h>

/* ── Hugepage query from sysfs ────────────────────────────────────── */
static uint64_t
read_sysfs_u64(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    uint64_t v = 0;
    if (fscanf(f, "%" PRIu64, &v) != 1)
        v = 0;
    fclose(f);
    return v;
}

static void
query_hugepages(mem_stats_snapshot_t *snap)
{
    static const struct { const char *dir; uint64_t size_kb; } hp_sizes[] = {
        { "/sys/kernel/mm/hugepages/hugepages-2048kB",    2048 },
        { "/sys/kernel/mm/hugepages/hugepages-1048576kB", 1048576 },
    };

    snap->n_hugepages = 0;
    for (uint32_t i = 0; i < TGEN_ARRAY_SIZE(hp_sizes) &&
                          snap->n_hugepages < MEM_STATS_MAX_HPSIZES; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/nr_hugepages", hp_sizes[i].dir);
        uint64_t total = read_sysfs_u64(path);
        snprintf(path, sizeof(path), "%s/free_hugepages", hp_sizes[i].dir);
        uint64_t free_hp = read_sysfs_u64(path);

        hugepage_info_t *hp = &snap->hugepages[snap->n_hugepages++];
        hp->size_kb = hp_sizes[i].size_kb;
        hp->total   = total;
        hp->free    = free_hp;
        hp->in_use  = (total > free_hp) ? total - free_hp : 0;
    }
}

/* ── Main snapshot ────────────────────────────────────────────────── */
void
mem_stats_snapshot(mem_stats_snapshot_t *snap, uint32_t n_workers)
{
    memset(snap, 0, sizeof(*snap));

    /* ── Packet buffer mempools ────────────────────────────────────── */
    snap->n_pools = 0;
    for (uint32_t w = 0; w < n_workers && w < TGEN_MAX_WORKERS; w++) {
        struct rte_mempool *mp = g_worker_mempools[w];
        if (!mp) continue;

        mempool_info_t *pi = &snap->pools[snap->n_pools++];
        snprintf(pi->name, sizeof(pi->name), "pool_w%u", w);
        pi->avail  = rte_mempool_avail_count(mp);
        pi->in_use = rte_mempool_in_use_count(mp);
        pi->total  = pi->avail + pi->in_use;
    }

    /* ── DPDK heap per NUMA socket ─────────────────────────────────── */
    snap->n_heaps = 0;
    for (int s = 0; s < MEM_STATS_MAX_SOCKETS; s++) {
        struct rte_malloc_socket_stats mstats;
        if (rte_malloc_get_socket_stats(s, &mstats) < 0)
            continue;
        if (mstats.heap_totalsz_bytes == 0)
            continue;

        heap_info_t *hi = &snap->heaps[snap->n_heaps++];
        hi->socket_id  = s;
        hi->heap_size  = mstats.heap_totalsz_bytes;
        hi->alloc_size = mstats.heap_allocsz_bytes;
        hi->free_size  = mstats.heap_freesz_bytes;
    }

    /* ── TCB store occupancy ───────────────────────────────────────── */
    snap->n_tcbs = 0;
    for (uint32_t w = 0; w < n_workers && w < TGEN_MAX_WORKERS; w++) {
        tcb_info_t *ti = &snap->tcbs[snap->n_tcbs++];
        ti->active   = g_tcb_stores[w].count;
        ti->capacity = g_tcb_stores[w].capacity;
    }

    /* ── Hugepages ─────────────────────────────────────────────────── */
    query_hugepages(snap);
}
