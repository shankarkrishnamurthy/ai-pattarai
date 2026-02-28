/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: IPv4 protocol layer (§2.3, RFC 791).
 */
#ifndef TGEN_IPV4_H
#define TGEN_IPV4_H

#include <stdint.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── IPv4 transmit configuration per profile ────────────────────────────── */
typedef struct {
    uint32_t src_ip;     /* network byte order; 0 = from pool */
    uint32_t dst_ip;     /* network byte order */
    uint8_t  dscp_ecn;
    uint8_t  ttl;
    uint8_t  protocol;   /* IPPROTO_TCP / IPPROTO_UDP / IPPROTO_ICMP */
    bool     df;         /* DF bit in flags */
} ipv4_tx_cfg_t;

/** Push an IPv4 header onto the mbuf (after the transport header is set).
 *  Updates total_length; computes SW checksum if hw_cksum_offload==false. */
int ipv4_push_hdr(struct rte_mbuf *m,
                   const ipv4_tx_cfg_t *cfg,
                   uint16_t payload_len,
                   bool hw_cksum_offload,
                   uint32_t *id_counter);

/** Validate an incoming IPv4 packet.
 *  Strips the IP header on success.
 *  Returns inner protocol (IPPROTO_*), or -1 on validation failure. */
int ipv4_validate_and_strip(struct rte_mbuf *m,
                              uint32_t local_ip_net,
                              bool skip_cksum_if_hw_ok);

/** Worker input path for IPv4 frames.
 *  Returns an mbuf to TX if an immediate reply is needed, or NULL. */
struct rte_mbuf *ipv4_input(uint32_t worker_idx, struct rte_mbuf *m);

/** LPM: look up egress port + next-hop IP for a destination address.
 *  Returns 0 on hit, -1 on miss. */
int ipv4_route_lookup(uint32_t dst_ip_net,
                       uint32_t *next_hop_ip_out,
                       uint16_t *egress_port_out);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_IPV4_H */
