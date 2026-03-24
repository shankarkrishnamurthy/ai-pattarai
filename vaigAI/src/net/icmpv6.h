/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: ICMPv6 module (RFC 4443, RFC 4861).
 */
#ifndef TGEN_ICMPV6_H
#define TGEN_ICMPV6_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ICMPv6 type codes */
#define ICMPV6_ECHO_REQUEST     128
#define ICMPV6_ECHO_REPLY       129
#define ICMPV6_NEIGHBOR_SOL     135
#define ICMPV6_NEIGHBOR_ADV     136

/* ICMPv6 NDP option types */
#define NDP_OPT_SRC_LLADDR      1
#define NDP_OPT_TGT_LLADDR      2

#define ICMPV6_HDR_LEN          8

/** Worker: forward ICMPv6 frames to management ring. */
void icmpv6_input(uint32_t worker_idx, struct rte_mbuf *m);

/** Management: process one ICMPv6 frame (echo, NS, NA). */
void icmpv6_mgmt_process(uint16_t port_id, struct rte_mbuf *m);

/** Management: periodic tick — drain ICMPv6 rings and process. */
void icmpv6_mgmt_tick(void);

/** Management: drain ICMPv6 ring; returns one mbuf reply or NULL. */
struct rte_mbuf *icmpv6_mgmt_drain(uint16_t port_id);

/** Management: ping6 — sends ICMPv6 Echo Requests; returns rcvd count.
 *  @param poll_fn  Optional callback during wait loops. */
typedef void (*icmpv6_mgmt_poll_fn)(void);
int icmpv6_ping_start(uint16_t port_id, const uint8_t *dst_ip6,
                       uint32_t count, uint32_t size, uint32_t interval_ms,
                       icmpv6_mgmt_poll_fn poll_fn);

/** Compute ICMPv6 checksum (includes IPv6 pseudo-header).
 *  @param src6  Source IPv6 address (16 bytes).
 *  @param dst6  Destination IPv6 address (16 bytes).
 *  @param icmpv6_data  Pointer to the ICMPv6 header+payload.
 *  @param icmpv6_len   Total length of ICMPv6 header+payload.
 *  Returns the 16-bit checksum in network byte order. */
uint16_t icmpv6_checksum(const uint8_t *src6, const uint8_t *dst6,
                          const void *icmpv6_data, uint16_t icmpv6_len);

/** Init ICMPv6 rings. */
int icmpv6_init(void);

/** Destroy. */
void icmpv6_destroy(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_ICMPV6_H */
