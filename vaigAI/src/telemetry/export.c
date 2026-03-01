/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Metrics export implementations.
 */
#include "export.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* JSON export                                                           */
/* ------------------------------------------------------------------ */
int
export_json(const metrics_snapshot_t *snap, char *buf, size_t len)
{
    const worker_metrics_t *t = &snap->total;
    int n = snprintf(buf, len,
        "{\n"
        "  \"num_workers\": %u,\n"
        "  \"tx_pkts\": %"PRIu64", \"tx_bytes\": %"PRIu64",\n"
        "  \"rx_pkts\": %"PRIu64", \"rx_bytes\": %"PRIu64",\n"
        "  \"arp_reply_tx\": %"PRIu64", \"arp_request_tx\": %"PRIu64",\n"
        "  \"arp_miss\": %"PRIu64",\n"
        "  \"icmp_echo_tx\": %"PRIu64", \"icmp_bad_cksum\": %"PRIu64",\n"
        "  \"icmp_unreachable_tx\": %"PRIu64",\n"
        "  \"udp_tx\": %"PRIu64", \"udp_rx\": %"PRIu64",\n"
        "  \"udp_bad_cksum\": %"PRIu64",\n"
        "  \"ip_bad_cksum\": %"PRIu64", \"ip_frag_dropped\": %"PRIu64",\n"
        "  \"ip_not_for_us\": %"PRIu64",\n"
        "  \"tcp_conn_open\": %"PRIu64", \"tcp_conn_close\": %"PRIu64",\n"
        "  \"tcp_syn_sent\": %"PRIu64", \"tcp_retransmit\": %"PRIu64",\n"
        "  \"tcp_reset_rx\": %"PRIu64", \"tcp_reset_sent\": %"PRIu64",\n"
        "  \"tcp_bad_cksum\": %"PRIu64",\n"
        "  \"tcp_syn_queue_drops\": %"PRIu64",\n"
        "  \"tcp_duplicate_acks\": %"PRIu64",\n"
        "  \"tcp_ooo_pkts\": %"PRIu64",\n"
        "  \"tcp_payload_tx\": %"PRIu64", \"tcp_payload_rx\": %"PRIu64",\n"
        "  \"http_req_tx\": %"PRIu64", \"http_rsp_rx\": %"PRIu64",\n"
        "  \"http_2xx\": %"PRIu64", \"http_4xx\": %"PRIu64",\n"
        "  \"tls_ok\": %"PRIu64", \"tls_fail\": %"PRIu64"\n"
        "}\n",
        snap->n_workers,
        t->tx_pkts, t->tx_bytes,
        t->rx_pkts, t->rx_bytes,
        t->arp_reply_tx, t->arp_request_tx,
        t->arp_miss,
        t->icmp_echo_tx, t->icmp_bad_cksum,
        t->icmp_unreachable_tx,
        t->udp_tx, t->udp_rx,
        t->udp_bad_cksum,
        t->ip_bad_cksum, t->ip_frag_dropped,
        t->ip_not_for_us,
        t->tcp_conn_open, t->tcp_conn_close,
        t->tcp_syn_sent,  t->tcp_retransmit,
        t->tcp_reset_rx,  t->tcp_reset_sent,
        t->tcp_bad_cksum,
        t->tcp_syn_queue_drops,
        t->tcp_duplicate_acks,
        t->tcp_ooo_pkts,
        t->tcp_payload_tx, t->tcp_payload_rx,
        t->http_req_tx,   t->http_rsp_rx,
        t->http_rsp_2xx,  t->http_rsp_4xx,
        t->tls_handshake_ok, t->tls_handshake_fail);
    return n;
}


