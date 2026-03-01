/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP New Reno congestion control (RFC 5681).
 */
#include "tcp_congestion.h"
#include "tcp_fsm.h"
#include <rte_log.h>

#define RTE_LOGTYPE_TGEN_CC RTE_LOGTYPE_USER3

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */
static inline uint32_t
cc_max(uint32_t a, uint32_t b) { return a > b ? a : b; }

static inline uint32_t
cc_min(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* ------------------------------------------------------------------ */
/* congestion_on_ack                                                    */
/* ------------------------------------------------------------------ */
void
congestion_on_ack(tcb_t *tcb, uint32_t acked)
{
    if (acked == 0)
        return;

    /* Reset dup-ACK counter on new ACK. */
    tcb->dup_ack_count = 0;

    if (tcb->in_fast_recovery) {
        /*
         * Fast Recovery: deflate cwnd back to ssthresh once
         * the retransmitted segment is acknowledged.
         */
        tcb->cwnd       = tcb->ssthresh;
        tcb->in_fast_recovery = false;
        RTE_LOG(DEBUG, TGEN_CC, "Fast recovery exit cwnd=%u\n", tcb->cwnd);
        return;
    }

    if (tcb->cwnd < tcb->ssthresh) {
        /* Slow Start: exponential growth */
        tcb->cwnd += cc_min(acked, tcb->mss_remote);
    } else {
        /* Congestion Avoidance: linear growth ~1 MSS per RTT */
        uint32_t inc = (tcb->mss_remote * tcb->mss_remote) / tcb->cwnd;
        if (inc == 0) inc = 1;
        tcb->cwnd += inc;
    }

    /* Cap at reasonable maximum (64 MB) */
    if (tcb->cwnd > (64u << 20))
        tcb->cwnd = 64u << 20;
}

/* ------------------------------------------------------------------ */
/* congestion_fast_retransmit                                           */
/* ------------------------------------------------------------------ */
void
congestion_fast_retransmit(uint32_t worker_idx, tcb_t *tcb)
{
    (void)worker_idx;
    /* RFC 5681 §3.2: set ssthresh and enter Fast Recovery */
    uint32_t flight = tcb->snd_nxt - tcb->snd_una;
    tcb->ssthresh = cc_max(flight / 2, 2u * tcb->mss_remote);
    tcb->cwnd     = tcb->ssthresh + 3u * tcb->mss_remote; /* inflate */
    tcb->in_fast_recovery = true;

    RTE_LOG(DEBUG, TGEN_CC,
            "Fast retransmit lcore=%u tcb=%p ssthresh=%u cwnd=%u\n",
            worker_idx, (void *)tcb, tcb->ssthresh, tcb->cwnd);

    /*
     * TODO: retransmit the oldest unacknowledged segment from TX buffer.
     * Without a TX buffer, we cannot actually retransmit here — just
     * adjust congestion state and rely on new data sends.
     */
}

/* ------------------------------------------------------------------ */
/* congestion_on_rto                                                    */
/* ------------------------------------------------------------------ */
void
congestion_on_rto(tcb_t *tcb)
{
    /*
     * RFC 5681 §3.1: on RTO, reduce ssthresh and reset cwnd to 1 MSS.
     * RFC 6298 §5.5: back off RTO (caller handles doubling).
     */
    uint32_t flight = tcb->snd_nxt - tcb->snd_una;
    tcb->ssthresh = cc_max(flight / 2, 2u * tcb->mss_remote);
    tcb->cwnd     = tcb->mss_remote;
    tcb->in_fast_recovery = false;
    tcb->dup_ack_count    = 0;

    RTE_LOG(DEBUG, TGEN_CC,
            "RTO cwnd reset tcb=%p ssthresh=%u cwnd=%u\n",
            (void *)tcb, tcb->ssthresh, tcb->cwnd);
}
