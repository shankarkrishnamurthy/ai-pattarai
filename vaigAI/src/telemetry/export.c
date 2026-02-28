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
        t->http_req_tx,   t->http_rsp_rx,
        t->http_rsp_2xx,  t->http_rsp_4xx,
        t->tls_handshake_ok, t->tls_handshake_fail);
    return n;
}

/* ------------------------------------------------------------------ */
/* Prometheus text export                                               */
/* ------------------------------------------------------------------ */
#define PROM_GAUGE(buf, len, pos, name, val) \
    do { int _r = snprintf((buf)+(pos), (len)-(pos), \
             "# HELP vaigai_" name " vaigAI traffic generator counter\n" \
             "# TYPE vaigai_" name " gauge\n" \
             "vaigai_" name " %"PRIu64"\n", (uint64_t)(val)); \
         if (_r > 0) pos += _r; } while (0)

int
export_prometheus(const metrics_snapshot_t *snap, char *buf, size_t len)
{
    const worker_metrics_t *t = &snap->total;
    int pos = 0;

    PROM_GAUGE(buf, len, pos, "tx_pkts",          t->tx_pkts);
    PROM_GAUGE(buf, len, pos, "tx_bytes",         t->tx_bytes);
    PROM_GAUGE(buf, len, pos, "rx_pkts",          t->rx_pkts);
    PROM_GAUGE(buf, len, pos, "rx_bytes",         t->rx_bytes);
    PROM_GAUGE(buf, len, pos, "udp_tx",           t->udp_tx);
    PROM_GAUGE(buf, len, pos, "udp_rx",           t->udp_rx);
    PROM_GAUGE(buf, len, pos, "udp_bad_cksum",    t->udp_bad_cksum);
    PROM_GAUGE(buf, len, pos, "tcp_conn_open",    t->tcp_conn_open);
    PROM_GAUGE(buf, len, pos, "tcp_conn_close",   t->tcp_conn_close);
    PROM_GAUGE(buf, len, pos, "tcp_syn_sent",     t->tcp_syn_sent);
    PROM_GAUGE(buf, len, pos, "tcp_retransmit",   t->tcp_retransmit);
    PROM_GAUGE(buf, len, pos, "tcp_reset_rx",     t->tcp_reset_rx);
    PROM_GAUGE(buf, len, pos, "tcp_reset_sent",   t->tcp_reset_sent);
    PROM_GAUGE(buf, len, pos, "http_req_tx",      t->http_req_tx);
    PROM_GAUGE(buf, len, pos, "http_rsp_2xx",     t->http_rsp_2xx);
    PROM_GAUGE(buf, len, pos, "http_rsp_4xx",     t->http_rsp_4xx);
    PROM_GAUGE(buf, len, pos, "http_rsp_5xx",     t->http_rsp_5xx);
    PROM_GAUGE(buf, len, pos, "tls_handshake_ok", t->tls_handshake_ok);
    PROM_GAUGE(buf, len, pos, "tls_handshake_fail",t->tls_handshake_fail);
    return pos;
}

/* ------------------------------------------------------------------ */
/* Histogram percentile export                                          */
/* ------------------------------------------------------------------ */
int
export_histogram_prometheus(const histogram_t *h,
                            const char *metric_name,
                            char *buf, size_t len)
{
    static const double pcts[] = {50.0, 90.0, 99.0, 99.9};
    static const char  *lbls[] = {"p50","p90","p99","p999"};
    int pos = 0;

    pos += snprintf(buf + pos, len - pos,
                    "# TYPE %s summary\n", metric_name);
    for (int i = 0; i < 4; i++) {
        uint64_t v = hist_percentile(h, pcts[i]);
        pos += snprintf(buf + pos, len - pos,
                        "%s{quantile=\"%s\"} %"PRIu64"\n",
                        metric_name, lbls[i], v);
    }
    pos += snprintf(buf + pos, len - pos,
                    "%s_count %"PRIu64"\n"
                    "%s_sum %"PRIu64"\n",
                    metric_name, h->total_count,
                    metric_name, h->total_sum_us);
    return pos;
}
