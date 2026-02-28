/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: UDP module (§2.5, RFC 768).
 */
#ifndef TGEN_UDP_H
#define TGEN_UDP_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UDP_HDR_LEN  8  /* standard UDP header size */

/** Worker: forward UDP frames to management ring for processing. */
void udp_input(uint32_t worker_idx, struct rte_mbuf *m);

/** Management: process one UDP frame (validates checksum). */
void udp_mgmt_process(uint16_t port_id, struct rte_mbuf *m);

/** Management: drain UDP ring; returns one mbuf or NULL. */
struct rte_mbuf *udp_mgmt_drain(uint16_t port_id);

/** Init UDP rings. */
int udp_init(void);

/** Destroy UDP rings. */
void udp_destroy(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_UDP_H */
