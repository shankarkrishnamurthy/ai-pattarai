/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: ICMP module (§2.4, RFC 792).
 */
#ifndef TGEN_ICMP_H
#define TGEN_ICMP_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ICMP_RATE_LIMIT 1000  /* unreachable/time-exceeded per second */

/** Worker: forward ICMP frames to management ring. */
void icmp_input(uint32_t worker_idx, struct rte_mbuf *m);

/** Management: process one ICMP frame; may generate a reply. */
void icmp_mgmt_process(uint16_t port_id, struct rte_mbuf *m);

/** Management: send ICMP Destination Unreachable (code 2 or 3). */
int icmp_send_unreachable(uint16_t port_id, uint8_t code,
                           struct rte_mbuf *orig_m);

/** Management: send ICMP Time Exceeded (type 11, code 0). */
int icmp_send_time_exceeded(uint16_t port_id, struct rte_mbuf *orig_m);

/** Management: ping client — sends Echo Requests; returns sent count. */
int icmp_ping_start(uint16_t port_id, uint32_t dst_ip_net,
                    uint32_t count, uint32_t size, uint32_t interval_ms);

/** Management: drain ICMP ring; returns one mbuf reply or NULL. */
struct rte_mbuf *icmp_mgmt_drain(uint16_t port_id);

/** Init ICMP rings. */
int icmp_init(void);

/** Destroy. */
void icmp_destroy(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_ICMP_H */
