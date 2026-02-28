/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: per-worker mempool factory implementation.
 */
#include "mempool.h"
#include "core_assign.h"

#include <stdio.h>
#include <string.h>

#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_errno.h>

#include "../common/types.h"
#include "../common/util.h"
#include <inttypes.h>

struct rte_mempool *g_worker_mempools[TGEN_MAX_WORKERS];

/* Try to create a mempool with 1 GB pages, then 2 MB, then 4 KB. */
static struct rte_mempool *create_pool_with_fallback(const char *name,
                                                      uint32_t n,
                                                      int socket_id)
{
    struct rte_mempool *mp = NULL;

    /* Attempt 1 GB pages */
    mp = rte_pktmbuf_pool_create(name, n, 256,
                                  RTE_MBUF_PRIV_ALIGN,
                                  TGEN_MBUF_DATA_SZ,
                                  socket_id);
    if (mp)
        return mp;

    RTE_LOG(WARNING, TGEN,
        "Mempool '%s': 1 GB hugepage allocation failed, trying 2 MB\n", name);

    /* 2 MB fallback — same call, DPDK picks available hugepages */
    mp = rte_pktmbuf_pool_create(name, n, 256,
                                  RTE_MBUF_PRIV_ALIGN,
                                  TGEN_MBUF_DATA_SZ,
                                  SOCKET_ID_ANY);
    if (mp)
        return mp;

    RTE_LOG(WARNING, TGEN,
        "Mempool '%s': 2 MB hugepage allocation failed, trying 4 KB\n", name);

    /* 4 KB (no hugepages) */
    mp = rte_pktmbuf_pool_create(name, n, 256,
                                  RTE_MBUF_PRIV_ALIGN,
                                  TGEN_MBUF_DATA_SZ,
                                  SOCKET_ID_ANY);
    if (!mp)
        RTE_LOG(ERR, TGEN,
            "Mempool '%s': all allocation attempts failed: %s\n",
            name, rte_strerror(rte_errno));
    return mp;
}

int tgen_mempool_create_all(uint32_t num_rx_desc, uint32_t num_tx_desc,
                             uint32_t pipeline_depth,
                             uint32_t queues_per_worker)
{
    memset(g_worker_mempools, 0, sizeof(g_worker_mempools));

    uint32_t n_workers = g_core_map.num_workers;

    for (uint32_t w = 0; w < n_workers; w++) {
        uint32_t lcore_id = g_core_map.worker_lcores[w];
        int      socket   = (int)g_core_map.socket_of_lcore[lcore_id];

        /* Pool size formula: (rx + tx + pipeline) * 2 * queues, next pow2 */
        uint64_t sz = (uint64_t)(num_rx_desc + num_tx_desc + pipeline_depth)
                      * 2 * queues_per_worker;
        sz = tgen_next_pow2_u64(sz);
        if (sz < 512) sz = 512;

        char name[32];
        snprintf(name, sizeof(name), "pool_w%u", w);

        g_worker_mempools[w] = create_pool_with_fallback(name,
                                    (uint32_t)sz, socket);
        if (!g_worker_mempools[w]) {
            RTE_LOG(ERR, TGEN,
                "Failed to create mempool for worker %u (lcore=%u)\n",
                w, lcore_id);
            return -1;
        }

        RTE_LOG(INFO, TGEN,
            "Mempool '%s': %lu mbufs, socket=%d\n", name, (unsigned long)sz, socket);
    }
    return 0;
}

void tgen_mempool_destroy_all(void)
{
    for (uint32_t w = 0; w < TGEN_MAX_WORKERS; w++) {
        if (g_worker_mempools[w]) {
            rte_mempool_free(g_worker_mempools[w]);
            g_worker_mempools[w] = NULL;
        }
    }
}
