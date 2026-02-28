/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: IPv4 layer implementation (§2.3, RFC 791).
 */
#include "ipv4.h"
#include "icmp.h"
#include "udp.h"
#include "lpm.h"
#include "arp.h"
#include "tcp_fsm.h"
#include "../port/port_init.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"

#include <string.h>
#include <netinet/in.h>

#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_byteorder.h>
#include <rte_log.h>

/* Per-worker monotonically incrementing IP ID counter — used via id_counter
 * parameter passed to ipv4_push_hdr(). */
/* static __thread uint32_t t_ip_id[TGEN_MAX_PORTS] = {0}; */

/* ── Build IPv4 header ───────────────────────────────────────────────────── */
int ipv4_push_hdr(struct rte_mbuf *m,
                   const ipv4_tx_cfg_t *cfg,
                   uint16_t payload_len,
                   bool hw_cksum_offload,
                   uint32_t *id_counter)
{
    struct rte_ipv4_hdr *ip =
        (struct rte_ipv4_hdr *)rte_pktmbuf_prepend(m,
                                    sizeof(struct rte_ipv4_hdr));
    if (!ip) return -1;

    uint16_t total_len = (uint16_t)(sizeof(*ip) + payload_len);
    uint16_t ip_id     = (id_counter) ? (uint16_t)((*id_counter)++ & 0xFFFF)
                                       : 0;
    uint16_t frag_off  = cfg->df ? rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG) : 0;

    ip->version_ihl   = RTE_IPV4_VHL_DEF;
    ip->type_of_service = cfg->dscp_ecn;
    ip->total_length  = rte_cpu_to_be_16(total_len);
    ip->packet_id     = rte_cpu_to_be_16(ip_id);
    ip->fragment_offset = frag_off;
    ip->time_to_live  = cfg->ttl ? cfg->ttl : 64;
    ip->next_proto_id = cfg->protocol;
    ip->hdr_checksum  = 0;
    ip->src_addr      = cfg->src_ip;
    ip->dst_addr      = cfg->dst_ip;

    if (!hw_cksum_offload) {
        ip->hdr_checksum = rte_ipv4_cksum(ip);
    } else {
        m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
        m->l3_len    = sizeof(*ip);
    }
    return 0;
}

/* ── Validate incoming IPv4 ──────────────────────────────────────────────── */
int ipv4_validate_and_strip(struct rte_mbuf *m,
                              uint32_t local_ip_net,
                              bool skip_cksum_if_hw_ok)
{
    if (m->data_len < sizeof(struct rte_ipv4_hdr)) goto bad;

    struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod(m, struct rte_ipv4_hdr *);

    /* Version = 4, IHL >= 5 */
    if ((ip->version_ihl >> 4) != 4)  goto bad;
    uint8_t ihl = (ip->version_ihl & 0x0F);
    if (ihl < 5) goto bad;

    uint16_t total_len = rte_be_to_cpu_16(ip->total_length);
    if (total_len > m->data_len) goto bad;

    /* Checksum */
    if (!skip_cksum_if_hw_ok ||
        !(m->ol_flags & RTE_MBUF_F_RX_IP_CKSUM_GOOD)) {
        if (rte_ipv4_cksum(ip) != 0) {
            worker_metrics_add_ip_bad_cksum(rte_lcore_id());
            goto bad;
        }
    }

    /* Fragment check: MF=1 or offset>0 → drop */
    uint16_t foff = rte_be_to_cpu_16(ip->fragment_offset);
    if ((foff & RTE_IPV4_HDR_MF_FLAG) || (foff & RTE_IPV4_HDR_OFFSET_MASK)) {
        worker_metrics_add_ip_frag_dropped(rte_lcore_id());
        goto bad;
    }

    /* Destination match */
    if (local_ip_net && ip->dst_addr != local_ip_net) {
        worker_metrics_add_ip_not_for_us(rte_lcore_id());
        goto bad;
    }

    uint8_t proto = ip->next_proto_id;
    /* Strip IP header */
    if (rte_pktmbuf_adj(m, (uint16_t)(ihl * 4)) == NULL) goto bad;

    return (int)proto;

bad:
    rte_pktmbuf_free(m);
    return -1;
}

/* ── Worker input: dispatch by protocol ──────────────────────────────────── */
struct rte_mbuf *ipv4_input(uint32_t worker_idx, struct rte_mbuf *m)
{
    /* We need local IP to validate destination — use port 0 for now */
    uint16_t port_id = m->port;
    uint32_t local_ip = (port_id < TGEN_MAX_PORTS) ?
                        g_arp[port_id].local_ip : 0;

    bool skip_cksum = g_port_caps[port_id].has_ipv4_cksum_offload;
    int  proto = ipv4_validate_and_strip(m, local_ip, skip_cksum);
    if (proto < 0) return NULL;

    switch (proto) {
    case IPPROTO_ICMP:
        icmp_input(worker_idx, m);
        return NULL;
    case IPPROTO_UDP:
        udp_input(worker_idx, m);
        return NULL;
    case IPPROTO_TCP:
        /* TCP handled by FSM */
        tcp_fsm_input(worker_idx, m);
        return NULL;
    default:
        /* Unsupported protocol — generate ICMP Unreachable if not worker */
        rte_pktmbuf_free(m);
        return NULL;
    }
}

/* ── Route lookup (LPM wrapper) ──────────────────────────────────────────── */
int ipv4_route_lookup(uint32_t dst_ip_net,
                       uint32_t *next_hop_ip_out,
                       uint16_t *egress_port_out)
{
    return lpm_lookup(dst_ip_net, next_hop_ip_out, egress_port_out);
}
