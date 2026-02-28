/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: REST API server (§5.4) — libmicrohttpd + jansson.
 *
 * Endpoints:
 *   GET  /api/v1/stats          — JSON metrics snapshot
 *   GET  /api/v1/config         — current config
 *   PUT  /api/v1/config         — replace config (JSON body)
 *   POST /api/v1/start          — start traffic
 *   POST /api/v1/stop           — stop traffic
 *   GET  /api/v1/metrics        — Prometheus exposition format
 */
#ifndef TGEN_REST_H
#define TGEN_REST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the REST server on 'port'.
 * Runs in a background thread (non-blocking call).
 * Returns 0 on success, negative on error.
 */
int rest_server_start(uint16_t port);

/** Stop the REST server and release resources. */
void rest_server_stop(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_REST_H */
