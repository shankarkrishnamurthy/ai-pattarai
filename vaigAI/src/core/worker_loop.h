/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: worker poll-loop skeleton (§1.7).
 */
#ifndef TGEN_WORKER_LOOP_H
#define TGEN_WORKER_LOOP_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_atomic.h>
#include "../common/types.h"
#include "tx_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Global run flag — cleared by management to stop all workers. */
extern volatile int g_run;
/** Traffic flag — set by /start, cleared by /stop; process stays alive. */
extern volatile int g_traffic;

/** Per-worker context (one per worker lcore). */
typedef struct {
    uint32_t worker_idx;    /* index into g_core_map.worker_lcores[] */
    uint32_t lcore_id;
    uint32_t socket_id;
    /* Assigned ports & queues */
    uint16_t ports[TGEN_MAX_PORTS];
    uint16_t rx_queues[TGEN_MAX_PORTS];
    uint16_t tx_queues[TGEN_MAX_PORTS];
    uint32_t num_ports;
    /* Mempool */
    struct rte_mempool *mempool;
    /* TX generator state */
    tx_gen_state_t tx_gen;
} worker_ctx_t;

/** Array of worker contexts, indexed by worker index. */
extern worker_ctx_t g_worker_ctx[TGEN_MAX_WORKERS];

/** Initialise all worker contexts after port + mempool setup.
 *  Returns 0 on success. */
int tgen_worker_ctx_init(void);

/** Entry point launched on each worker lcore via rte_eal_remote_launch().
 *  arg = (worker_ctx_t *). */
int tgen_worker_loop(void *arg);

/** Management core: signal all workers to stop. */
void tgen_workers_stop(void);

/** Management core: wait until all workers have exited. */
void tgen_workers_join(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_WORKER_LOOP_H */
