/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: worker poll-loop implementation (§1.7).
 */
#include "worker_loop.h"
#include "core_assign.h"
#include "mempool.h"
#include "ipc.h"

#include <string.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_log.h>

#include "../common/util.h"
#include "../common/types.h"
#include "../net/ethernet.h"
#include "../net/arp.h"
#include "../net/ipv4.h"
#include "../net/icmp.h"
#include "../net/tcp_fsm.h"
#include "../net/tcp_timer.h"
#include "../telemetry/metrics.h"

/* ── Globals ─────────────────────────────────────────────────────────────── */
volatile int      g_run = 0;
/* g_traffic: 1 = traffic generation active, 0 = paused/idle.
 * Distinct from g_run (process lifecycle): stop/start via REST/CLI
 * toggle g_traffic only; g_run=0 means the whole process should exit. */
volatile int      g_traffic = 0;
worker_ctx_t      g_worker_ctx[TGEN_MAX_WORKERS];

/* ── TX drain helper ─────────────────────────────────────────────────────── */
static inline void tx_drain(worker_ctx_t *ctx,
                              struct rte_mbuf **tx_pkts, uint32_t n)
{
    uint32_t sent;
    for (uint32_t p = 0; p < ctx->num_ports; p++) {
        if (n == 0) break;
        sent = rte_eth_tx_burst(ctx->ports[p], ctx->tx_queues[p], tx_pkts, (uint16_t)n);
        /* Free unsent mbufs */
        for (uint32_t i = sent; i < n; i++)
            rte_pktmbuf_free(tx_pkts[i]);
        /* Update metrics */
        worker_metrics_add_tx(ctx->worker_idx, sent, 0 /* bytes tracked in classif */);
    }
}

/* ── Packet classification helper ────────────────────────────────────────── */
/*
 * Returns an rte_mbuf* to enqueue for TX if the packet generates an
 * immediate response (e.g. ARP reply, ICMP echo reply), or NULL.
 * More complex responses (TCP) are handled by tcp_fsm_process().
 */
static inline struct rte_mbuf *classify_and_process(worker_ctx_t *ctx,
                                                      struct rte_mbuf *m)
{
    (void)ctx;
    /* Peek at ether_type without advancing the data pointer.
     * ARP processing (arp_mgmt_process) needs the full Ethernet frame,
     * so we must NOT strip it for ARP.  IPv4 processing must have the
     * Ethernet header stripped so ipv4_validate_and_strip sees byte 0
     * as the IP version/IHL field. */
    const struct rte_ether_hdr *eth = eth_hdr(m);
    if (!eth) { rte_pktmbuf_free(m); return NULL; }

    uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);

    /* Peek through 802.1Q tag to get inner type */
    if (ether_type == RTE_ETHER_TYPE_VLAN) {
        const struct rte_vlan_hdr *vlan =
            rte_pktmbuf_mtod_offset(m, struct rte_vlan_hdr *,
                                    sizeof(struct rte_ether_hdr));
        ether_type = rte_be_to_cpu_16(vlan->eth_proto);
    }

    switch (ether_type) {
    case RTE_ETHER_TYPE_ARP:
        /* Forward full Ethernet frame to ARP ring — mgmt reads it */
        arp_input(ctx->worker_idx, m);
        return NULL;

    case RTE_ETHER_TYPE_IPV4:
        /* Strip Ethernet (+ VLAN) header so IPv4 handler sees IP at offset 0 */
        eth_pop_hdr(m);
        return ipv4_input(ctx->worker_idx, m);

    default:
        rte_pktmbuf_free(m);
        return NULL;
    }
}

/* ── Worker context initialisation ───────────────────────────────────────── */
int tgen_worker_ctx_init(void)
{
    memset(g_worker_ctx, 0, sizeof(g_worker_ctx));
    uint32_t n_workers = g_core_map.num_workers;

    for (uint32_t w = 0; w < n_workers; w++) {
        worker_ctx_t *ctx = &g_worker_ctx[w];
        ctx->worker_idx  = w;
        ctx->lcore_id    = g_core_map.worker_lcores[w];
        ctx->socket_id   = g_core_map.socket_of_lcore[ctx->lcore_id];
        ctx->mempool     = g_worker_mempools[w];

        /* Assign ports served by this worker */
        uint32_t n_ports = rte_eth_dev_count_avail();
        for (uint32_t p = 0; p < n_ports && p < TGEN_MAX_PORTS; p++) {
            /* Check if this worker is in port_workers list */
            bool found = false;
            for (uint32_t pw = 0; pw < g_core_map.port_num_workers[p]; pw++) {
                if (g_core_map.port_workers[p][pw] == ctx->lcore_id) {
                    found = true;
                    break;
                }
            }
            if (found) {
                uint32_t idx = ctx->num_ports++;
                ctx->ports[idx]     = (uint16_t)p;
                ctx->rx_queues[idx] = (uint16_t)w; /* queue index = worker index */
                ctx->tx_queues[idx] = (uint16_t)w;
                if (ctx->num_ports >= TGEN_MAX_PORTS) break;
            }
        }
    }
    return 0;
}

/* ── Main worker loop ────────────────────────────────────────────────────── */
int tgen_worker_loop(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    uint32_t lcore_id = rte_lcore_id();

    /* Seed per-core PRNG */
    tgen_prng_seed(rte_rdtsc() ^ ((uint64_t)lcore_id << 32));

    struct rte_mbuf *rx_pkts[TGEN_MAX_RX_BURST];
    struct rte_mbuf *tx_pkts[TGEN_MAX_TX_BURST];
    uint32_t n_tx = 0;

    while (__atomic_load_n(&g_run, __ATOMIC_RELAXED)) {

        /* ── 1. Drain IPC ring from management ─────────────────────────── */
        config_update_t cmd;
        while (tgen_ipc_recv(ctx->worker_idx, &cmd)) {
            if (cmd.cmd == CFG_CMD_SHUTDOWN) {
                __atomic_store_n(&g_run, 0, __ATOMIC_RELAXED);
                goto done;
            }
            /* TODO: dispatch other commands to appropriate handlers */
            tgen_ipc_ack(ctx->worker_idx, cmd.seq, 0);
        }

        /* ── 2. RX + classify ────────────────────────────────────────────── */
        n_tx = 0;
        for (uint32_t p = 0; p < ctx->num_ports; p++) {
            uint16_t nb_rx = rte_eth_rx_burst(ctx->ports[p],
                                               ctx->rx_queues[p],
                                               rx_pkts, TGEN_MAX_RX_BURST);
            if (nb_rx == 0) continue;

            worker_metrics_add_rx(ctx->worker_idx, nb_rx, 0);

            for (uint16_t i = 0; i < nb_rx; i++) {
                struct rte_mbuf *reply = classify_and_process(ctx, rx_pkts[i]);
                if (reply && n_tx < TGEN_MAX_TX_BURST)
                    tx_pkts[n_tx++] = reply;
                else if (reply)
                    rte_pktmbuf_free(reply);
            }
        }

        /* ── 3. TX burst ─────────────────────────────────────────────────── */
        if (n_tx > 0) {
            for (uint32_t p = 0; p < ctx->num_ports; p++) {
                uint16_t sent = rte_eth_tx_burst(ctx->ports[p],
                                                  ctx->tx_queues[p],
                                                  tx_pkts, (uint16_t)n_tx);
                for (uint16_t i = sent; i < n_tx; i++)
                    rte_pktmbuf_free(tx_pkts[i]);
                worker_metrics_add_tx(ctx->worker_idx, sent, 0);
            }
            n_tx = 0;
        }

        /* ── 4. Timer wheel tick ─────────────────────────────────────────── */
        tcp_timer_tick(ctx->worker_idx);
    }

done:
    RTE_LOG(INFO, TGEN, "Worker %u exiting\n", ctx->worker_idx);
    return 0;
}

void tgen_workers_stop(void)
{
    __atomic_store_n(&g_run, 0, __ATOMIC_RELAXED);
}

void tgen_workers_join(void)
{
    uint32_t lcore_id;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        rte_eal_wait_lcore(lcore_id);
    }
}
