/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP/IP checksum helpers with HW offload support (§3.4).
 */
#ifndef TGEN_TCP_CHECKSUM_H
#define TGEN_TCP_CHECKSUM_H

#include <stdint.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set TCP checksum — hardware offload when available, software otherwise.
 *
 * @param m        Packet mbuf (data already built, m->l2/l3/l4_len set).
 * @param ip4h     Pointer to the IPv4 header inside the mbuf.
 * @param tcph     Pointer to the TCP  header inside the mbuf.
 * @param hw_cksum Non-zero if the port supports TX TCP checksum offload.
 */
static inline void
tcp_checksum_set(struct rte_mbuf *m,
                 struct rte_ipv4_hdr *ip4h,
                 struct rte_tcp_hdr  *tcph,
                 int hw_cksum)
{
    tcph->cksum = 0;
    if (hw_cksum) {
        /* Let the NIC compute it */
        ip4h->hdr_checksum = 0;
        m->ol_flags |= RTE_MBUF_F_TX_IPV4 |
                       RTE_MBUF_F_TX_IP_CKSUM |
                       RTE_MBUF_F_TX_TCP_CKSUM;
        /* Pseudo-header checksum seeded into tcph->cksum */
        tcph->cksum = rte_ipv4_phdr_cksum(ip4h, m->ol_flags);
    } else {
        /* Software full checksum */
        ip4h->hdr_checksum = 0;
        ip4h->hdr_checksum = rte_ipv4_cksum(ip4h);
        tcph->cksum = rte_ipv4_udptcp_cksum(ip4h, tcph);
    }
}

/**
 * Verify TCP checksum in software.
 * Returns 0 if correct, non-zero on mismatch.
 */
static inline int
tcp_checksum_verify(const struct rte_ipv4_hdr *ip4h,
                    const struct rte_tcp_hdr  *tcph)
{
    return rte_ipv4_udptcp_cksum(ip4h, tcph) != 0xFFFF;
}

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TCP_CHECKSUM_H */
