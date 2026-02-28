/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: port abstraction & capability negotiation (§1.5).
 */
#ifndef TGEN_PORT_INIT_H
#define TGEN_PORT_INIT_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Port capability flags ────────────────────────────────────────────────── */
typedef struct {
    driver_kind_t driver;
    char          driver_name[64];
    bool          has_ipv4_cksum_offload;
    bool          has_tcp_cksum_offload;
    bool          has_udp_cksum_offload;
    bool          has_sctp_cksum_offload;
    bool          has_scatter_rx;
    bool          has_multi_seg_tx;
    bool          has_rss;
    bool          has_vlan_offload;
    uint32_t      max_rx_queues;
    uint32_t      max_tx_queues;
    uint32_t      rx_desc_lim_min;
    uint32_t      rx_desc_lim_max;
    uint32_t      tx_desc_lim_min;
    uint32_t      tx_desc_lim_max;
    uint32_t      socket_id;
    struct rte_ether_addr mac_addr;
} port_caps_t;

/** Per-port caps array, indexed by DPDK port_id. */
extern port_caps_t g_port_caps[TGEN_MAX_PORTS];
extern uint32_t    g_n_ports;

/** Initialise all DPDK ports: probe capabilities, configure queues,
 *  set RSS, enable promiscuous mode, start device.
 *  @param num_rx_desc  Desired RX descriptors per queue (clamped to driver max)
 *  @param num_tx_desc  Desired TX descriptors per queue
 *  Returns 0 on success, -1 on error. */
int tgen_ports_init(uint32_t num_rx_desc, uint32_t num_tx_desc);

/** Stop and close all ports. */
void tgen_ports_close(void);

/** Display per-port capability summary. */
void tgen_ports_dump(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_PORT_INIT_H */
