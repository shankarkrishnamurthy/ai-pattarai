/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP Transmission Control Block (§2.5, RFC 793/7323).
 */
#ifndef TGEN_TCP_TCB_H
#define TGEN_TCP_TCB_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── TCP state machine ────────────────────────────────────────────────────── */
typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

/* ── SACK block ───────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t left;
    uint32_t right;
} sack_block_t;

/* ── Out-of-order segment ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t seq;
    struct rte_mbuf *m;
} ooo_seg_t;

/* ── Transmission Control Block ───────────────────────────────────────────── */
typedef struct {
    /* 4-tuple */
    uint32_t    src_ip;
    uint32_t    dst_ip;
    uint16_t    src_port;
    uint16_t    dst_port;

    /* Send state */
    uint32_t    snd_una;
    uint32_t    snd_nxt;
    uint32_t    snd_wnd;

    /* Receive state */
    uint32_t    rcv_nxt;
    uint32_t    rcv_wnd;

    /* Congestion control */
    uint32_t    cwnd;
    uint32_t    ssthresh;
    uint8_t     dup_ack_count;
    bool        in_fast_recovery;

    /* Retransmission */
    uint64_t    rto_deadline_tsc;
    uint32_t    srtt_us;
    uint32_t    rttvar_us;
    uint32_t    rto_us;         /* current RTO in microseconds */
    uint8_t     retransmit_count;

    /* TCP timestamps (RFC 7323) */
    uint32_t    ts_val;
    uint32_t    ts_ecr;

    /* Window scale */
    uint8_t     wscale_local;
    uint8_t     wscale_remote;

    /* Options negotiated */
    bool        sack_enabled;
    bool        ts_enabled;
    bool        nagle_enabled;
    uint16_t    mss_local;
    uint16_t    mss_remote;

    /* State */
    tcp_state_t state;
    uint8_t     lcore_id;
    bool        active_open;     /* we initiated the connection */

    /* Out-of-order queue */
    ooo_seg_t   ooo[TGEN_OOO_QUEUE_SZ];
    uint8_t     ooo_count;

    /* SACK blocks (max 4 per RFC) */
    sack_block_t sack_blocks[4];
    uint8_t      sack_block_count;

    /* Delayed ACK */
    uint64_t    delayed_ack_tsc;
    bool        pending_ack;
    uint32_t    pending_ack_seq;

    /* TIME_WAIT */
    uint64_t    timewait_deadline_tsc;

    /* L7 layer state (8 bytes for app-level opaque data) */
    uint64_t    app_state;
    void       *app_ctx;     /* pointer to L7 context (HTTP, TLS, etc.) */

    /* Valid flag */
    bool        in_use;
} tcb_t;

/* ── Per-worker TCB store ─────────────────────────────────────────────────── */
#define TCB_HASH_BITS   20
#define TCB_HASH_SIZE   (1 << TCB_HASH_BITS)
#define TCB_HASH_MASK   (TCB_HASH_SIZE - 1)

typedef struct {
    tcb_t      *tcbs;           /* pre-allocated array */
    uint32_t    capacity;
    uint32_t    count;
    /* open-addressing hash table: key = 4-tuple hash, value = tcb index */
    int32_t    *ht;             /* -1 = empty */
    uint32_t    ht_size;
    uint32_t    ht_mask;
} tcb_store_t;

/** Initialise per-worker TCB store.  capacity = max_connections_per_core. */
int tcb_store_init(tcb_store_t *store, uint32_t capacity, int socket_id);

/** Allocate a new TCB; returns NULL on OOM. */
tcb_t *tcb_alloc(tcb_store_t *store,
                  uint32_t src_ip, uint16_t src_port,
                  uint32_t dst_ip, uint16_t dst_port);

/** Look up a TCB by 4-tuple; returns NULL if not found. */
tcb_t *tcb_lookup(tcb_store_t *store,
                   uint32_t src_ip, uint16_t src_port,
                   uint32_t dst_ip, uint16_t dst_port);

/** Free a TCB back to the store. */
void tcb_free(tcb_store_t *store, tcb_t *tcb);

/** Reset all TCBs in the store (free all connections). */
void tcb_store_reset(tcb_store_t *store);

/** Per-worker array of TCB stores (indexed by worker_idx). */
extern tcb_store_t g_tcb_stores[TGEN_MAX_WORKERS];

/** Init all per-worker TCB stores. */
int tcb_stores_init(uint32_t max_connections_per_core);

/** Destroy all TCB stores. */
void tcb_stores_destroy(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TCP_TCB_H */
