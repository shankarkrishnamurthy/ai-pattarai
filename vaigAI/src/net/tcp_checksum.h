/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP/IP checksum helpers with HW offload support (§3.4).
 */
#ifndef TGEN_TCP_CHECKSUM_H
#define TGEN_TCP_CHECKSUM_H

#include <stdint.h>
#include <string.h>
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
 * Verify TCP checksum in software using saved src/dst IPs.
 *
 * Called after the IP header has been stripped from the mbuf.
 * Uses the src/dst IPs saved in mbuf metadata before stripping,
 * and m->data_len as the TCP segment length.
 * Returns 0 if correct, non-zero on mismatch.
 */
static inline int
tcp_checksum_verify_stripped(uint32_t src_ip_nbo, uint32_t dst_ip_nbo,
                              const struct rte_tcp_hdr *tcph,
                              uint16_t tcp_seg_len)
{
    /* Build IPv4 pseudo-header for TCP checksum calculation */
    struct {
        uint32_t src_addr;
        uint32_t dst_addr;
        uint8_t  zero;
        uint8_t  proto;
        uint16_t len;
    } pseudo;
    pseudo.src_addr = src_ip_nbo;
    pseudo.dst_addr = dst_ip_nbo;
    pseudo.zero     = 0;
    pseudo.proto    = IPPROTO_TCP;
    pseudo.len      = rte_cpu_to_be_16(tcp_seg_len);

    uint32_t cksum = rte_raw_cksum(&pseudo, sizeof(pseudo));
    cksum += rte_raw_cksum(tcph, tcp_seg_len);
    cksum  = ((cksum >> 16) & 0xFFFF) + (cksum & 0xFFFF);
    cksum += (cksum >> 16);
    return ((cksum & 0xFFFF) == 0xFFFF) ? 0 : -1;
}

/**
 * Set TCP checksum for IPv6 — hardware offload when available, software otherwise.
 */
static inline void
tcp_checksum_set_v6(struct rte_mbuf *m,
                    const uint8_t *src_ip6,
                    const uint8_t *dst_ip6,
                    struct rte_tcp_hdr *tcph,
                    uint16_t tcp_seg_len,
                    int hw_cksum)
{
    tcph->cksum = 0;
    if (hw_cksum) {
        m->ol_flags |= RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_TCP_CKSUM;
        /* Seed pseudo-header checksum */
        struct {
            uint8_t  src[16];
            uint8_t  dst[16];
            uint32_t len;
            uint8_t  zeros[3];
            uint8_t  next_hdr;
        } __attribute__((packed)) pseudo;
        memcpy(pseudo.src, src_ip6, 16);
        memcpy(pseudo.dst, dst_ip6, 16);
        pseudo.len = rte_cpu_to_be_32((uint32_t)tcp_seg_len);
        pseudo.zeros[0] = 0;
        pseudo.zeros[1] = 0;
        pseudo.zeros[2] = 0;
        pseudo.next_hdr = IPPROTO_TCP;
        uint32_t cksum = rte_raw_cksum(&pseudo, sizeof(pseudo));
        cksum = ((cksum >> 16) & 0xFFFF) + (cksum & 0xFFFF);
        tcph->cksum = (uint16_t)~cksum;
    } else {
        /* Full software checksum over pseudo-header + TCP segment */
        struct {
            uint8_t  src[16];
            uint8_t  dst[16];
            uint32_t len;
            uint8_t  zeros[3];
            uint8_t  next_hdr;
        } __attribute__((packed)) pseudo;
        memcpy(pseudo.src, src_ip6, 16);
        memcpy(pseudo.dst, dst_ip6, 16);
        pseudo.len = rte_cpu_to_be_32((uint32_t)tcp_seg_len);
        pseudo.zeros[0] = 0;
        pseudo.zeros[1] = 0;
        pseudo.zeros[2] = 0;
        pseudo.next_hdr = IPPROTO_TCP;
        uint32_t cksum = rte_raw_cksum(&pseudo, sizeof(pseudo));
        cksum += rte_raw_cksum(tcph, tcp_seg_len);
        cksum = ((cksum >> 16) & 0xFFFF) + (cksum & 0xFFFF);
        cksum += (cksum >> 16);
        tcph->cksum = (uint16_t)(~cksum & 0xFFFF);
    }
}

/**
 * Verify TCP checksum in software using saved IPv6 src/dst addresses.
 */
static inline int
tcp_checksum_verify_stripped_v6(const uint8_t *src_ip6, const uint8_t *dst_ip6,
                                 const struct rte_tcp_hdr *tcph,
                                 uint16_t tcp_seg_len)
{
    struct {
        uint8_t  src[16];
        uint8_t  dst[16];
        uint32_t len;
        uint8_t  zeros[3];
        uint8_t  next_hdr;
    } __attribute__((packed)) pseudo;
    memcpy(pseudo.src, src_ip6, 16);
    memcpy(pseudo.dst, dst_ip6, 16);
    pseudo.len = rte_cpu_to_be_32((uint32_t)tcp_seg_len);
    pseudo.zeros[0] = 0;
    pseudo.zeros[1] = 0;
    pseudo.zeros[2] = 0;
    pseudo.next_hdr = IPPROTO_TCP;

    uint32_t cksum = rte_raw_cksum(&pseudo, sizeof(pseudo));
    cksum += rte_raw_cksum(tcph, tcp_seg_len);
    cksum = ((cksum >> 16) & 0xFFFF) + (cksum & 0xFFFF);
    cksum += (cksum >> 16);
    return ((cksum & 0xFFFF) == 0xFFFF) ? 0 : -1;
}

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TCP_CHECKSUM_H */
