/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP congestion control — New Reno (RFC 5681) + CUBIC (RFC 8312).
 */
#ifndef TGEN_TCP_CONGESTION_H
#define TGEN_TCP_CONGESTION_H

#include <stdint.h>
#include "tcp_tcb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── CC algorithm identifiers ────────────────────────────────────────────── */
#define CC_NEWRENO  0
#define CC_CUBIC    1

/** Called when a new ACK advances snd_una by 'acked' bytes. */
void congestion_on_ack(tcb_t *tcb, uint32_t acked);

/** Called on 3 duplicate ACKs (fast retransmit entry). */
void congestion_fast_retransmit(uint32_t worker_idx, tcb_t *tcb);

/** Called on RTO expiry. */
void congestion_on_rto(tcb_t *tcb);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TCP_CONGESTION_H */
