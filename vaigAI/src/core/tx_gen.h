/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Timer-based TX packet generator (§1.9).
 *
 * Generic framework for sustained packet generation on worker lcores.
 * Each worker owns a tx_gen_state_t; the management core configures it
 * via IPC (CFG_CMD_START / CFG_CMD_STOP).
 *
 * Protocol-specific packet builders are dispatched by tx_gen_proto_t.
 * Adding a new protocol is a single builder function + an enum entry.
 */
#ifndef TGEN_TX_GEN_H
#define TGEN_TX_GEN_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Protocol selector ────────────────────────────────────────────────────── */
typedef enum {
    TX_GEN_PROTO_ICMP    = 0,   /* ICMP Echo Request TPS                */
    TX_GEN_PROTO_UDP,           /* UDP datagram TPS                     */
    TX_GEN_PROTO_TCP_SYN,       /* TCP SYN TPS                          */
    TX_GEN_PROTO_HTTP,          /* HTTP request TPS                     */
    TX_GEN_PROTO_THROUGHPUT,    /* TCP bulk data throughput              */
    TX_GEN_PROTO_MAX,
} tx_gen_proto_t;

/* ── Configuration (sent from mgmt → worker via IPC payload) ─────────────
 *    Must fit in the 248-byte config_update_t.payload field.            */
typedef struct {
    tx_gen_proto_t        proto;
    uint32_t              dst_ip;       /* network byte order            */
    uint32_t              src_ip;       /* network byte order            */
    struct rte_ether_addr dst_mac;
    struct rte_ether_addr src_mac;
    uint16_t              dst_port;     /* host byte order (UDP/TCP)     */
    uint16_t              src_port;     /* host byte order (UDP/TCP)     */
    uint16_t              pkt_size;     /* protocol payload size (bytes) */
    uint16_t              port_id;      /* DPDK port to transmit on      */
    uint64_t              rate_pps;     /* 0 = unlimited (line rate)     */
    uint32_t              duration_s;   /* 0 = run until stopped         */
    uint32_t              max_initiations; /* 0 = unlimited; >0 = stop after N */
    bool                  enable_tls;   /* initiate TLS after TCP 3WHS   */
    uint8_t               http_method;  /* http_method_t (0=GET,1=POST…) */
    uint8_t               throughput_streams; /* streams for THROUGHPUT (1-16) */
    uint8_t               _pad[1];      /* alignment padding             */
    uint32_t              ramp_s;       /* ramp-up duration (0 = instant) */
    uint32_t              txn_per_conn; /* HTTP txns per conn (0 = 1 shot) */
    uint32_t              think_time_us;/* think time between txns in µs  */
    char                  http_url[64]; /* URL path for HTTP TPS         */
    char                  http_host[64];/* Host: header for HTTP TPS    */
} tx_gen_config_t;

_Static_assert(sizeof(tx_gen_config_t) <= 248,
               "tx_gen_config_t must fit in IPC payload");

/* ── Per-worker generation state ──────────────────────────────────────────── */

typedef struct {
    volatile bool   active;
    tx_gen_config_t cfg;

    /* Timing */
    uint64_t        start_tsc;
    uint64_t        deadline_tsc;       /* 0 = no deadline              */

    /* Token bucket (rate limiting) */
    uint64_t        tokens;
    uint64_t        last_refill_tsc;

    /* Counters */
    uint64_t        pkts_sent;
    uint64_t        pkts_dropped;       /* TX ring full                 */

    /* Per-protocol state */
    uint16_t        seq;                /* ICMP seq / etc.              */
    uint16_t        ident;              /* ICMP identifier              */
    uint16_t        tx_queue_id;        /* resolved at configure time   */
    uint16_t        _pad;

    /* Throughput mode state */
    void           *tp_tcbs[16];        /* active throughput TCBs       */
    uint32_t        tp_n_streams;       /* number of streams allocated  */
    uint8_t         tp_phase;           /* 0=connect, 1=pump, 2=done   */
} tx_gen_state_t;

/* ── Pre-built HTTP request (one per worker, reused across connections) ──── */
typedef struct {
    uint8_t  hdr[512];
    uint32_t hdr_len;
    bool     keep_alive;          /* recycle: state 4→5→4 loop */
    uint32_t txn_per_conn;        /* max transactions per conn (0 = 1 shot) */
    uint32_t think_time_us;       /* inter-transaction think time in µs */
    uint64_t expected_interval_us;/* CO correction: 1M/rate_pps (0 = disabled) */
} http_prebuilt_req_t;

extern http_prebuilt_req_t g_http_req[TGEN_MAX_WORKERS];

/* ── API ──────────────────────────────────────────────────────────────────── */

/** Load configuration into the generator (does NOT start it).
 *  @param tx_queue  TX queue this worker owns on the target port. */
void tx_gen_configure(tx_gen_state_t *state, const tx_gen_config_t *cfg,
                      uint16_t tx_queue);

/** Arm the generator — starts the clock and token bucket. */
void tx_gen_start(tx_gen_state_t *state);

/** Disarm the generator — stops packet production immediately. */
void tx_gen_stop(tx_gen_state_t *state);

/** Generate and transmit a burst of packets (called from worker loop).
 *  Returns number of packets successfully transmitted. */
uint32_t tx_gen_burst(tx_gen_state_t *state, struct rte_mempool *mp,
                      uint32_t worker_idx);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TX_GEN_H */
