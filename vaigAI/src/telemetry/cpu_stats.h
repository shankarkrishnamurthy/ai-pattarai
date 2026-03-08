/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Per-worker CPU cycle accounting for DPDK poll loops.
 *
 * Each worker accumulates TSC cycles spent in each phase of the
 * poll loop (IPC, RX, TX, timer) plus idle polls.  The management
 * thread reads these to compute per-core CPU utilisation.
 */
#ifndef TGEN_CPU_STATS_H
#define TGEN_CPU_STATS_H

#include <stdint.h>
#include <rte_common.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Per-worker cycle counters (single-writer per slab, no atomics)      */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t cycles_rx;         /* RX burst + classify + reply TX */
    uint64_t cycles_tx;         /* TX generation burst */
    uint64_t cycles_timer;      /* TCP timer + port pool tick */
    uint64_t cycles_ipc;        /* IPC command drain */
    uint64_t cycles_idle;       /* iterations with no work */
    uint64_t cycles_total;      /* wall-clock cycles since reset */
    uint64_t window_start_tsc;  /* TSC at last reset */
    uint64_t loop_count;        /* total loop iterations */
} __rte_cache_aligned cpu_stats_t;

/* Global array — one slab per worker. */
extern cpu_stats_t g_cpu_stats[TGEN_MAX_WORKERS];

/* ------------------------------------------------------------------ */
/* Snapshot (used by management/export thread)                         */
/* ------------------------------------------------------------------ */
typedef struct {
    cpu_stats_t per_worker[TGEN_MAX_WORKERS];
    uint32_t    n_workers;
    uint64_t    tsc_hz;
} cpu_stats_snapshot_t;

/**
 * Snapshot all worker CPU stats into 'snap'.
 * Reads are racy (no lock) — tolerable for monitoring.
 */
void cpu_stats_snapshot(cpu_stats_snapshot_t *snap, uint32_t n_workers);

/**
 * Reset all worker CPU stats to zero and record current TSC
 * as the new window start.
 */
void cpu_stats_reset(uint32_t n_workers);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_CPU_STATS_H */
