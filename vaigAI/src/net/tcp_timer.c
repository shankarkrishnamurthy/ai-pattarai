/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP timer wheel — O(1) insert/cancel, O(expired) per tick.
 */
#include "tcp_timer.h"
#include "tcp_fsm.h"
#include "tcp_port_pool.h"
#include "../core/core_assign.h"
#include "../core/tx_gen.h"
#include "../telemetry/metrics.h"
#include "../tls/tls_session.h"
#include "../app/server.h"

#include <rte_cycles.h>
#include <rte_log.h>
#include <rte_tcp.h>
#include <string.h>

tcp_timer_wheel_t g_timer_wheels[TGEN_MAX_WORKERS];

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline uint32_t tcb_index(uint32_t worker_idx, const tcb_t *tcb)
{
    return (uint32_t)(tcb - g_tcb_stores[worker_idx].tcbs);
}

/* Remove TCB from its current wheel slot (O(1)).  No-op if unscheduled. */
static inline void wheel_remove(uint32_t worker_idx, tcb_t *tcb)
{
    tcp_timer_wheel_t *w = &g_timer_wheels[worker_idx];
    tcb_store_t *store = &g_tcb_stores[worker_idx];
    uint32_t slot = tcb->tw_slot;

    if (slot == TIMER_SLOT_NONE)
        return;

    /* Update prev link */
    if (tcb->tw_prev == UINT32_MAX) {
        /* We are the head of this slot/overflow chain */
        if (slot == TIMER_WHEEL_SLOTS)
            w->overflow = tcb->tw_next;
        else
            w->slots[slot] = tcb->tw_next;
    } else {
        store->tcbs[tcb->tw_prev].tw_next = tcb->tw_next;
    }

    /* Update next link */
    if (tcb->tw_next != UINT32_MAX)
        store->tcbs[tcb->tw_next].tw_prev = tcb->tw_prev;

    tcb->tw_slot = TIMER_SLOT_NONE;
    tcb->tw_next = UINT32_MAX;
    tcb->tw_prev = UINT32_MAX;
}

/* Insert TCB at the head of a wheel slot chain (O(1)). */
static inline void wheel_insert_slot(uint32_t worker_idx, tcb_t *tcb,
                                     uint32_t slot)
{
    tcp_timer_wheel_t *w = &g_timer_wheels[worker_idx];
    tcb_store_t *store = &g_tcb_stores[worker_idx];
    uint32_t ci = tcb_index(worker_idx, tcb);
    uint32_t *head;

    if (slot == TIMER_WHEEL_SLOTS)
        head = &w->overflow;
    else
        head = &w->slots[slot];

    tcb->tw_prev = UINT32_MAX;   /* we become the head */
    tcb->tw_next = *head;
    if (*head != UINT32_MAX)
        store->tcbs[*head].tw_prev = ci;
    *head = ci;
    tcb->tw_slot = slot;
}

/* ── Compute earliest active deadline on a TCB ──────────────────────────── */
static uint64_t tcb_earliest_deadline(const tcb_t *tcb)
{
    uint64_t earliest = UINT64_MAX;

    if (tcb->rto_deadline_tsc && tcb->rto_deadline_tsc < earliest)
        earliest = tcb->rto_deadline_tsc;

    if (tcb->timewait_deadline_tsc && tcb->timewait_deadline_tsc < earliest)
        earliest = tcb->timewait_deadline_tsc;

    if (tcb->think_deadline_tsc && tcb->think_deadline_tsc < earliest)
        earliest = tcb->think_deadline_tsc;

    if (tcb->http_req_sent_tsc && tcb->http_req_sent_tsc < earliest) {
        /* HTTP response timeout: fire at req_sent + 5s */
        uint64_t http_dl = tcb->http_req_sent_tsc +
                           TCP_HTTP_RSP_TIMEOUT_US * (rte_get_tsc_hz() / 1000000ULL);
        if (http_dl < earliest)
            earliest = http_dl;
    }

    /* TLS handshake timeout: app_state 1 or 2, fire at creation + 5s */
    if (tcb->state == TCP_ESTABLISHED &&
        (tcb->app_state == 1 || tcb->app_state == 2) &&
        tcb->timewait_deadline_tsc) {
        uint64_t tls_dl = tcb->timewait_deadline_tsc +
                          TCP_TLS_HS_TIMEOUT_S * rte_get_tsc_hz();
        if (tls_dl < earliest)
            earliest = tls_dl;
    }

    /* Streaming pump: needs per-tick fire → schedule 1 slot ahead */
    if (tcb->state == TCP_ESTABLISHED && tcb->app_state == 12) {
        uint64_t now = rte_rdtsc();
        if (now < earliest)
            earliest = now;
    }

    return earliest;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int tcp_timer_init(void)
{
    uint64_t tsc_hz = rte_get_tsc_hz();
    for (uint32_t w = 0; w < TGEN_MAX_WORKERS; w++) {
        tcp_timer_wheel_t *tw = &g_timer_wheels[w];
        memset(tw, 0, sizeof(*tw));
        for (uint32_t s = 0; s < TIMER_WHEEL_SLOTS; s++)
            tw->slots[s] = UINT32_MAX;
        tw->overflow   = UINT32_MAX;
        tw->dack_head  = UINT32_MAX;
        tw->current    = 0;
        tw->tsc_per_slot = tsc_hz / 1000;  /* 1 ms per slot */
        tw->last_tick_tsc = rte_rdtsc();
    }
    return 0;
}

void tcp_timer_schedule(uint32_t worker_idx, tcb_t *tcb, uint64_t deadline_tsc)
{
    tcp_timer_wheel_t *w = &g_timer_wheels[worker_idx];

    /* Remove from current position if already scheduled */
    if (tcb->tw_slot != TIMER_SLOT_NONE)
        wheel_remove(worker_idx, tcb);

    /* How many slots into the future? */
    uint64_t now = rte_rdtsc();
    uint64_t delta_tsc = (deadline_tsc > now) ? (deadline_tsc - now) : 0;
    uint32_t slots_ahead = (uint32_t)(delta_tsc / w->tsc_per_slot);

    if (slots_ahead >= TIMER_WHEEL_SLOTS) {
        /* Beyond wheel range → overflow list */
        wheel_insert_slot(worker_idx, tcb, TIMER_WHEEL_SLOTS);
    } else {
        uint32_t target = (w->current + slots_ahead) & TIMER_WHEEL_MASK;
        wheel_insert_slot(worker_idx, tcb, target);
    }
}

void tcp_timer_cancel(uint32_t worker_idx, tcb_t *tcb)
{
    wheel_remove(worker_idx, tcb);
}

void tcp_timer_resched(uint32_t worker_idx, tcb_t *tcb)
{
    uint64_t dl = tcb_earliest_deadline(tcb);
    if (dl == UINT64_MAX) {
        /* No active timers — remove from wheel */
        if (tcb->tw_slot != TIMER_SLOT_NONE)
            wheel_remove(worker_idx, tcb);
    } else {
        tcp_timer_schedule(worker_idx, tcb, dl);
    }
}

void tcp_timer_dack_add(uint32_t worker_idx, tcb_t *tcb)
{
    if (tcb->in_dack_list)
        return;
    tcp_timer_wheel_t *w = &g_timer_wheels[worker_idx];
    uint32_t ci = tcb_index(worker_idx, tcb);
    tcb->dack_next = w->dack_head;
    w->dack_head = ci;
    tcb->in_dack_list = true;
}

/* ── Process a single fired TCB ──────────────────────────────────────────── */
static void timer_fire(uint32_t worker_idx, tcb_t *tcb)
{
    if (!tcb->in_use)
        return;

    uint64_t now = rte_rdtsc();
    uint32_t ci = tcb_index(worker_idx, tcb);

    switch (tcb->state) {
    case TCP_TIME_WAIT:
        if (tcb->timewait_deadline_tsc && now >= tcb->timewait_deadline_tsc) {
            if (tcb->app_state >= 2) {
                tls_session_detach(worker_idx, ci);
                tcb->app_state = 0;
            }
            tcp_port_free(worker_idx, tcb->src_ip, tcb->src_port);
            tcb_free(&g_tcb_stores[worker_idx], tcb);
        }
        break;

    case TCP_FIN_WAIT_2:
        if (tcb->timewait_deadline_tsc && now >= tcb->timewait_deadline_tsc) {
            if (tcb->app_state >= 2) {
                tls_session_detach(worker_idx, ci);
                tcb->app_state = 0;
            }
            worker_metrics_add_tcp_conn_close(worker_idx);
            tcp_port_free(worker_idx, tcb->src_ip, tcb->src_port);
            tcb_free(&g_tcb_stores[worker_idx], tcb);
        }
        break;

    case TCP_SYN_SENT:
    case TCP_SYN_RECEIVED:
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
        /* RTO expiry */
        if (tcb->rto_deadline_tsc && now >= tcb->rto_deadline_tsc)
            tcp_fsm_rto_expired(worker_idx, tcb);

        /* Guard: RTO handler may have freed the TCB */
        if (!tcb->in_use)
            break;

        /* TLS handshake timeout */
        if (tcb->state == TCP_ESTABLISHED &&
            (tcb->app_state == 1 || tcb->app_state == 2)) {
            uint64_t age_s = (now - tcb->timewait_deadline_tsc) /
                             rte_get_tsc_hz();
            if (age_s >= TCP_TLS_HS_TIMEOUT_S) {
                tls_session_detach(worker_idx, ci);
                tcb->app_state = 0;
                tcp_fsm_reset(worker_idx, tcb);
                break;
            }
        }

        /* Think-time wait */
        if (tcb->state == TCP_ESTABLISHED &&
            tcb->app_state == 7 &&
            tcb->think_deadline_tsc != 0 &&
            now >= tcb->think_deadline_tsc) {
            tcb->think_deadline_tsc = 0;
            tcp_fsm_http_send_next(worker_idx, tcb);
        }
        if (!tcb->in_use)
            break;

        /* Streaming pump */
        if (tcb->state == TCP_ESTABLISHED && tcb->app_state == 12)
            srv_stream_pump(worker_idx, tcb);
        if (!tcb->in_use)
            break;

        /* HTTP response timeout */
        if (tcb->state == TCP_ESTABLISHED &&
            tcb->app_state == 5 &&
            tcb->http_req_sent_tsc != 0 &&
            (now - tcb->http_req_sent_tsc) >=
                TCP_HTTP_RSP_TIMEOUT_US * (rte_get_tsc_hz() / 1000000ULL)) {
            if (tcb->app_state >= 2)
                tls_session_detach(worker_idx, ci);
            tcb->app_state = 0;
            tcp_fsm_reset(worker_idx, tcb);
        }
        break;

    default:
        break;
    }
}

/* ── Main tick — advance wheel and fire expired timers ───────────────────── */
void tcp_timer_tick(uint32_t worker_idx)
{
    tcp_timer_wheel_t *w = &g_timer_wheels[worker_idx];
    tcb_store_t *store = &g_tcb_stores[worker_idx];
    uint64_t now = rte_rdtsc();

    /* How many slots to advance? */
    uint64_t elapsed = now - w->last_tick_tsc;
    uint32_t slots_to_advance = (uint32_t)(elapsed / w->tsc_per_slot);
    if (slots_to_advance == 0)
        return;

    /* Cap to one full revolution to avoid infinite loop on long stalls */
    if (slots_to_advance > TIMER_WHEEL_SLOTS)
        slots_to_advance = TIMER_WHEEL_SLOTS;

    w->last_tick_tsc += (uint64_t)slots_to_advance * w->tsc_per_slot;

    /* Process each slot we're advancing past */
    for (uint32_t s = 0; s < slots_to_advance; s++) {
        w->current = (w->current + 1) & TIMER_WHEEL_MASK;
        uint32_t idx = w->slots[w->current];
        w->slots[w->current] = UINT32_MAX; /* detach chain */

        while (idx != UINT32_MAX) {
            tcb_t *tcb = &store->tcbs[idx];
            uint32_t next = tcb->tw_next;

            /* Unlink from chain before firing (fire may free TCB) */
            tcb->tw_slot = TIMER_SLOT_NONE;
            tcb->tw_next = UINT32_MAX;
            tcb->tw_prev = UINT32_MAX;

            timer_fire(worker_idx, tcb);

            /* If TCB still alive and has remaining timers, reschedule */
            if (tcb->in_use)
                tcp_timer_resched(worker_idx, tcb);

            idx = next;
        }
    }

    /* On full revolution (current == 0), drain overflow into the wheel */
    if (w->current < slots_to_advance) {
        uint32_t idx = w->overflow;
        w->overflow = UINT32_MAX;

        while (idx != UINT32_MAX) {
            tcb_t *tcb = &store->tcbs[idx];
            uint32_t next = tcb->tw_next;

            tcb->tw_slot = TIMER_SLOT_NONE;
            tcb->tw_next = UINT32_MAX;
            tcb->tw_prev = UINT32_MAX;

            if (tcb->in_use)
                tcp_timer_resched(worker_idx, tcb);

            idx = next;
        }
    }

    /* ── Flush delayed ACKs ──────────────────────────────────────────────── */
    uint32_t dack_idx = w->dack_head;
    uint32_t new_head = UINT32_MAX;
    uint32_t *new_tail = &new_head;

    while (dack_idx != UINT32_MAX) {
        tcb_t *tcb = &store->tcbs[dack_idx];
        uint32_t next = tcb->dack_next;
        tcb->in_dack_list = false;
        tcb->dack_next = UINT32_MAX;

        if (tcb->in_use && tcb->pending_ack) {
            if (now >= tcb->delayed_ack_tsc) {
                tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                                  NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
                tcb->pending_ack = false;
            } else {
                /* Not yet due — keep in list */
                *new_tail = dack_idx;
                new_tail = &tcb->dack_next;
                tcb->in_dack_list = true;
            }
        }
        dack_idx = next;
    }
    w->dack_head = new_head;
}
