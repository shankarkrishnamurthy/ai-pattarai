/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: management-to-worker IPC via SPSC rte_ring (§1.8, §4.3).
 */
#ifndef TGEN_IPC_H
#define TGEN_IPC_H

#include <stdint.h>
#include <stdbool.h>

#include <rte_ring.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Config-update message (§4.3 — 256-byte fixed-size struct) ────────────── */
typedef enum {
    CFG_CMD_NOOP        = 0,
    CFG_CMD_SET_PROFILE,
    CFG_CMD_START,
    CFG_CMD_STOP,
    CFG_CMD_SET_RATE,
    CFG_CMD_SHUTDOWN,
} cfg_cmd_t;

typedef struct __rte_aligned(8) {
    cfg_cmd_t cmd;            /* 4 bytes */
    uint32_t  seq;            /* sequence counter for ACK matching */
    uint8_t   payload[248];   /* command-specific data */
} config_update_t;

_Static_assert(sizeof(config_update_t) == 256, "config_update_t must be exactly 256 bytes");

/* ── ACK message (worker → management) ───────────────────────────────────── */
typedef struct {
    uint32_t worker_idx;
    uint32_t seq;
    int      rc;              /* 0 = OK, negative = error */
} ipc_ack_t;

/* ── Per-worker IPC rings ─────────────────────────────────────────────────── */
extern struct rte_ring *g_ipc_rings[TGEN_MAX_WORKERS];   /* mgmt→worker */
extern struct rte_ring *g_ack_rings[TGEN_MAX_WORKERS];   /* worker→mgmt */

/** Create all IPC rings.  pipeline_depth used to size them.
 *  Returns 0 on success, -1 on error. */
int tgen_ipc_init(uint32_t pipeline_depth);

/** Destroy all IPC rings. */
void tgen_ipc_destroy(void);

/** Management core: send ConfigUpdate to a specific worker.
 *  Spin-waits up to 100 µs on full ring; increments mgmt_ring_overflow and
 *  drops on timeout.
 *  Returns 0 on success, -1 on drop. */
int tgen_ipc_send(uint32_t worker_idx, const config_update_t *msg);

/** Management core: broadcast ConfigUpdate to all workers.
 *  Returns number of successful sends. */
uint32_t tgen_ipc_broadcast(const config_update_t *msg);

/** Worker core: try to dequeue one ConfigUpdate (non-blocking).
 *  Returns true if a message was dequeued. */
bool tgen_ipc_recv(uint32_t worker_idx, config_update_t *out_msg);

/** Worker core: send ACK back to management. */
void tgen_ipc_ack(uint32_t worker_idx, uint32_t seq, int rc);

/** Management core: drain ACK ring to collect worker acknowledgements. */
bool tgen_ipc_collect_ack(uint32_t worker_idx, ipc_ack_t *out_ack);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_IPC_H */
