/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: DPDK-native packet capture via rte_pcapng + eth callbacks.
 */
#include "pktrace.h"
#include "log.h"

#include <rte_ethdev.h>
#include <rte_pcapng.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_errno.h>

#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

/* ─── tunables ──────────────────────────────────────────────────────────── */
#define PKTRACE_RING_SZ   4096   /* power-of-2; ring slots                  */
#define PKTRACE_POOL_SZ   4096   /* pcapng-clone mempool elements            */
#define PKTRACE_SNAP_LEN  1600   /* bytes captured per packet                */
#define PKTRACE_BATCH      64    /* write batch size                         */

/* ─── module state ──────────────────────────────────────────────────────── */
static struct rte_ring                    *g_ring;
static struct rte_mempool                 *g_mp;
static const struct rte_eth_rxtx_callback *g_rx_cb;
static const struct rte_eth_rxtx_callback *g_tx_cb;
static uint16_t                            g_port;
static uint16_t                            g_queue;
static uint32_t                            g_max_pkts;
static atomic_uint                         g_active;
static atomic_uint                         g_captured;
static atomic_uint                         g_dropped;   /* ring-full drops  */

/* ─── RX callback (runs on worker lcore) ───────────────────────────────── */
static uint16_t
pktrace_rx_cb(uint16_t port, uint16_t queue,
              struct rte_mbuf **pkts, uint16_t nb_pkts,
              uint16_t max_pkts_arg, void *user_param)
{
    (void)max_pkts_arg; (void)user_param;

    if (!atomic_load_explicit(&g_active, memory_order_relaxed))
        return nb_pkts;

    for (uint16_t i = 0; i < nb_pkts; i++) {
        uint32_t n = atomic_fetch_add_explicit(&g_captured, 1,
                                               memory_order_relaxed);
        if (g_max_pkts && n >= g_max_pkts) {
            atomic_store_explicit(&g_active, 0, memory_order_relaxed);
            atomic_fetch_sub_explicit(&g_captured, 1, memory_order_relaxed);
            break;
        }
        struct rte_mbuf *clone =
            rte_pcapng_copy(port, queue, pkts[i], g_mp,
                            PKTRACE_SNAP_LEN,
                            RTE_PCAPNG_DIRECTION_IN, NULL);
        if (!clone || rte_ring_enqueue(g_ring, clone) != 0) {
            if (clone)
                rte_pktmbuf_free(clone);
            atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
            atomic_fetch_sub_explicit(&g_captured, 1, memory_order_relaxed);
        }
    }
    return nb_pkts;
}

/* ─── TX callback (runs on worker lcore) ───────────────────────────────── */
static uint16_t
pktrace_tx_cb(uint16_t port, uint16_t queue,
              struct rte_mbuf **pkts, uint16_t nb_pkts,
              void *user_param)
{
    (void)user_param;

    if (!atomic_load_explicit(&g_active, memory_order_relaxed))
        return nb_pkts;

    for (uint16_t i = 0; i < nb_pkts; i++) {
        uint32_t n = atomic_fetch_add_explicit(&g_captured, 1,
                                               memory_order_relaxed);
        if (g_max_pkts && n >= g_max_pkts) {
            atomic_store_explicit(&g_active, 0, memory_order_relaxed);
            atomic_fetch_sub_explicit(&g_captured, 1, memory_order_relaxed);
            break;
        }
        struct rte_mbuf *clone =
            rte_pcapng_copy(port, queue, pkts[i], g_mp,
                            PKTRACE_SNAP_LEN,
                            RTE_PCAPNG_DIRECTION_OUT, NULL);
        if (!clone || rte_ring_enqueue(g_ring, clone) != 0) {
            if (clone)
                rte_pktmbuf_free(clone);
            atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
            atomic_fetch_sub_explicit(&g_captured, 1, memory_order_relaxed);
        }
    }
    return nb_pkts;
}

/* ─── Public API ────────────────────────────────────────────────────────── */

int
pktrace_init(void)
{
    /* Compute required mbuf data-room for pcapng-annotated packets. */
    uint32_t data_room = rte_pcapng_mbuf_size(PKTRACE_SNAP_LEN);

    g_ring = rte_ring_create("pktrace_ring", PKTRACE_RING_SZ,
                             rte_socket_id(), 0 /* MPMC */);
    if (!g_ring) {
        TGEN_ERR(TGEN_LOG_MGMT,
                 "pktrace: ring create failed: %s\n",
                 rte_strerror(rte_errno));
        return -ENOMEM;
    }

    g_mp = rte_pktmbuf_pool_create("pktrace_pool", PKTRACE_POOL_SZ,
                                   256 /* cache */, 0,
                                   (uint16_t)data_room,
                                   rte_socket_id());
    if (!g_mp) {
        TGEN_ERR(TGEN_LOG_MGMT,
                 "pktrace: mempool create failed: %s\n",
                 rte_strerror(rte_errno));
        rte_ring_free(g_ring);
        g_ring = NULL;
        return -ENOMEM;
    }

    TGEN_INFO(TGEN_LOG_MGMT,
              "pktrace: ready (ring=%u slots, pool=%u mbufs, "
              "snap=%u bytes, mbuf_data_room=%u)\n",
              PKTRACE_RING_SZ, PKTRACE_POOL_SZ,
              PKTRACE_SNAP_LEN, data_room);
    return 0;
}

void
pktrace_destroy(void)
{
    pktrace_stop();

    /* drain and free any remaining captured mbufs */
    if (g_ring) {
        void *obj;
        while (rte_ring_dequeue(g_ring, &obj) == 0)
            rte_pktmbuf_free((struct rte_mbuf *)obj);
        rte_ring_free(g_ring);
        g_ring = NULL;
    }
    if (g_mp) {
        rte_mempool_free(g_mp);
        g_mp = NULL;
    }
}

int
pktrace_start(uint16_t port_id, uint16_t queue_id, uint32_t max_pkts)
{
    if (!g_ring || !g_mp) {
        TGEN_ERR(TGEN_LOG_MGMT, "pktrace: not initialised\n");
        return -EINVAL;
    }
    if (atomic_load(&g_active)) {
        TGEN_WARN(TGEN_LOG_MGMT,
                  "pktrace: already active on port %u queue %u\n",
                  g_port, g_queue);
        return -EBUSY;
    }

    g_port      = port_id;
    g_queue     = queue_id;
    g_max_pkts  = max_pkts;
    atomic_store(&g_captured, 0);
    atomic_store(&g_dropped,  0);
    atomic_store(&g_active,   1);

    g_rx_cb = rte_eth_add_rx_callback(port_id, queue_id,
                                      pktrace_rx_cb, NULL);
    g_tx_cb = rte_eth_add_tx_callback(port_id, queue_id,
                                      pktrace_tx_cb, NULL);
    if (!g_rx_cb || !g_tx_cb) {
        TGEN_ERR(TGEN_LOG_MGMT,
                 "pktrace: failed to install callbacks on port %u q %u\n",
                 port_id, queue_id);
        pktrace_stop();
        return -EIO;
    }

    TGEN_INFO(TGEN_LOG_MGMT,
              "pktrace: capturing on port %u queue %u%s\n",
              port_id, queue_id,
              max_pkts ? "" : " (unlimited)");
    return 0;
}

void
pktrace_stop(void)
{
    if (!atomic_load(&g_active) && !g_rx_cb && !g_tx_cb)
        return;

    atomic_store(&g_active, 0);

    if (g_rx_cb) {
        rte_eth_remove_rx_callback(g_port, g_queue, g_rx_cb);
        g_rx_cb = NULL;
    }
    if (g_tx_cb) {
        rte_eth_remove_tx_callback(g_port, g_queue, g_tx_cb);
        g_tx_cb = NULL;
    }

    TGEN_INFO(TGEN_LOG_MGMT,
              "pktrace: stopped — captured=%u  dropped=%u  ring_used=%u\n",
              atomic_load(&g_captured),
              atomic_load(&g_dropped),
              rte_ring_count(g_ring));
}

int
pktrace_save(const char *path)
{
    if (!g_ring) {
        TGEN_ERR(TGEN_LOG_MGMT, "pktrace: not initialised\n");
        return -EINVAL;
    }

    uint32_t npkts = rte_ring_count(g_ring);
    if (npkts == 0) {
        TGEN_WARN(TGEN_LOG_MGMT, "pktrace: ring is empty, nothing to save\n");
        return 0;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        TGEN_ERR(TGEN_LOG_MGMT,
                 "pktrace: cannot open '%s': %s\n", path, strerror(errno));
        return -errno;
    }

    rte_pcapng_t *pcapng = rte_pcapng_fdopen(fd,
                                             NULL,   /* osname  */
                                             NULL,   /* hardware */
                                             "vaigai",
                                             NULL);  /* comment */
    if (!pcapng) {
        TGEN_ERR(TGEN_LOG_MGMT,
                 "pktrace: rte_pcapng_fdopen failed: %s\n",
                 rte_strerror(rte_errno));
        close(fd);
        return -EIO;
    }

    /* Register the interface that was captured. */
    rte_pcapng_add_interface(pcapng, g_port, NULL, NULL, NULL);

    /* Drain ring in batches and write to file. */
    struct rte_mbuf *batch[PKTRACE_BATCH];
    uint32_t total = 0;

    while (rte_ring_count(g_ring) > 0) {
        unsigned n = rte_ring_dequeue_burst(g_ring, (void **)batch,
                                            PKTRACE_BATCH, NULL);
        if (n == 0)
            break;
        /* rte_pcapng_write_packets frees the mbufs */
        ssize_t written = rte_pcapng_write_packets(pcapng, batch, (uint16_t)n);
        if (written < 0) {
            TGEN_ERR(TGEN_LOG_MGMT,
                     "pktrace: write error after %u packets\n", total);
            /* free remaining batch mbufs on error */
            for (unsigned i = 0; i < n; i++)
                rte_pktmbuf_free(batch[i]);
            break;
        }
        total += n;
    }

    rte_pcapng_write_stats(pcapng, g_port,
                           atomic_load(&g_captured),
                           atomic_load(&g_dropped),
                           NULL);
    rte_pcapng_close(pcapng);
    close(fd);

    TGEN_INFO(TGEN_LOG_MGMT,
              "pktrace: saved %u packets to '%s'\n", total, path);
    return (int)total;
}

uint32_t
pktrace_count(void)
{
    return atomic_load(&g_captured);
}
