/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Per-worker metrics — global storage and snapshot.
 */
#include "metrics.h"
#include <string.h>

/* Global array — one cache-line-aligned slab per worker. */
worker_metrics_t g_metrics[TGEN_MAX_WORKERS];

void
metrics_snapshot(metrics_snapshot_t *snap, uint32_t n_workers)
{
    memset(snap, 0, sizeof(*snap));
    snap->n_workers = n_workers;

    for (uint32_t w = 0; w < n_workers && w < TGEN_MAX_WORKERS; w++) {
        /* Copy worker slab */
        memcpy(&snap->per_worker[w], &g_metrics[w], sizeof(worker_metrics_t));

        /* Accumulate into total */
        worker_metrics_t *t = &snap->total;
        const worker_metrics_t *s = &snap->per_worker[w];

#define ACC(field) t->field += s->field
        ACC(tx_pkts);        ACC(tx_bytes);
        ACC(rx_pkts);        ACC(rx_bytes);
        ACC(ip_bad_cksum);   ACC(ip_frag_dropped); ACC(ip_not_for_us);
        ACC(arp_reply_tx);   ACC(arp_request_tx);  ACC(arp_miss);
        ACC(icmp_echo_tx);   ACC(icmp_bad_cksum);  ACC(icmp_unreachable_tx);
        ACC(tcp_conn_open);  ACC(tcp_conn_close);
        ACC(tcp_syn_sent);   ACC(tcp_retransmit);
        ACC(tcp_reset_rx);   ACC(tcp_reset_sent);
        ACC(tcp_bad_cksum);  ACC(tcp_syn_queue_drops);
        ACC(tcp_ooo_pkts);   ACC(tcp_duplicate_acks);
        ACC(tls_handshake_ok);   ACC(tls_handshake_fail);
        ACC(tls_records_tx); ACC(tls_records_rx);
        ACC(http_req_tx);    ACC(http_rsp_rx);
        ACC(http_rsp_1xx);   ACC(http_rsp_2xx);
        ACC(http_rsp_3xx);   ACC(http_rsp_4xx);   ACC(http_rsp_5xx);
        ACC(http_parse_err);
#undef ACC
    }
}

void
metrics_reset(uint32_t n_workers)
{
    for (uint32_t w = 0; w < n_workers && w < TGEN_MAX_WORKERS; w++)
        memset(&g_metrics[w], 0, sizeof(worker_metrics_t));
}
