/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: CPU stats — global storage, snapshot, and reset.
 */
#include "cpu_stats.h"
#include "../common/util.h"
#include <string.h>
#include <rte_cycles.h>

/* Global array — one cache-line-aligned slab per worker. */
cpu_stats_t g_cpu_stats[TGEN_MAX_WORKERS];

void
cpu_stats_snapshot(cpu_stats_snapshot_t *snap, uint32_t n_workers)
{
    memset(snap, 0, sizeof(*snap));
    snap->n_workers = n_workers;
    snap->tsc_hz = g_tsc_hz ? g_tsc_hz : rte_get_tsc_hz();

    for (uint32_t w = 0; w < n_workers && w < TGEN_MAX_WORKERS; w++)
        memcpy(&snap->per_worker[w], &g_cpu_stats[w], sizeof(cpu_stats_t));
}

void
cpu_stats_reset(uint32_t n_workers)
{
    uint64_t now = rte_rdtsc();
    for (uint32_t w = 0; w < n_workers && w < TGEN_MAX_WORKERS; w++) {
        memset(&g_cpu_stats[w], 0, sizeof(cpu_stats_t));
        g_cpu_stats[w].window_start_tsc = now;
    }
}
