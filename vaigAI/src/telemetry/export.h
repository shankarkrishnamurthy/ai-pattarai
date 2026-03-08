/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Metrics export — JSON + human-readable text (§6.4).
 */
#ifndef TGEN_EXPORT_H
#define TGEN_EXPORT_H

#include "metrics.h"
#include "cpu_stats.h"
#include "mem_stats.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Render a metrics snapshot as a JSON string into 'buf' (null-terminated).
 * Returns number of bytes written (excluding NUL), or negative on error.
 */
int export_json(const metrics_snapshot_t *snap, char *buf, size_t len);

/**
 * Render a human-readable traffic summary with status, rates, warnings.
 * @param duration_s  test duration in seconds (0 for standalone stats)
 * @param proto       protocol name string (e.g. "http", NULL for standalone)
 */
int export_summary(const metrics_snapshot_t *snap, uint32_t duration_s,
                   const char *proto, char *buf, size_t len);

/**
 * Render CPU stats as human-readable text.
 * @param core  Worker index to filter to, or -1 for all workers.
 */
int export_cpu_text(const cpu_stats_snapshot_t *snap, int core,
                    char *buf, size_t len);

/**
 * Render memory stats as human-readable text.
 * @param core  Worker index to filter to, or -1 for all workers.
 */
int export_mem_text(const mem_stats_snapshot_t *snap, int core,
                    char *buf, size_t len);

/**
 * Render per-port NIC hardware stats as human-readable text.
 */
int export_port_text(char *buf, size_t len);

/**
 * Render a brief one-liner summary of all stat domains.
 */
int export_stat_summary(const cpu_stats_snapshot_t *cpu,
                        const mem_stats_snapshot_t *mem,
                        const metrics_snapshot_t *net,
                        char *buf, size_t len);

/**
 * Render per-worker network stats with TCP connection state for a single core.
 */
int export_net_core_text(const metrics_snapshot_t *snap, uint32_t core,
                         char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_EXPORT_H */
