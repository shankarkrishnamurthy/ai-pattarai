/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP timer wheel implementation.
 */
#include "tcp_timer.h"
#include "tcp_fsm.h"
#include "tcp_port_pool.h"
#include "../core/core_assign.h"
#include "../telemetry/metrics.h"

#include <rte_cycles.h>
#include <rte_log.h>

int tcp_timer_init(void)
{
    /* No state beyond the TCB store required */
    return 0;
}

void tcp_timer_tick(uint32_t worker_idx)
{
    tcb_store_t *store = &g_tcb_stores[worker_idx];
    uint64_t now = rte_rdtsc();

    for (uint32_t i = 0; i < store->capacity; i++) {
        tcb_t *tcb = &store->tcbs[i];
        if (!tcb->in_use) continue;

        switch (tcb->state) {
        /* ── TIME_WAIT expiry ─────────────────────────────────────────────── */
        case TCP_TIME_WAIT:
            if (now >= tcb->timewait_deadline_tsc) {
                /* Return ephemeral port */
                tcp_port_free(worker_idx, tcb->src_ip, tcb->src_port);
                tcb_free(store, tcb);
                worker_metrics_add_tcp_conn_close(worker_idx);
            }
            break;

        /* ── RTO expiry ───────────────────────────────────────────────────── */
        case TCP_SYN_SENT:
        case TCP_SYN_RECEIVED:
        case TCP_ESTABLISHED:
        case TCP_FIN_WAIT_1:
        case TCP_LAST_ACK:
            if (tcb->rto_deadline_tsc &&
                now >= tcb->rto_deadline_tsc) {
                tcp_fsm_rto_expired(worker_idx, tcb);
            }
            break;

        default:
            break;
        }
    }

    /* Flush delayed ACKs once per tick */
    tcp_fsm_flush_delayed_acks(worker_idx);
}
