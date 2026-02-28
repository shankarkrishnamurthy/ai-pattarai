/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Per-worker lock-free metrics (§6.1).
 *
 * Each worker core increments its own slab of counters with no atomic
 * contention.  The management thread reads all slabs to aggregate.
 */
#ifndef TGEN_METRICS_H
#define TGEN_METRICS_H

#include <stdint.h>
#include "../common/types.h"
#include <rte_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Per-worker counter slab                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    /* L2/L3 TX */
    uint64_t tx_pkts;
    uint64_t tx_bytes;
    /* L2/L3 RX */
    uint64_t rx_pkts;
    uint64_t rx_bytes;

    /* IP */
    uint64_t ip_bad_cksum;
    uint64_t ip_frag_dropped;
    uint64_t ip_not_for_us;

    /* ARP */
    uint64_t arp_reply_tx;
    uint64_t arp_request_tx;
    uint64_t arp_miss;

    /* ICMP */
    uint64_t icmp_echo_tx;
    uint64_t icmp_bad_cksum;
    uint64_t icmp_unreachable_tx;

    /* TCP */
    uint64_t tcp_conn_open;
    uint64_t tcp_conn_close;
    uint64_t tcp_syn_sent;
    uint64_t tcp_retransmit;
    uint64_t tcp_reset_rx;
    uint64_t tcp_reset_sent;
    uint64_t tcp_bad_cksum;
    uint64_t tcp_syn_queue_drops;
    uint64_t tcp_ooo_pkts;
    uint64_t tcp_duplicate_acks;

    /* TLS */
    uint64_t tls_handshake_ok;
    uint64_t tls_handshake_fail;
    uint64_t tls_records_tx;
    uint64_t tls_records_rx;

    /* HTTP */
    uint64_t http_req_tx;
    uint64_t http_rsp_rx;
    uint64_t http_rsp_1xx;
    uint64_t http_rsp_2xx;
    uint64_t http_rsp_3xx;
    uint64_t http_rsp_4xx;
    uint64_t http_rsp_5xx;
    uint64_t http_parse_err;

    /* Padding to a full cache line */
    uint8_t  _pad[RTE_CACHE_LINE_SIZE -
                  (40 * sizeof(uint64_t)) % RTE_CACHE_LINE_SIZE];
} __rte_cache_aligned worker_metrics_t;

/* ------------------------------------------------------------------ */
/* Global array — one slab per worker                                   */
/* ------------------------------------------------------------------ */
extern worker_metrics_t g_metrics[TGEN_MAX_WORKERS];

/* ------------------------------------------------------------------ */
/* Fast increment macros (no atomics — same lcore owns each slab)       */
/* ------------------------------------------------------------------ */
#define WORKER_METRICS(widx) (&g_metrics[(widx)])

#define worker_metrics_add_tx(widx, pkts, bytes) \
    do { g_metrics[(widx)].tx_pkts  += (pkts); \
         g_metrics[(widx)].tx_bytes += (bytes); } while (0)

#define worker_metrics_add_rx(widx, pkts, bytes) \
    do { g_metrics[(widx)].rx_pkts  += (pkts); \
         g_metrics[(widx)].rx_bytes += (bytes); } while (0)

#define worker_metrics_add_ip_bad_cksum(widx)    (g_metrics[(widx)].ip_bad_cksum++)
#define worker_metrics_add_ip_frag_dropped(widx) (g_metrics[(widx)].ip_frag_dropped++)
#define worker_metrics_add_ip_not_for_us(widx)   (g_metrics[(widx)].ip_not_for_us++)

#define worker_metrics_add_arp_reply_tx(widx)    (g_metrics[(widx)].arp_reply_tx++)
#define worker_metrics_add_arp_request_tx(widx)  (g_metrics[(widx)].arp_request_tx++)
#define worker_metrics_add_arp_miss(widx)        (g_metrics[(widx)].arp_miss++)

#define worker_metrics_add_icmp_echo_tx(widx)    (g_metrics[(widx)].icmp_echo_tx++)
#define worker_metrics_add_icmp_bad_cksum(widx)  (g_metrics[(widx)].icmp_bad_cksum++)

#define worker_metrics_add_tcp_conn_open(widx)    (g_metrics[(widx)].tcp_conn_open++)
#define worker_metrics_add_tcp_conn_close(widx)   (g_metrics[(widx)].tcp_conn_close++)
#define worker_metrics_add_tcp_syn_sent(widx)     (g_metrics[(widx)].tcp_syn_sent++)
#define worker_metrics_add_tcp_retransmit(widx)   (g_metrics[(widx)].tcp_retransmit++)
#define worker_metrics_add_tcp_reset_rx(widx)     (g_metrics[(widx)].tcp_reset_rx++)
#define worker_metrics_add_tcp_reset_sent(widx)   (g_metrics[(widx)].tcp_reset_sent++)
#define worker_metrics_add_tcp_bad_cksum(widx)    (g_metrics[(widx)].tcp_bad_cksum++)
#define worker_metrics_add_syn_queue_drops(widx)  (g_metrics[(widx)].tcp_syn_queue_drops++)
#define worker_metrics_add_tcp_ooo(widx)          (g_metrics[(widx)].tcp_ooo_pkts++)
#define worker_metrics_add_tcp_dup_ack(widx)      (g_metrics[(widx)].tcp_duplicate_acks++)

#define worker_metrics_add_tls_ok(widx)           (g_metrics[(widx)].tls_handshake_ok++)
#define worker_metrics_add_tls_fail(widx)         (g_metrics[(widx)].tls_handshake_fail++)
#define worker_metrics_add_tls_tx(widx)           (g_metrics[(widx)].tls_records_tx++)
#define worker_metrics_add_tls_rx(widx)           (g_metrics[(widx)].tls_records_rx++)

#define worker_metrics_add_http_req(widx)         (g_metrics[(widx)].http_req_tx++)
#define worker_metrics_add_http_rsp(widx, code) \
    do { g_metrics[(widx)].http_rsp_rx++;       \
         if      ((code) < 200) g_metrics[(widx)].http_rsp_1xx++; \
         else if ((code) < 300) g_metrics[(widx)].http_rsp_2xx++; \
         else if ((code) < 400) g_metrics[(widx)].http_rsp_3xx++; \
         else if ((code) < 500) g_metrics[(widx)].http_rsp_4xx++; \
         else                   g_metrics[(widx)].http_rsp_5xx++; } while (0)
#define worker_metrics_add_http_parse_err(widx)   (g_metrics[(widx)].http_parse_err++)

/* ------------------------------------------------------------------ */
/* Aggregated snapshot (used by management/export thread)               */
/* ------------------------------------------------------------------ */
typedef struct {
    worker_metrics_t total;
    worker_metrics_t per_worker[TGEN_MAX_WORKERS];
    uint32_t         n_workers;
} metrics_snapshot_t;

/**
 * Snapshot all worker metrics into 'snap'.
 * Reads are racy (no lock) — tolerable for monitoring.
 */
void metrics_snapshot(metrics_snapshot_t *snap, uint32_t n_workers);

/**
 * Reset all worker metrics to zero.
 * Call only from management thread when no workers are sending traffic.
 */
void metrics_reset(uint32_t n_workers);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_METRICS_H */
