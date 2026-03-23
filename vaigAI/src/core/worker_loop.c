/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: worker poll-loop implementation (§1.7).
 */
#include "worker_loop.h"
#include "core_assign.h"
#include "mempool.h"
#include "ipc.h"
#include "tx_gen.h"

#include <string.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_log.h>

#include "../common/util.h"
#include "../common/types.h"
#include "../port/port_init.h"
#include "../net/ethernet.h"
#include "../net/arp.h"
#include "../net/ipv4.h"
#include "../net/icmp.h"
#include "../net/tcp_fsm.h"
#include "../net/tcp_timer.h"
/* tcp_tx_flush() declared in tcp_fsm.h — flushes batched TCP TX segments */
#include "../net/tcp_port_pool.h"
#include "../telemetry/metrics.h"
#include "../telemetry/cpu_stats.h"
#include "../app/server.h"

/* ── Globals ─────────────────────────────────────────────────────────────── */
volatile int      g_run = 0;
/* g_traffic: 1 = traffic generation active, 0 = paused/idle.
 * Distinct from g_run (process lifecycle): stop/start via REST/CLI
 * toggle g_traffic only; g_run=0 means the whole process should exit.
 * Atomic because REST thread writes from MHD_USE_INTERNAL_POLLING_THREAD. */
_Atomic int       g_traffic = 0;
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
    if (!eth) {
        RTE_LOG(WARNING, TGEN,
            "classify: NULL eth hdr! buf_addr=%p data_off=%u data_len=%u pkt_len=%u\n",
            m->buf_addr, m->data_off, m->data_len, m->pkt_len);
        rte_pktmbuf_free(m); return NULL;
    }

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
            /* Find this worker's position in the port's worker list */
            uint32_t my_pw = UINT32_MAX;
            for (uint32_t pw = 0; pw < g_core_map.port_num_workers[p]; pw++) {
                if (g_core_map.port_workers[p][pw] == ctx->lcore_id) {
                    my_pw = pw;
                    break;
                }
            }
            if (my_pw == UINT32_MAX)
                continue;  /* worker not assigned to this port */

            uint32_t max_rxq = g_port_caps[p].max_rx_queues;
            uint32_t max_txq = g_port_caps[p].max_tx_queues;
            if (max_rxq == 0) max_rxq = 1;
            if (max_txq == 0) max_txq = 1;

            /* Skip port if this worker would duplicate an RX queue
             * already owned by an earlier worker.  AF_PACKET / TAP
             * ports expose only 1 RX queue — only the first worker
             * in the list should poll them. */
            if (my_pw >= max_rxq)
                continue;

            uint32_t idx = ctx->num_ports++;
            ctx->ports[idx]     = (uint16_t)p;
            ctx->rx_queues[idx] = (uint16_t)my_pw;
            ctx->tx_queues[idx] = (uint16_t)(my_pw % max_txq);
            if (ctx->num_ports >= TGEN_MAX_PORTS) break;
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

    cpu_stats_t *cstats = &g_cpu_stats[ctx->worker_idx];
    cstats->window_start_tsc = rte_rdtsc();

    while (__atomic_load_n(&g_run, __ATOMIC_RELAXED)) {

        uint64_t t0 = rte_rdtsc();

        /* ── 1. Drain IPC ring from management ─────────────────────────── */
        config_update_t cmd;
        while (tgen_ipc_recv(ctx->worker_idx, &cmd)) {
            if (cmd.cmd == CFG_CMD_SHUTDOWN) {
                __atomic_store_n(&g_run, 0, __ATOMIC_RELAXED);
                goto done;
            }
            if (cmd.cmd == CFG_CMD_START) {
                tx_gen_config_t *gcfg = (tx_gen_config_t *)cmd.payload;
                /* Reset stale TCP state from any previous test.
                 * Sends RST for all active connections, frees TCBs
                 * and returns ports to the pool.  Do NOT call
                 * tcp_port_pool_reset() here — the management core
                 * already reset the pool AND applied the RSS queue-
                 * affinity filter before broadcasting this command.
                 * A second reset would destroy that filter. */
                tcp_fsm_reset_all(ctx->worker_idx);
                /* Clear pre-built HTTP request for this worker */
                if (gcfg->proto == TX_GEN_PROTO_HTTP)
                    g_http_req[ctx->worker_idx].hdr_len = 0;
                /* Only start traffic generation if this worker owns the
                 * target port.  With single-queue drivers (AF_PACKET/TAP),
                 * only one worker per port passes the max_rxq filter in
                 * tgen_worker_ctx_init().  Workers without the port must
                 * not TX on it: they can never receive replies (no RX
                 * queue), causing endless retransmits; and their TX calls
                 * would fire pktrace_tx_cb from a second lcore, violating
                 * the MPSC ring assumption. */
                uint16_t tx_q = UINT16_MAX;   /* UINT16_MAX = port not owned */
                for (uint32_t pp = 0; pp < ctx->num_ports; pp++) {
                    if (ctx->ports[pp] == gcfg->port_id) {
                        tx_q = ctx->tx_queues[pp];
                        break;
                    }
                }
                if (tx_q != UINT16_MAX) {
                    tx_gen_configure(&ctx->tx_gen, gcfg, tx_q);
                    tx_gen_start(&ctx->tx_gen);
                }
                tgen_ipc_ack(ctx->worker_idx, cmd.seq, 0);
                continue;
            }
            if (cmd.cmd == CFG_CMD_STOP) {
                tx_gen_stop(&ctx->tx_gen);
                /* Also stop server listeners if serving */
                if (g_srv_tables[ctx->worker_idx].serving) {
                    tcp_fsm_reset_all(ctx->worker_idx);
                    srv_table_stop_all(ctx->worker_idx);
                }
                tgen_ipc_ack(ctx->worker_idx, cmd.seq, 0);
                continue;
            }
            if (cmd.cmd == CFG_CMD_SERVE) {
                srv_ipc_payload_t *scfg = (srv_ipc_payload_t *)cmd.payload;
                tcp_fsm_reset_all(ctx->worker_idx);
                srv_table_configure(ctx->worker_idx, scfg);
                tgen_ipc_ack(ctx->worker_idx, cmd.seq, 0);
                continue;
            }
            if (cmd.cmd == CFG_CMD_STOP_LISTENER) {
                /* payload[0..3] = listener index, UINT32_MAX = stop all */
                uint32_t li;
                memcpy(&li, cmd.payload, sizeof(li));
                if (li == UINT32_MAX) {
                    tcp_fsm_reset_all(ctx->worker_idx);
                    srv_table_stop_all(ctx->worker_idx);
                } else {
                    srv_table_stop_one(ctx->worker_idx, li);
                }
                tgen_ipc_ack(ctx->worker_idx, cmd.seq, 0);
                continue;
            }
            tgen_ipc_ack(ctx->worker_idx, cmd.seq, 0);
        }

        uint64_t t1 = rte_rdtsc();

        /* ── 2. RX + classify + flush replies ────────────────────────── */
        uint16_t nb_rx_total = 0;
        n_tx = 0;
        for (uint32_t p = 0; p < ctx->num_ports; p++) {
            uint16_t nb_rx = rte_eth_rx_burst(ctx->ports[p],
                                               ctx->rx_queues[p],
                                               rx_pkts, TGEN_MAX_RX_BURST);
            if (nb_rx == 0) continue;
            nb_rx_total += nb_rx;

            uint64_t rx_total_bytes = 0;
            for (uint16_t bi = 0; bi < nb_rx; bi++)
                rx_total_bytes += rx_pkts[bi]->pkt_len;
            worker_metrics_add_rx(ctx->worker_idx, nb_rx, rx_total_bytes);

            for (uint16_t i = 0; i < nb_rx; i++) {
                struct rte_mbuf *reply = classify_and_process(ctx, rx_pkts[i]);
                if (reply && n_tx < TGEN_MAX_TX_BURST)
                    tx_pkts[n_tx++] = reply;
                else if (reply)
                    rte_pktmbuf_free(reply);
            }
        }
        if (n_tx > 0) {
            for (uint32_t p = 0; p < ctx->num_ports; p++) {
                uint16_t sent = rte_eth_tx_burst(ctx->ports[p],
                                                  ctx->tx_queues[p],
                                                  tx_pkts, (uint16_t)n_tx);
                for (uint16_t i = sent; i < n_tx; i++)
                    rte_pktmbuf_free(tx_pkts[i]);
                worker_metrics_add_tx(ctx->worker_idx, sent, 0); /* replies are few */
            }
            n_tx = 0;
        }

        /* Flush batched TCP TX segments (SYN-ACKs, ACKs, RSTs, data) */
        tcp_tx_flush(ctx->worker_idx);

        uint64_t t2 = rte_rdtsc();

        /* ── 3. TX generation (tps / sustained traffic) ─────────────── */
        /* In server mode, workers are reactive — no tx_gen.  Server-side
         * TX (echo, HTTP response, chargen) is handled inline by
         * srv_on_data() / srv_on_established() from tcp_fsm_input(). */
        if (ctx->tx_gen.active && !g_srv_tables[ctx->worker_idx].serving) {
            tx_gen_burst(&ctx->tx_gen, ctx->mempool, ctx->worker_idx);
        }

        /* Flush TCP TX segments generated by tx_gen_burst (SYNs) */
        tcp_tx_flush(ctx->worker_idx);

        uint64_t t3 = rte_rdtsc();

        /* ── 4. Timer wheel tick ─────────────────────────────────────────── */
        tcp_timer_tick(ctx->worker_idx);

        /* ── 5. Port pool tick — drain TIME_WAIT ring ────────────────── */
        tcp_port_pool_tick(ctx->worker_idx, t3);

        uint64_t t4 = rte_rdtsc();

        /* ── CPU cycle accounting ────────────────────────────────────── */
        cstats->cycles_ipc   += (t1 - t0);
        cstats->cycles_rx    += (t2 - t1);
        cstats->cycles_tx    += (t3 - t2);
        cstats->cycles_timer += (t4 - t3);
        cstats->cycles_total += (t4 - t0);
        if (nb_rx_total == 0 && !ctx->tx_gen.active)
            cstats->cycles_idle += (t4 - t0);
        cstats->loop_count++;
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
