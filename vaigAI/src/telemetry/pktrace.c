/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: DPDK-native packet capture via rte_pcapng + eth callbacks.
 *
 * Streaming mode:  when pktrace_start() is given a file path, packets are
 *                  continuously drained from the ring and written to disk
 *                  via pktrace_flush() (called from the mgmt-lcore tick).
 *                  The ring never fills up, so capture is unlimited.
 *
 * Buffered mode:   when path is NULL, packets accumulate in the ring
 *                  (capped at PKTRACE_RING_SZ) and are written out later
 *                  via pktrace_save().
 */
#include "pktrace.h"
#include "log.h"

#include <rte_ethdev.h>
#include <rte_pcapng.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_errno.h>

#include "../port/port_init.h"
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

/* ─── streaming state (only used when started with a file path) ─────────── */
static rte_pcapng_t                       *g_writer;
static int                                 g_out_fd = -1;
static uint32_t                            g_written;   /* packets flushed  */
static bool                                g_streaming;

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
        bool pool_empty = (clone == NULL);
        bool ring_full  = clone && (rte_ring_enqueue(g_ring, clone) != 0);
        if (pool_empty || ring_full) {
            if (clone)
                rte_pktmbuf_free(clone);
            uint32_t prev =
                atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
            atomic_fetch_sub_explicit(&g_captured, 1, memory_order_relaxed);
            /* Log first drop and every 1000th thereafter so we can see when
             * starvation begins without flooding the log. */
            if (prev == 0 || prev % 1000 == 0)
                RTE_LOG(WARNING, USER1,
                        "pktrace: DROP #%u — ring=%u/%u %s\n",
                        prev + 1,
                        rte_ring_count(g_ring), PKTRACE_RING_SZ,
                        pool_empty ? "(pool empty)" : "(ring full)");
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

/* ─── Internal: close the streaming writer ──────────────────────────────── */
static void
streaming_close(void)
{
    if (g_writer) {
        rte_pcapng_write_stats(g_writer, g_port,
                               atomic_load(&g_captured),
                               atomic_load(&g_dropped),
                               NULL);
        rte_pcapng_close(g_writer);
        g_writer = NULL;
    }
    if (g_out_fd >= 0) {
        close(g_out_fd);
        g_out_fd = -1;
    }
    g_streaming = false;
}

/* ─── Public API ────────────────────────────────────────────────────────── */

int
pktrace_init(void)
{
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
pktrace_start(uint16_t port_id, uint16_t queue_id, uint32_t max_pkts,
              const char *path)
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

    /* Drain any stale packets from a previous capture */
    {
        void *obj;
        while (rte_ring_dequeue(g_ring, &obj) == 0)
            rte_pktmbuf_free((struct rte_mbuf *)obj);
    }

    /* Open streaming output if a path was given */
    g_streaming = false;
    g_written   = 0;
    if (path) {
        g_out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_out_fd < 0) {
            TGEN_ERR(TGEN_LOG_MGMT,
                     "pktrace: cannot open '%s': %s\n", path, strerror(errno));
            return -errno;
        }
        g_writer = rte_pcapng_fdopen(g_out_fd, NULL, NULL, "vaigai", NULL);
        if (!g_writer) {
            TGEN_ERR(TGEN_LOG_MGMT,
                     "pktrace: rte_pcapng_fdopen failed: %s\n",
                     rte_strerror(rte_errno));
            close(g_out_fd);
            g_out_fd = -1;
            return -EIO;
        }
        /* Add an IDB for every active port so that the sequential interface
         * index in the pcapng file equals the DPDK port number.
         * rte_pcapng_copy() uses port_id directly as the EPB interface_id,
         * so interface 0 must be port 0, interface 1 must be port 1, etc.
         * Capturing port N without adding ports 0..N-1 first causes packets
         * to reference a non-existent interface_id and the file is malformed. */
        for (uint16_t p = 0; p < (uint16_t)g_n_ports; p++)
            rte_pcapng_add_interface(g_writer, p, NULL, NULL, NULL);
        g_streaming = true;
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
              "pktrace: capturing on port %u queue %u%s%s\n",
              port_id, queue_id,
              max_pkts ? "" : " (unlimited)",
              g_streaming ? " [streaming]" : " [buffered]");
    return 0;
}

void
pktrace_flush(void)
{
    if (!g_streaming || !g_writer)
        return;

    struct rte_mbuf *batch[PKTRACE_BATCH];
    unsigned n;
    while ((n = rte_ring_dequeue_burst(g_ring, (void **)batch,
                                       PKTRACE_BATCH, NULL)) > 0) {
        ssize_t written = rte_pcapng_write_packets(g_writer, batch, (uint16_t)n);
        /* Always free — rte_pcapng_write_packets() does NOT free mbufs */
        for (unsigned i = 0; i < n; i++)
            rte_pktmbuf_free(batch[i]);
        if (written < 0) {
            TGEN_ERR(TGEN_LOG_MGMT,
                     "pktrace: write error after %u flushed packets\n",
                     g_written);
            break;
        }
        g_written += n;
    }
}

void
pktrace_stop(void)
{
    if (!atomic_load(&g_active) && !g_rx_cb && !g_tx_cb) {
        /* Still close streaming resources if open */
        if (g_streaming)
            streaming_close();
        return;
    }

    atomic_store(&g_active, 0);

    if (g_rx_cb) {
        rte_eth_remove_rx_callback(g_port, g_queue, g_rx_cb);
        g_rx_cb = NULL;
    }
    if (g_tx_cb) {
        rte_eth_remove_tx_callback(g_port, g_queue, g_tx_cb);
        g_tx_cb = NULL;
    }

    /* In streaming mode, flush remaining ring contents and close file */
    if (g_streaming) {
        pktrace_flush();
        streaming_close();

        TGEN_INFO(TGEN_LOG_MGMT,
                  "pktrace: stopped — captured=%u  written=%u  dropped=%u\n",
                  atomic_load(&g_captured), g_written,
                  atomic_load(&g_dropped));
    } else {
        TGEN_INFO(TGEN_LOG_MGMT,
                  "pktrace: stopped — captured=%u  dropped=%u  ring_used=%u\n",
                  atomic_load(&g_captured),
                  atomic_load(&g_dropped),
                  rte_ring_count(g_ring));
    }
}

uint32_t
pktrace_count(void)
{
    return atomic_load(&g_captured);
}

bool
pktrace_is_active(void)
{
    return atomic_load_explicit(&g_active, memory_order_relaxed) != 0;
}
