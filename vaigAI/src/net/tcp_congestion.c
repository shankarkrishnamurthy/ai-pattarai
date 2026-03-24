/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP congestion control — New Reno (RFC 5681) + CUBIC (RFC 8312).
 */
#include "tcp_congestion.h"
#include "tcp_fsm.h"
#include <rte_log.h>
#include <rte_cycles.h>
#include <math.h>

#define RTE_LOGTYPE_TGEN_CC RTE_LOGTYPE_USER3

/* ── CUBIC constants (RFC 8312 §4) ───────────────────────────────────────── */
#define CUBIC_C     0.4     /* scaling constant */
#define CUBIC_BETA  0.7     /* multiplicative decrease factor */

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */
static inline uint32_t
cc_max(uint32_t a, uint32_t b) { return a > b ? a : b; }

static inline uint32_t
cc_min(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* Approximate cube root using Newton's method (integer, in MSS units) */
static inline uint32_t
cubic_root(uint64_t x)
{
    if (x == 0) return 0;
    /* Start with a rough estimate */
    double d = cbrt((double)x);
    return (uint32_t)(d + 0.5);
}

/* ── New Reno ─────────────────────────────────────────────────────────────── */
static void
newreno_on_ack(tcb_t *tcb, uint32_t acked)
{
    if (tcb->in_fast_recovery) {
        tcb->cwnd = tcb->ssthresh;
        tcb->in_fast_recovery = false;
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

    if (tcb->cwnd > (64u << 20))
        tcb->cwnd = 64u << 20;
}

/* ── CUBIC (RFC 8312) ─────────────────────────────────────────────────────── */
static void
cubic_on_ack(tcb_t *tcb, uint32_t acked)
{
    if (tcb->in_fast_recovery) {
        tcb->cwnd = tcb->ssthresh;
        tcb->in_fast_recovery = false;
        return;
    }

    /* Slow Start phase — use standard exponential growth */
    if (tcb->cwnd < tcb->ssthresh) {
        tcb->cwnd += cc_min(acked, tcb->mss_remote);
        return;
    }

    /* CUBIC congestion avoidance */
    uint64_t now_tsc = rte_rdtsc();
    uint64_t hz = rte_get_tsc_hz();

    /* Initialize epoch on first CA ACK */
    if (tcb->cubic_epoch_start == 0) {
        tcb->cubic_epoch_start = now_tsc;
        if (tcb->cwnd < tcb->cubic_wmax) {
            /* K = cubic_root(W_max - cwnd) / C) in MSS units */
            uint32_t wmax_mss = tcb->cubic_wmax / tcb->mss_remote;
            uint32_t cwnd_mss = tcb->cwnd / tcb->mss_remote;
            uint64_t diff = (wmax_mss > cwnd_mss) ?
                            (wmax_mss - cwnd_mss) : 0;
            /* K in seconds * 1000000 for µs */
            tcb->cubic_k_us = (uint32_t)(cbrt((double)diff / CUBIC_C) * 1e6);
        } else {
            tcb->cubic_k_us = 0;
        }
        tcb->cubic_origin_point = tcb->cubic_wmax / tcb->mss_remote;
    }

    /* t = time since epoch start in µs */
    uint64_t elapsed_us = (now_tsc - tcb->cubic_epoch_start) * 1000000ULL / hz;

    /* W_cubic(t) = C * (t - K)^3 + W_max   (in MSS units) */
    double t_sec = (double)elapsed_us / 1e6;
    double k_sec = (double)tcb->cubic_k_us / 1e6;
    double dt = t_sec - k_sec;
    double w_cubic = CUBIC_C * dt * dt * dt + (double)tcb->cubic_origin_point;

    /* TCP-friendly estimate: W_est = W_max * β + 3*(1-β)/(1+β) * t/RTT */
    uint32_t rtt_us = tcb->srtt_us > 0 ? tcb->srtt_us : 200000;
    double w_est = (double)tcb->cubic_origin_point * CUBIC_BETA +
                   3.0 * (1.0 - CUBIC_BETA) / (1.0 + CUBIC_BETA) *
                   (t_sec / ((double)rtt_us / 1e6));

    /* Use the larger of CUBIC and TCP-friendly window */
    double target = w_cubic > w_est ? w_cubic : w_est;
    uint32_t target_bytes = (uint32_t)(target * tcb->mss_remote);

    if (target_bytes > tcb->cwnd) {
        uint32_t inc = (target_bytes - tcb->cwnd) * tcb->mss_remote / tcb->cwnd;
        if (inc == 0) inc = 1;
        tcb->cwnd += inc;
    } else {
        /* Below target — grow by at least 1 byte per RTT */
        uint32_t inc = tcb->mss_remote * tcb->mss_remote / tcb->cwnd;
        if (inc == 0) inc = 1;
        tcb->cwnd += inc;
    }

    if (tcb->cwnd > (64u << 20))
        tcb->cwnd = 64u << 20;
}

/* ------------------------------------------------------------------ */
/* congestion_on_ack (dispatcher)                                       */
/* ------------------------------------------------------------------ */
void
congestion_on_ack(tcb_t *tcb, uint32_t acked)
{
    if (acked == 0)
        return;

    tcb->dup_ack_count = 0;

    /* Throughput mode: keep cwnd unlimited. */
    if (tcb->app_ctx == (void *)1) {
        tcb->cwnd = UINT32_MAX;
        return;
    }

    if (tcb->cc_algo == CC_CUBIC)
        cubic_on_ack(tcb, acked);
    else
        newreno_on_ack(tcb, acked);
}

/* ------------------------------------------------------------------ */
/* congestion_fast_retransmit                                           */
/* ------------------------------------------------------------------ */
void
congestion_fast_retransmit(uint32_t worker_idx, tcb_t *tcb)
{
    (void)worker_idx;

    if (tcb->app_ctx == (void *)1)
        return;

    uint32_t flight = tcb->snd_nxt - tcb->snd_una;

    if (tcb->cc_algo == CC_CUBIC) {
        /* CUBIC: save W_max and reduce by β */
        tcb->cubic_wmax = tcb->cwnd;
        tcb->cubic_epoch_start = 0; /* reset epoch */
        tcb->ssthresh = cc_max((uint32_t)(tcb->cwnd * CUBIC_BETA),
                               2u * tcb->mss_remote);
        tcb->cwnd = tcb->ssthresh + 3u * tcb->mss_remote;
    } else {
        /* New Reno: RFC 5681 §3.2 */
        tcb->ssthresh = cc_max(flight / 2, 2u * tcb->mss_remote);
        tcb->cwnd = tcb->ssthresh + 3u * tcb->mss_remote;
    }

    tcb->in_fast_recovery = true;

    RTE_LOG(DEBUG, TGEN_CC,
            "Fast retransmit lcore=%u tcb=%p algo=%s ssthresh=%u cwnd=%u\n",
            worker_idx, (void *)tcb,
            tcb->cc_algo == CC_CUBIC ? "CUBIC" : "NewReno",
            tcb->ssthresh, tcb->cwnd);
}

/* ------------------------------------------------------------------ */
/* congestion_on_rto                                                    */
/* ------------------------------------------------------------------ */
void
congestion_on_rto(tcb_t *tcb)
{
    if (tcb->app_ctx == (void *)1)
        return;

    uint32_t flight = tcb->snd_nxt - tcb->snd_una;

    if (tcb->cc_algo == CC_CUBIC) {
        /* CUBIC: save W_max, reset epoch, cwnd = 1 MSS */
        tcb->cubic_wmax = tcb->cwnd;
        tcb->cubic_epoch_start = 0;
        tcb->ssthresh = cc_max((uint32_t)(tcb->cwnd * CUBIC_BETA),
                               2u * tcb->mss_remote);
    } else {
        tcb->ssthresh = cc_max(flight / 2, 2u * tcb->mss_remote);
    }

    tcb->cwnd = tcb->mss_remote;
    tcb->in_fast_recovery = false;
    tcb->dup_ack_count = 0;

    RTE_LOG(DEBUG, TGEN_CC,
            "RTO cwnd reset tcb=%p algo=%s ssthresh=%u cwnd=%u\n",
            (void *)tcb,
            tcb->cc_algo == CC_CUBIC ? "CUBIC" : "NewReno",
            tcb->ssthresh, tcb->cwnd);
}
