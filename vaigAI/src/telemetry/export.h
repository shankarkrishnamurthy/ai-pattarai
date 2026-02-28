/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Metrics export — JSON, Prometheus text, gRPC stream (§6.4).
 */
#ifndef TGEN_EXPORT_H
#define TGEN_EXPORT_H

#include "metrics.h"
#include "histogram.h"
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
 * Render a metrics snapshot as Prometheus exposition format into 'buf'.
 * Returns number of bytes written (excluding NUL), or negative on error.
 */
int export_prometheus(const metrics_snapshot_t *snap, char *buf, size_t len);

/**
 * Append latency histogram percentiles (p50/p90/p99/p999) to 'buf'
 * in Prometheus format.
 */
int export_histogram_prometheus(const histogram_t *h,
                                const char *metric_name,
                                char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_EXPORT_H */
