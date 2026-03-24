/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP timer wheel — O(1) insert/cancel, O(expired) per tick.
 *
 * 4096 slots × 1 ms = ~4 s coverage.  Timers beyond the wheel range
 * go into an overflow list that is re-hashed once per revolution.
 */
#ifndef TGEN_TCP_TIMER_H
#define TGEN_TCP_TIMER_H

#include <stdint.h>
#include "tcp_tcb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Wheel geometry ──────────────────────────────────────────────────────── */
#define TIMER_WHEEL_BITS    12
#define TIMER_WHEEL_SLOTS   (1u << TIMER_WHEEL_BITS)    /* 4096 */
#define TIMER_WHEEL_MASK    (TIMER_WHEEL_SLOTS - 1)
#define TIMER_SLOT_NONE     UINT32_MAX                   /* not scheduled */

/* ── Per-worker timer wheel ──────────────────────────────────────────────── */
typedef struct {
    uint32_t  slots[TIMER_WHEEL_SLOTS]; /* head of doubly-linked TCB chain */
    uint32_t  overflow;                 /* head of overflow chain           */
    uint32_t  current;                  /* current slot index (0..4095)     */
    uint64_t  last_tick_tsc;            /* TSC of last slot advance         */
    uint64_t  tsc_per_slot;             /* TSC ticks per 1-ms slot          */
    /* Delayed ACK list (singly-linked, lazy removal) */
    uint32_t  dack_head;
} tcp_timer_wheel_t;

extern tcp_timer_wheel_t g_timer_wheels[TGEN_MAX_WORKERS];

/* ── Public API ──────────────────────────────────────────────────────────── */

/** Initialise all per-worker timer wheels.  Call after TSC calibration. */
int tcp_timer_init(void);

/** Called once per worker poll iteration.
 *  Advances the wheel and fires expired timers — O(expired). */
void tcp_timer_tick(uint32_t worker_idx);

/** Insert / reschedule a TCB in the timer wheel at @deadline_tsc.
 *  If the TCB is already scheduled, it is removed first (O(1)). */
void tcp_timer_schedule(uint32_t worker_idx, tcb_t *tcb,
                        uint64_t deadline_tsc);

/** Remove a TCB from the timer wheel (O(1)).  Safe to call if unscheduled. */
void tcp_timer_cancel(uint32_t worker_idx, tcb_t *tcb);

/** Recalculate the earliest active deadline on @tcb and reschedule.
 *  Call this after arming or disarming any timer field on the TCB. */
void tcp_timer_resched(uint32_t worker_idx, tcb_t *tcb);

/** Add a TCB to the delayed-ACK list (if not already on it). */
void tcp_timer_dack_add(uint32_t worker_idx, tcb_t *tcb);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TCP_TIMER_H */
