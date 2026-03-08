/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP timer wheel implementation.
 */
#include "tcp_timer.h"
#include "tcp_fsm.h"
#include "tcp_port_pool.h"
#include "../core/core_assign.h"
#include "../telemetry/metrics.h"
#include "../tls/tls_session.h"

#include <rte_cycles.h>
#include <rte_log.h>

int tcp_timer_init(void)
{
    /* No state beyond the TCB store required */
    return 0;
}

void tcp_timer_tick(uint32_t worker_idx)
{
    /* Throttle timer scan to ~1ms intervals — sufficient for 200ms+ RTOs.
     * This avoids scanning all TCBs on every poll iteration (O(capacity)). */
    static uint64_t last_tick_tsc[TGEN_MAX_WORKERS];
    uint64_t now = rte_rdtsc();
    if (now - last_tick_tsc[worker_idx] < rte_get_tsc_hz() / 1000)
        return;
    last_tick_tsc[worker_idx] = now;

    tcb_store_t *store = &g_tcb_stores[worker_idx];

    for (uint32_t i = 0; i < store->capacity; i++) {
        tcb_t *tcb = &store->tcbs[i];
        if (!tcb->in_use) continue;

        switch (tcb->state) {
        /* ── TIME_WAIT expiry ─────────────────────────────────────────────── */
        case TCP_TIME_WAIT:
            if (now >= tcb->timewait_deadline_tsc) {
                /* Detach TLS session if present */
                if (tcb->app_state >= 2) {
                    tls_session_detach(worker_idx, i);
                    tcb->app_state = 0;
                }
                /* Return ephemeral port */
                tcp_port_free(worker_idx, tcb->src_ip, tcb->src_port);
                tcb_free(store, tcb);
                /* conn_close already counted on FIN_WAIT→TIME_WAIT transition */
            }
            break;

        /* ── RTO expiry ───────────────────────────────────────────────────── */
        case TCP_SYN_SENT:
        case TCP_SYN_RECEIVED:
        case TCP_ESTABLISHED:
        case TCP_FIN_WAIT_1:
        case TCP_CLOSING:
        case TCP_LAST_ACK:
            if (tcb->rto_deadline_tsc &&
                now >= tcb->rto_deadline_tsc) {
                tcp_fsm_rto_expired(worker_idx, tcb);
            }
            /* TLS handshake timeout: close connections stuck in TLS handshake.
             * app_state 1 = TLS requested, 2 = handshaking.
             * timewait_deadline_tsc = connection creation TSC. */
            if (tcb->state == TCP_ESTABLISHED &&
                (tcb->app_state == 1 || tcb->app_state == 2)) {
                uint64_t age_s = (now - tcb->timewait_deadline_tsc) /
                                 rte_get_tsc_hz();
                if (age_s >= TCP_TLS_HS_TIMEOUT_S) {
                    tls_session_detach(worker_idx, i);
                    tcb->app_state = 0;
                    tcp_fsm_reset(worker_idx, tcb);
                }
            }
            /* HTTP response timeout: abort connections that sent a request
             * (app_state == 5) but haven't received a response in time.
             * Frees the TCB slot for new connections. */
            if (tcb->state == TCP_ESTABLISHED &&
                tcb->app_state == 5 &&
                tcb->http_req_sent_tsc != 0 &&
                (now - tcb->http_req_sent_tsc) >=
                    TCP_HTTP_RSP_TIMEOUT_US * (rte_get_tsc_hz() / 1000000ULL)) {
                if (tcb->app_state >= 2) {
                    tls_session_detach(worker_idx, i);
                }
                tcb->app_state = 0;
                tcp_fsm_reset(worker_idx, tcb);
            }
            break;

        /* ── FIN_WAIT_2 idle timeout (RFC 9293 §3.6) ──────────────────── */
        case TCP_FIN_WAIT_2:
            if (tcb->timewait_deadline_tsc &&
                now >= tcb->timewait_deadline_tsc) {
                if (tcb->app_state >= 2) {
                    tls_session_detach(worker_idx, i);
                    tcb->app_state = 0;
                }
                tcp_port_free(worker_idx, tcb->src_ip, tcb->src_port);
                tcb_free(store, tcb);
            }
            break;

        default:
            break;
        }
    }

    /* Flush delayed ACKs once per tick */
    tcp_fsm_flush_delayed_acks(worker_idx);
}
