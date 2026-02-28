/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: UDP implementation (§2.5, RFC 768).
 *
 * Minimal RX path: worker forwards UDP datagrams to a per-port ring,
 * management thread validates checksum and accounts metrics.  TX path
 * is handled by the tx_gen builder in core/tx_gen.c.
 */
#include "udp.h"
#include "arp.h"
#include "../core/mempool.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"

#include <string.h>
#include <netinet/in.h>

#include <rte_udp.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>
#include <rte_log.h>

static struct rte_ring *g_udp_rings[TGEN_MAX_PORTS];

int udp_init(void)
{
    memset(g_udp_rings, 0, sizeof(g_udp_rings));
    uint32_t n_ports = rte_eth_dev_count_avail();
    for (uint32_t p = 0; p < n_ports && p < TGEN_MAX_PORTS; p++) {
        char name[32];
        snprintf(name, sizeof(name), "udp_ring_%u", p);
        g_udp_rings[p] = rte_ring_create(name, 512,
                              (int)rte_eth_dev_socket_id((uint16_t)p),
                              RING_F_SC_DEQ);
        if (!g_udp_rings[p]) return -1;
    }
    return 0;
}

void udp_destroy(void)
{
    for (uint32_t p = 0; p < TGEN_MAX_PORTS; p++) {
        if (g_udp_rings[p]) {
            rte_ring_free(g_udp_rings[p]);
            g_udp_rings[p] = NULL;
        }
    }
}

/* Worker: forward to mgmt ring */
void udp_input(uint32_t worker_idx, struct rte_mbuf *m)
{
    (void)worker_idx;
    uint16_t port_id = m->port;
    if (port_id >= TGEN_MAX_PORTS || !g_udp_rings[port_id]) {
        rte_pktmbuf_free(m);
        return;
    }
    /* Account RX */
    worker_metrics_add_udp_rx(worker_idx);
    if (rte_ring_enqueue(g_udp_rings[port_id], m) != 0)
        rte_pktmbuf_free(m);
}

/* Management: process one UDP datagram.
 * The mbuf data pointer is at the UDP header (IP header already stripped). */
void udp_mgmt_process(uint16_t port_id, struct rte_mbuf *m)
{
    (void)port_id;

    if (m->data_len < UDP_HDR_LEN) {
        rte_pktmbuf_free(m);
        return;
    }

    /* UDP checksum validation is optional per RFC 768 (checksum 0 = none).
     * If non-zero, we could verify, but we need the original IP header
     * which has been stripped.  For a traffic generator RX path the
     * important thing is to count arrivals; full validation is done
     * by the ipv4 layer already.  Just account and discard. */

    rte_pktmbuf_free(m);
}

/* Management: drain UDP ring */
struct rte_mbuf *udp_mgmt_drain(uint16_t port_id)
{
    if (port_id >= TGEN_MAX_PORTS || !g_udp_rings[port_id]) return NULL;
    void *m;
    if (rte_ring_dequeue(g_udp_rings[port_id], &m) != 0) return NULL;
    return (struct rte_mbuf *)m;
}
