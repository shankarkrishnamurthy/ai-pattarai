/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: IPv6 layer implementation (RFC 8200).
 */
#include "ipv6.h"
#include "icmpv6.h"
#include "ndp.h"
#include "udp.h"
#include "tcp_fsm.h"
#include "../port/port_init.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"

#include <string.h>
#include <netinet/in.h>

#include <rte_mbuf.h>
#include <rte_byteorder.h>
#include <rte_log.h>

/* Per-worker saved IPv6 addresses — set by ipv6_input() before stripping
 * the header, so protocol handlers can recover src/dst for TCB lookup
 * and checksum verification. */
__thread uint8_t t_saved_src6[16];
__thread uint8_t t_saved_dst6[16];

/* ── Build IPv6 header ───────────────────────────────────────────────────── */
int ipv6_push_hdr(struct rte_mbuf *m,
                   const uint8_t *src_ip6,
                   const uint8_t *dst_ip6,
                   uint8_t next_hdr,
                   uint16_t payload_len,
                   uint8_t hop_limit)
{
    struct rte_ipv6_hdr *ip6 =
        (struct rte_ipv6_hdr *)rte_pktmbuf_prepend(m, IPV6_HDR_LEN);
    if (!ip6) return -1;

    ip6->vtc_flow = rte_cpu_to_be_32(0x60000000); /* version=6, TC=0, flow=0 */
    ip6->payload_len = rte_cpu_to_be_16(payload_len);
    ip6->proto = next_hdr;
    ip6->hop_limits = hop_limit ? hop_limit : 64;
    memcpy(&ip6->src_addr, src_ip6, 16);
    memcpy(&ip6->dst_addr, dst_ip6, 16);

    m->l2_len = sizeof(struct rte_ether_hdr);
    m->l3_len = IPV6_HDR_LEN;
    return 0;
}

/* ── Validate incoming IPv6 ──────────────────────────────────────────────── */
int ipv6_validate_and_strip(struct rte_mbuf *m,
                              const uint8_t *local_ip6)
{
    if (m->data_len < IPV6_HDR_LEN) goto bad;

    const struct rte_ipv6_hdr *ip6 =
        rte_pktmbuf_mtod(m, const struct rte_ipv6_hdr *);

    /* Version must be 6 */
    uint32_t vtc = rte_be_to_cpu_32(ip6->vtc_flow);
    if ((vtc >> 28) != 6) goto bad;

    uint16_t payload_len = rte_be_to_cpu_16(ip6->payload_len);
    if ((uint32_t)payload_len + IPV6_HDR_LEN > m->data_len) goto bad;

    /* Destination check: match unicast, solicited-node multicast, or all-nodes */
    if (local_ip6) {
        bool dst_match = (memcmp(&ip6->dst_addr, local_ip6, 16) == 0);
        /* Accept multicast (ff00::/8) */
        if (!dst_match && ip6->dst_addr.a[0] == 0xff)
            dst_match = true;
        if (!dst_match) {
            worker_metrics_add_ip_not_for_us(rte_lcore_id());
            goto bad;
        }
    }

    uint8_t next_hdr = ip6->proto;

    /* Strip IPv6 header */
    if (rte_pktmbuf_adj(m, IPV6_HDR_LEN) == NULL) goto bad;

    /* Trim data_len to IPv6 payload length */
    if (m->data_len > payload_len) {
        m->data_len = payload_len;
        m->pkt_len  = payload_len;
    }

    return (int)next_hdr;

bad:
    rte_pktmbuf_free(m);
    return -1;
}

/* ── Worker input: dispatch by next header ───────────────────────────────── */
struct rte_mbuf *ipv6_input(uint32_t worker_idx, struct rte_mbuf *m)
{
    uint16_t port_id = m->port;
    const uint8_t *local_ip6 = NULL;
    if (port_id < TGEN_MAX_PORTS && g_ndp[port_id].has_ip6)
        local_ip6 = g_ndp[port_id].local_ip6;

    /* Save IPv6 src/dst in thread-local storage before stripping */
    if (m->data_len >= IPV6_HDR_LEN) {
        const struct rte_ipv6_hdr *ip6 =
            rte_pktmbuf_mtod(m, const struct rte_ipv6_hdr *);
        memcpy(t_saved_src6, &ip6->src_addr, 16);
        memcpy(t_saved_dst6, &ip6->dst_addr, 16);

        /* Also save IP version indicator in mbuf metadata for TCP FSM */
        m->hash.usr = 6; /* version marker */
    }

    int next_hdr = ipv6_validate_and_strip(m, local_ip6);
    if (next_hdr < 0) return NULL;

    switch (next_hdr) {
    case IPPROTO_ICMPV6:
        icmpv6_input(worker_idx, m);
        return NULL;
    case IPPROTO_UDP:
        udp_input(worker_idx, m);
        return NULL;
    case IPPROTO_TCP:
        tcp_fsm_input(worker_idx, m);
        return NULL;
    default:
        rte_pktmbuf_free(m);
        return NULL;
    }
}
