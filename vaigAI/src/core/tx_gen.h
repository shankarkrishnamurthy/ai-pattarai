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
    TX_GEN_PROTO_ICMP    = 0,   /* ICMP Echo Request flood              */
    TX_GEN_PROTO_UDP,           /* UDP datagram flood                   */
    TX_GEN_PROTO_TCP_SYN,       /* TCP SYN flood (future)               */
    TX_GEN_PROTO_HTTP,          /* HTTP request flood (future)          */
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
} tx_gen_state_t;

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
