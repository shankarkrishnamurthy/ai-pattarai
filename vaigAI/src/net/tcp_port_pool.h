/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Per-lcore ephemeral TCP port pool (§3.3).
 */
#ifndef TGEN_TCP_PORT_POOL_H
#define TGEN_TCP_PORT_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Ephemeral port range [EPHEM_LO, EPHEM_HI). */
#define TGEN_EPHEM_LO  10000u
#define TGEN_EPHEM_HI  60000u
#define TGEN_EPHEM_CNT (TGEN_EPHEM_HI - TGEN_EPHEM_LO)

/**
 * Initialise port pools for `n_workers` workers.
 * Must be called from management thread before lcores start.
 */
int tcp_port_pool_init(uint32_t n_workers);

/** Release all resources. */
void tcp_port_pool_fini(void);

/**
 * Allocate an ephemeral port for (worker_idx, src_ip).
 * Returns 0 on success, negative on exhaustion.
 * *port is in host byte order.
 */
int tcp_port_alloc(uint32_t worker_idx, uint32_t src_ip, uint16_t *port);

/**
 * Release a port previously allocated with tcp_port_alloc().
 * The port is not immediately reusable — it enters a TIME_WAIT
 * hold-off that expires after TGEN_TCP_TIMEWAIT_MS milliseconds.
 */
void tcp_port_free(uint32_t worker_idx, uint32_t src_ip, uint16_t port);

/**
 * Per-worker tick: release ports whose TIME_WAIT hold-off has expired.
 * Called from the worker poll loop on each timer tick (1 ms).
 */
void tcp_port_pool_tick(uint32_t worker_idx, uint64_t now_tsc);

/**
 * Reset all port allocations for a worker (free everything immediately).
 */
void tcp_port_pool_reset(uint32_t worker_idx);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TCP_PORT_POOL_H */
