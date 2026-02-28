/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP timer wheel (§3.4, RFC 6298).
 *  1 ms tick resolution driven by TSC delta — no syscalls.
 */
#ifndef TGEN_TCP_TIMER_H
#define TGEN_TCP_TIMER_H

#include <stdint.h>
#include "tcp_tcb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called once per worker poll iteration.
 *  Scans for expired RTOs and TIME_WAIT timeouts. */
void tcp_timer_tick(uint32_t worker_idx);

/** Initialise per-worker timer state. */
int tcp_timer_init(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TCP_TIMER_H */
