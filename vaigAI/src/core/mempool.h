/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: per-worker mempool factory (§1.2).
 */
#ifndef TGEN_MEMPOOL_H
#define TGEN_MEMPOOL_H

#include <rte_mempool.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Per-worker mempool handle array (indexed by worker index). */
extern struct rte_mempool *g_worker_mempools[TGEN_MAX_WORKERS];

/** Create per-worker mempools.
 *  @param num_rx_desc  RX descriptors per queue
 *  @param num_tx_desc  TX descriptors per queue
 *  @param pipeline_depth  Pipeline inflight budget
 *  @param queues_per_worker  Number of queue pairs per worker
 *  Returns 0 on success, -1 on error. */
int tgen_mempool_create_all(uint32_t num_rx_desc, uint32_t num_tx_desc,
                            uint32_t pipeline_depth,
                            uint32_t queues_per_worker);

/** Destroy all mempools at shutdown. */
void tgen_mempool_destroy_all(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_MEMPOOL_H */
