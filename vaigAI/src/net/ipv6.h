/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: IPv6 protocol layer (RFC 8200).
 */
#ifndef TGEN_IPV6_H
#define TGEN_IPV6_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPV6_HDR_LEN  40  /* fixed IPv6 header length */

/* ── Per-worker saved IPv6 addresses (set by ipv6_input before stripping) ── */
extern __thread uint8_t t_saved_src6[16];
extern __thread uint8_t t_saved_dst6[16];

/** Push an IPv6 header onto the mbuf.
 *  @param src_ip6  Source IPv6 address (16 bytes, network byte order).
 *  @param dst_ip6  Destination IPv6 address (16 bytes, network byte order).
 *  @param next_hdr Next header protocol (IPPROTO_TCP, IPPROTO_UDP, etc.).
 *  @param payload_len  Length of payload after IPv6 header.
 *  @param hop_limit  TTL equivalent (default 64).
 *  Returns 0 on success, -1 on failure. */
int ipv6_push_hdr(struct rte_mbuf *m,
                   const uint8_t *src_ip6,
                   const uint8_t *dst_ip6,
                   uint8_t next_hdr,
                   uint16_t payload_len,
                   uint8_t hop_limit);

/** Validate an incoming IPv6 packet.
 *  Strips the IPv6 header on success.
 *  Returns next header protocol (IPPROTO_*), or -1 on validation failure. */
int ipv6_validate_and_strip(struct rte_mbuf *m,
                              const uint8_t *local_ip6);

/** Worker input path for IPv6 frames.
 *  Returns an mbuf to TX if an immediate reply is needed, or NULL. */
struct rte_mbuf *ipv6_input(uint32_t worker_idx, struct rte_mbuf *m);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_IPV6_H */
