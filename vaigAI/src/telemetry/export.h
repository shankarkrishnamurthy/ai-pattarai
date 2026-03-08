/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Metrics export — JSON (§6.4).
 */
#ifndef TGEN_EXPORT_H
#define TGEN_EXPORT_H

#include "metrics.h"
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
#ifdef __cplusplus
}
#endif
#endif /* TGEN_EXPORT_H */
