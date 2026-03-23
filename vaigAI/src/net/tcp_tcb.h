/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP Transmission Control Block (§2.5, RFC 793/7323).
 */
#ifndef TGEN_TCP_TCB_H
#define TGEN_TCP_TCB_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
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
    uint16_t    port_id;         /* DPDK egress port for this connection */
    bool        active_open;     /* we initiated the connection */

    /* Cached destination MAC (resolved once, reused per segment) */
    struct rte_ether_addr dst_mac;
    bool        dst_mac_valid;

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

    /* TLS handshake timing (TSC at handshake start) */
    uint64_t    tls_hs_start_tsc;

    /* HTTP request-response latency (TSC at request send) */
    uint64_t    http_req_sent_tsc;

    /* HTTP response body tracking for proper active close.
     * http_content_length is parsed from the Content-Length header.
     * 0 means unknown (chunked / connection-close / not yet parsed).
     * http_body_rx accumulates body bytes received across segments.
     * app_state 6 = waiting for remaining body data after headers parsed.
     * app_state 7 = think-time wait (timer transitions to 4). */
    uint32_t    http_content_length;
    uint32_t    http_body_rx;
    uint32_t    http_txn_count;       /* completed HTTP transactions */
    uint64_t    think_deadline_tsc;   /* TSC deadline for think-time wait */

    /* Server streaming state (chunked HTTP response pump).
     * app_state 12 = streaming; srv_stream_total > 0 means active. */
    uint32_t    srv_stream_total;     /* total body bytes to stream */
    uint32_t    srv_stream_sent;      /* body bytes sent so far */

    /* When true, use graceful FIN close instead of RST at end of
     * transaction (--one flag).  Set by tx_gen when max_initiations > 0. */
    bool        graceful_close;

    /* TCP send buffer (lazily allocated for retransmission + queuing).
     * NULL when no buffered data; allocated on first tcp_fsm_send(). */
    struct tcp_snd_buf_s *snd_buf;

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
    /* free-index stack: O(1) alloc/free instead of linear scan */
    uint32_t   *free_stack;     /* indices of unused TCB slots */
    uint32_t    free_top;       /* next pop position (grows downward) */
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
