/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP FSM (§3.2, RFC 793).
 */
#ifndef TGEN_TCP_FSM_H
#define TGEN_TCP_FSM_H

#include <stdint.h>
#include <rte_mbuf.h>
#include "tcp_tcb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SYN_QUEUE_SZ       1024
#define TCP_DELAYED_ACK_US     40000   /* 40 ms */
#define TCP_MAX_RETRANSMITS    15
#define TCP_INITIAL_RTO_US     200000  /* 200 ms — matches post-measurement minimum */
#define TCP_MAX_RTO_US         60000000 /* 60 s */
#define TCP_TLS_HS_TIMEOUT_S   5       /* TLS handshake timeout (seconds) */
#define TCP_HTTP_RSP_TIMEOUT_US 5000000 /* 5 s — wait for full response body */

/** Worker: receive and dispatch an incoming TCP segment.
 *  m's data pointer should be at start of TCP header. */
void tcp_fsm_input(uint32_t worker_idx, struct rte_mbuf *m);

/** Worker: open an active connection (client side). */
tcb_t *tcp_fsm_connect(uint32_t worker_idx,
                         uint32_t src_ip, uint16_t src_port,
                         uint32_t dst_ip, uint16_t dst_port,
                         uint16_t port_id);

/** Worker: initiate a passive-open listener on a port. */
int tcp_fsm_listen(uint32_t worker_idx, uint16_t local_port);

/** Worker: send data on an established connection. */
int tcp_fsm_send(uint32_t worker_idx, tcb_t *tcb,
                  const uint8_t *data, uint32_t len);

/** Worker: close a connection (active close). */
int tcp_fsm_close(uint32_t worker_idx, tcb_t *tcb);

/** Worker: send RST for a TCB; free it immediately. */
void tcp_fsm_reset(uint32_t worker_idx, tcb_t *tcb);

/** Worker: RST all active connections and reset the TCB store + port pool. */
void tcp_fsm_reset_all(uint32_t worker_idx);

/** Called from timer wheel to handle RTO expiry. */
void tcp_fsm_rto_expired(uint32_t worker_idx, tcb_t *tcb);

/** Called once per poll iteration to flush delayed ACKs. */
void tcp_fsm_flush_delayed_acks(uint32_t worker_idx);

/** Flush the per-worker TCP TX batch buffer via rte_eth_tx_burst.
 *  Called from the worker loop after RX and TX-gen phases to amortize
 *  the per-packet sendto() overhead of AF_PACKET/TAP PMDs. */
void tcp_tx_flush(uint32_t worker_idx);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TCP_FSM_H */
