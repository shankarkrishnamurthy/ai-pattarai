/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: management-to-worker IPC implementation.
 */
#include "ipc.h"
#include "core_assign.h"

#include <string.h>
#include <stdio.h>

#include <rte_ring.h>
#include <rte_cycles.h>
#include <rte_log.h>
#include <rte_malloc.h>

#include "../common/types.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"

/* ── Global ring arrays ───────────────────────────────────────────────────── */
struct rte_ring *g_ipc_rings[TGEN_MAX_WORKERS];
struct rte_ring *g_ack_rings[TGEN_MAX_WORKERS];

/* ── Timeout for spin-wait on full ring (§4.3: 100 µs) ───────────────────── */
#define IPC_SPIN_TIMEOUT_US  100ULL

int tgen_ipc_init(uint32_t pipeline_depth)
{
    memset(g_ipc_rings, 0, sizeof(g_ipc_rings));
    memset(g_ack_rings, 0, sizeof(g_ack_rings));

    uint32_t ring_sz = TGEN_MAX((uint32_t)64,
                                (uint32_t)tgen_next_pow2_u64(pipeline_depth * 2));
    if (ring_sz < 64) ring_sz = 64;

    uint32_t n_workers = g_core_map.num_workers;
    for (uint32_t w = 0; w < n_workers; w++) {
        char name[48];

        snprintf(name, sizeof(name), "ipc_ring_w%u", w);
        g_ipc_rings[w] = rte_ring_create(name, ring_sz,
                             (int)g_core_map.socket_of_lcore[
                                     g_core_map.worker_lcores[w]],
                             RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (!g_ipc_rings[w]) {
            RTE_LOG(ERR, TGEN, "Failed to create IPC ring for worker %u\n", w);
            return -1;
        }

        snprintf(name, sizeof(name), "ack_ring_w%u", w);
        g_ack_rings[w] = rte_ring_create(name, ring_sz,
                             (int)g_core_map.socket_of_lcore[
                                     g_core_map.worker_lcores[w]],
                             RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (!g_ack_rings[w]) {
            RTE_LOG(ERR, TGEN, "Failed to create ACK ring for worker %u\n", w);
            return -1;
        }
    }
    RTE_LOG(INFO, TGEN, "IPC rings created: %u workers, ring_sz=%u\n",
            n_workers, ring_sz);
    return 0;
}

void tgen_ipc_destroy(void)
{
    for (uint32_t w = 0; w < TGEN_MAX_WORKERS; w++) {
        if (g_ipc_rings[w]) {
            rte_ring_free(g_ipc_rings[w]);
            g_ipc_rings[w] = NULL;
        }
        if (g_ack_rings[w]) {
            rte_ring_free(g_ack_rings[w]);
            g_ack_rings[w] = NULL;
        }
    }
}

int tgen_ipc_send(uint32_t worker_idx, const config_update_t *msg)
{
    struct rte_ring *ring = g_ipc_rings[worker_idx];
    if (!ring) return -1;

    /* Make a heap copy; rte_ring stores pointers, so we box the message */
    config_update_t *copy = rte_malloc("cfg_update", sizeof(*copy), 8);
    if (!copy) return -1;
    *copy = *msg;

    /* Spin-wait up to IPC_SPIN_TIMEOUT_US µs on a full ring */
    uint64_t t0 = rte_rdtsc();
    uint64_t timeout_tsc = g_tsc_hz ? (g_tsc_hz * IPC_SPIN_TIMEOUT_US / 1000000ULL)
                                     : (IPC_SPIN_TIMEOUT_US * 1000ULL);
    while (rte_ring_enqueue(ring, copy) != 0) {
        if ((rte_rdtsc() - t0) > timeout_tsc) {
            RTE_LOG(WARNING, TGEN,
                "IPC ring full for worker %u — dropping (mgmt_ring_overflow)\n",
                worker_idx);
            rte_free(copy);
            /* Metric increment handled in metrics module */
            return -1;
        }
        rte_pause();
    }
    return 0;
}

uint32_t tgen_ipc_broadcast(const config_update_t *msg)
{
    uint32_t ok = 0;
    for (uint32_t w = 0; w < g_core_map.num_workers; w++) {
        if (tgen_ipc_send(w, msg) == 0)
            ok++;
    }
    return ok;
}

bool tgen_ipc_recv(uint32_t worker_idx, config_update_t *out_msg)
{
    struct rte_ring *ring = g_ipc_rings[worker_idx];
    if (!ring) return false;

    void *ptr = NULL;
    if (rte_ring_dequeue(ring, &ptr) != 0)
        return false;

    *out_msg = *(config_update_t *)ptr;
    rte_free(ptr);
    return true;
}

void tgen_ipc_ack(uint32_t worker_idx, uint32_t seq, int rc)
{
    struct rte_ring *ring = g_ack_rings[worker_idx];
    if (!ring) return;

    ipc_ack_t *ack = rte_malloc("ipc_ack", sizeof(*ack), 8);
    if (!ack) return;
    ack->worker_idx = worker_idx;
    ack->seq        = seq;
    ack->rc         = rc;

    if (rte_ring_enqueue(ring, ack) != 0)
        rte_free(ack); /* drop silently; management will timeout */
}

bool tgen_ipc_collect_ack(uint32_t worker_idx, ipc_ack_t *out_ack)
{
    struct rte_ring *ring = g_ack_rings[worker_idx];
    if (!ring) return false;

    void *ptr = NULL;
    if (rte_ring_dequeue(ring, &ptr) != 0)
        return false;

    *out_ack = *(ipc_ack_t *)ptr;
    rte_free(ptr);
    return true;
}
