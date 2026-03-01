/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: port abstraction implementation — capability negotiation (§1.5).
 */
#include "port_init.h"
#include "soft_nic.h"
#include "../core/core_assign.h"
#include "../core/mempool.h"
#include "../common/types.h"
#include "../common/util.h"

#include <string.h>
#include <stdio.h>

#include <rte_ethdev.h>
#include <rte_eth_ctrl.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_flow.h>

/* ── Symmetric Toeplitz RSS key (40 bytes) ────────────────────────────────── */
static const uint8_t g_rss_key_sym[40] = {
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
};

/* ── Globals ─────────────────────────────────────────────────────────────── */
port_caps_t g_port_caps[TGEN_MAX_PORTS];
uint32_t    g_n_ports;

/* ── Probe & populate port_caps_t ─────────────────────────────────────────── */
static void probe_caps(uint16_t port_id, port_caps_t *caps)
{
    struct rte_eth_dev_info info;
    int _r1 = rte_eth_dev_info_get(port_id, &info); (void)_r1;

    snprintf(caps->driver_name, sizeof(caps->driver_name), "%s",
             info.driver_name ? info.driver_name : "unknown");

    caps->driver = soft_nic_detect(caps->driver_name);

    uint64_t tx_ol = info.tx_offload_capa;
    uint64_t rx_ol = info.rx_offload_capa;

    caps->has_ipv4_cksum_offload = !!(tx_ol & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM);
    caps->has_tcp_cksum_offload  = !!(tx_ol & RTE_ETH_TX_OFFLOAD_TCP_CKSUM);
    caps->has_udp_cksum_offload  = !!(tx_ol & RTE_ETH_TX_OFFLOAD_UDP_CKSUM);
    caps->has_sctp_cksum_offload = !!(tx_ol & RTE_ETH_TX_OFFLOAD_SCTP_CKSUM);
    caps->has_scatter_rx         = !!(rx_ol & RTE_ETH_RX_OFFLOAD_SCATTER);
    caps->has_multi_seg_tx       = !!(tx_ol & RTE_ETH_TX_OFFLOAD_MULTI_SEGS);
    caps->has_rss                = (info.flow_type_rss_offloads != 0);
    caps->has_vlan_offload       = !!(tx_ol & RTE_ETH_TX_OFFLOAD_VLAN_INSERT);

    caps->max_rx_queues  = info.max_rx_queues;
    caps->max_tx_queues  = info.max_tx_queues;
    caps->rx_desc_lim_min = info.rx_desc_lim.nb_min;
    caps->rx_desc_lim_max = info.rx_desc_lim.nb_max;
    caps->tx_desc_lim_min = info.tx_desc_lim.nb_min;
    caps->tx_desc_lim_max = info.tx_desc_lim.nb_max;
    caps->socket_id = (uint32_t)rte_eth_dev_socket_id(port_id);

    rte_eth_macaddr_get(port_id, &caps->mac_addr);
}

/* ── Configure and start a single port ─────────────────────────────────────── */
static int port_setup(uint16_t port_id,
                       uint32_t n_rxq, uint32_t n_txq,
                       uint32_t rx_desc, uint32_t tx_desc,
                       struct rte_mempool *mp)
{
    port_caps_t *caps = &g_port_caps[port_id];
    probe_caps(port_id, caps);

    /* Use rte_eth_dev_adjust_nb_rx_tx_desc instead of manual clamping:
     * this API properly handles PMDs (e.g. mlx5) that report min=0, max=0
     * in their desc_lim but enforce limits internally.                    */

    /* Clamp queue counts */
    if (n_rxq > caps->max_rx_queues) {
        RTE_LOG(WARNING, PORT,
            "Port %u: RX queue count clamped %u → %u\n",
            port_id, n_rxq, caps->max_rx_queues);
        n_rxq = caps->max_rx_queues;
    }
    if (n_txq > caps->max_tx_queues) {
        RTE_LOG(WARNING, PORT,
            "Port %u: TX queue count clamped %u → %u\n",
            port_id, n_txq, caps->max_tx_queues);
        n_txq = caps->max_tx_queues;
    }
    if (n_txq == 0)
        n_txq = 1; /* single-queue fallback (e.g. net_af_packet reports max=0) */
    if (!caps->has_rss)
        n_rxq = 1; /* single-queue fallback */

    struct rte_eth_conf port_conf;
    memset(&port_conf, 0, sizeof(port_conf));

    /* Enable RSS if supported */
    if (caps->has_rss && n_rxq > 1) {
        port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
        port_conf.rx_adv_conf.rss_conf.rss_key     = (uint8_t *)g_rss_key_sym;
        port_conf.rx_adv_conf.rss_conf.rss_key_len = sizeof(g_rss_key_sym);
        port_conf.rx_adv_conf.rss_conf.rss_hf =
            RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP;
    }

    /* TX offloads */
    if (caps->has_ipv4_cksum_offload)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
    if (caps->has_tcp_cksum_offload)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
    if (caps->has_udp_cksum_offload)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
    if (caps->has_multi_seg_tx)
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

    int rc = rte_eth_dev_configure(port_id, (uint16_t)n_rxq, (uint16_t)n_txq,
                                    &port_conf);
    if (rc < 0) {
        RTE_LOG(ERR, PORT, "Port %u: rte_eth_dev_configure failed: %d\n",
                port_id, rc);
        return -1;
    }

    /* Let the driver negotiate actual descriptor counts.
     * This handles PMDs like mlx5 that may report bogus limits in
     * dev_info but accept sane values through this API.             */
    uint16_t nb_rxd = (uint16_t)rx_desc;
    uint16_t nb_txd = (uint16_t)tx_desc;
    rc = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);
    if (rc < 0) {
        RTE_LOG(ERR, PORT,
                "Port %u: rte_eth_dev_adjust_nb_rx_tx_desc failed: %d\n",
                port_id, rc);
        return -1;
    }
    if (nb_rxd != rx_desc || nb_txd != tx_desc)
        RTE_LOG(INFO, PORT,
                "Port %u: descriptors adjusted RX %u→%u  TX %u→%u\n",
                port_id, rx_desc, nb_rxd, tx_desc, nb_txd);
    rx_desc = nb_rxd;
    tx_desc = nb_txd;

    /* RX queues */
    struct rte_eth_rxconf rxconf;
    struct rte_eth_dev_info info;
    int _r2 = rte_eth_dev_info_get(port_id, &info); (void)_r2;
    rxconf = info.default_rxconf;
    if (!caps->has_scatter_rx)
        rxconf.offloads &= ~RTE_ETH_RX_OFFLOAD_SCATTER;

    for (uint16_t q = 0; q < n_rxq; q++) {
        rc = rte_eth_rx_queue_setup(port_id, q, (uint16_t)rx_desc,
                                     (unsigned int)caps->socket_id,
                                     &rxconf, mp);
        if (rc < 0) {
            RTE_LOG(ERR, PORT, "Port %u RX queue %u setup failed: %d\n",
                    port_id, q, rc);
            return -1;
        }
    }

    /* TX queues */
    struct rte_eth_txconf txconf = info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;

    for (uint16_t q = 0; q < n_txq; q++) {
        rc = rte_eth_tx_queue_setup(port_id, q, (uint16_t)tx_desc,
                                     (unsigned int)caps->socket_id,
                                     &txconf);
        if (rc < 0) {
            RTE_LOG(ERR, PORT, "Port %u TX queue %u setup failed: %d\n",
                    port_id, q, rc);
            return -1;
        }
    }

    /* Promiscuous mode */
    rte_eth_promiscuous_enable(port_id);

    rc = rte_eth_dev_start(port_id);
    if (rc < 0) {
        RTE_LOG(ERR, PORT, "Port %u: rte_eth_dev_start failed: %d\n",
                port_id, rc);
        return -1;
    }

    char mac_buf[18];
    tgen_mac_str(caps->mac_addr.addr_bytes, mac_buf, sizeof(mac_buf));
    RTE_LOG(INFO, PORT,
        "Port %u: driver=%s mac=%s rxq=%u txq=%u rx_desc=%u tx_desc=%u\n",
        port_id, caps->driver_name, mac_buf, n_rxq, n_txq, rx_desc, tx_desc);

    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */
int tgen_ports_init(uint32_t num_rx_desc, uint32_t num_tx_desc)
{
    g_n_ports = rte_eth_dev_count_avail();
    if (g_n_ports == 0) {
        RTE_LOG(ERR, PORT, "No DPDK ports available\n");
        return -1;
    }
    if (g_n_ports > TGEN_MAX_PORTS) {
        RTE_LOG(WARNING, PORT,
            "Capping port count at %u (have %u)\n", TGEN_MAX_PORTS, g_n_ports);
        g_n_ports = TGEN_MAX_PORTS;
    }

    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id) {
        if (port_id >= TGEN_MAX_PORTS) break;

        /* Determine queues: workers per port */
        uint32_t n_queues = g_core_map.port_num_workers[port_id];
        if (n_queues == 0) n_queues = 1;

        /* Use first worker's mempool (all workers on same socket use same pool) */
        struct rte_mempool *mp = NULL;
        for (uint32_t w = 0; w < g_core_map.port_num_workers[port_id]; w++) {
            uint32_t wlcore = g_core_map.port_workers[port_id][w];
            for (uint32_t wi = 0; wi < g_core_map.num_workers; wi++) {
                if (g_core_map.worker_lcores[wi] == wlcore) {
                    mp = g_worker_mempools[wi];
                    break;
                }
            }
            if (mp) break;
        }
        if (!mp && g_core_map.num_workers > 0)
            mp = g_worker_mempools[0];

        if (port_setup(port_id, n_queues, n_queues,
                       num_rx_desc, num_tx_desc, mp) < 0)
            return -1;

        /* Run soft NIC post-init if needed */
        soft_nic_post_init(port_id, &g_port_caps[port_id]);
    }

    return 0;
}

void tgen_ports_close(void)
{
    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id) {
        if (port_id >= TGEN_MAX_PORTS) break;
        rte_eth_dev_stop(port_id);
        rte_eth_dev_close(port_id);
    }
}

void tgen_ports_dump(void)
{
    for (uint32_t p = 0; p < g_n_ports; p++) {
        port_caps_t *c = &g_port_caps[p];
        RTE_LOG(INFO, PORT,
            "  Port %u: driver=%-16s ipv4_cksum=%d tcp_cksum=%d "
            "rss=%d scatter=%d multi_seg=%d\n",
            p, c->driver_name,
            c->has_ipv4_cksum_offload, c->has_tcp_cksum_offload,
            c->has_rss, c->has_scatter_rx, c->has_multi_seg_tx);
    }
}
