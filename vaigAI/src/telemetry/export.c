/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Metrics export implementations.
 */
#include "export.h"
#include "histogram.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* JSON export (compact, for REST API / machine consumption)            */
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
        "  \"http_rsp_1xx\": %"PRIu64", \"http_rsp_2xx\": %"PRIu64",\n"
        "  \"http_rsp_3xx\": %"PRIu64", \"http_rsp_4xx\": %"PRIu64",\n"
        "  \"http_rsp_5xx\": %"PRIu64", \"http_parse_err\": %"PRIu64",\n"
        "  \"tls_handshake_ok\": %"PRIu64", \"tls_handshake_fail\": %"PRIu64",\n"
        "  \"tls_records_tx\": %"PRIu64", \"tls_records_rx\": %"PRIu64",\n"
        "  \"p50\": %"PRIu64", \"p95\": %"PRIu64", \"p99\": %"PRIu64"\n"
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
        t->http_rsp_1xx,  t->http_rsp_2xx,
        t->http_rsp_3xx,  t->http_rsp_4xx,
        t->http_rsp_5xx,  t->http_parse_err,
        t->tls_handshake_ok, t->tls_handshake_fail,
        t->tls_records_tx, t->tls_records_rx,
        hist_percentile(&snap->latency, 50.0),
        hist_percentile(&snap->latency, 95.0),
        hist_percentile(&snap->latency, 99.0));
    return n;
}

/* ------------------------------------------------------------------ */
/* Helpers for human-readable summary                                    */
/* ------------------------------------------------------------------ */
static int
append(char *buf, size_t len, int pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int
append(char *buf, size_t len, int pos, const char *fmt, ...)
{
    if (pos < 0 || (size_t)pos >= len) return pos;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, len - (size_t)pos, fmt, ap);
    va_end(ap);
    return (n < 0) ? pos : pos + n;
}

static const char *
fmt_bytes(uint64_t bytes, char *tmp, size_t sz)
{
    if (bytes >= 1073741824ULL)
        snprintf(tmp, sz, "%.1f GB", (double)bytes / 1073741824.0);
    else if (bytes >= 1048576ULL)
        snprintf(tmp, sz, "%.1f MB", (double)bytes / 1048576.0);
    else if (bytes >= 1024ULL)
        snprintf(tmp, sz, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(tmp, sz, "%"PRIu64" B", bytes);
    return tmp;
}

static const char *
fmt_pps(double pps, char *tmp, size_t sz)
{
    if (pps >= 1e6)
        snprintf(tmp, sz, "%.2f Mpps", pps / 1e6);
    else if (pps >= 1e3)
        snprintf(tmp, sz, "%.1f Kpps", pps / 1e3);
    else
        snprintf(tmp, sz, "%.1f pps", pps);
    return tmp;
}

static const char *
fmt_lat(uint64_t us, char *tmp, size_t sz)
{
    if (us == 0) { snprintf(tmp, sz, "--"); return tmp; }
    if (us >= 1000000ULL)
        snprintf(tmp, sz, "%.2fs", (double)us / 1e6);
    else if (us >= 1000ULL)
        snprintf(tmp, sz, "%.1fms", (double)us / 1e3);
    else
        snprintf(tmp, sz, "%"PRIu64"µs", us);
    return tmp;
}

/* ------------------------------------------------------------------ */
/* Human-readable summary with status, rates, warnings                  */
/* ------------------------------------------------------------------ */
int
export_summary(const metrics_snapshot_t *snap, uint32_t duration_s,
               const char *proto, char *buf, size_t len)
{
    const worker_metrics_t *t = &snap->total;
    int p = 0;
    double dur = duration_s > 0 ? (double)duration_s : 1.0;
    char tmp1[32], tmp2[32], tmp3[32];
    uint64_t p50 = hist_percentile(&snap->latency, 50.0);
    uint64_t p95 = hist_percentile(&snap->latency, 95.0);
    uint64_t p99 = hist_percentile(&snap->latency, 99.0);

    /* ── STATUS line ───────────────────────────────────────────────── */
    const char *status_str = "OK";
    uint64_t errs = t->tcp_bad_cksum + t->ip_bad_cksum + t->http_parse_err
                    + t->tls_handshake_fail;
    if (t->http_req_tx > 0 && t->http_rsp_rx < t->http_req_tx)
        status_str = "PARTIAL";
    if (errs > 0)
        status_str = "ERRORS";

    if (t->http_req_tx > 0) {
        double pct = (double)t->http_rsp_rx * 100.0 / (double)t->http_req_tx;
        p = append(buf, len, p,
            "STATUS: %s | %"PRIu64"/%"PRIu64" req completed (%.1f%%) | "
            "%.1f req/s | %"PRIu64" errs | p50=%s p95=%s p99=%s\n",
            status_str, t->http_rsp_rx, t->http_req_tx, pct,
            (double)t->http_rsp_rx / dur, errs,
            fmt_lat(p50, tmp1, sizeof(tmp1)),
            fmt_lat(p95, tmp2, sizeof(tmp2)),
            fmt_lat(p99, tmp3, sizeof(tmp3)));
    } else if (t->udp_tx > 0) {
        p = append(buf, len, p,
            "STATUS: %s | %"PRIu64" pkts | %s | %s TX | %"PRIu64" errs\n",
            status_str, t->udp_tx,
            fmt_pps((double)t->udp_tx / dur, tmp1, sizeof(tmp1)),
            fmt_bytes(t->tx_bytes, tmp2, sizeof(tmp2)), errs);
    } else if (t->tcp_syn_sent > 0) {
        double conn_rate = (double)t->tcp_conn_open / dur;
        p = append(buf, len, p,
            "STATUS: %s | %"PRIu64" conn opened | %.1f conn/s | "
            "%"PRIu64" retransmit | %"PRIu64" errs\n",
            status_str, t->tcp_conn_open, conn_rate,
            t->tcp_retransmit, errs);
    } else {
        p = append(buf, len, p,
            "STATUS: %s | TX: %"PRIu64" pkts %s | RX: %"PRIu64" pkts %s\n",
            status_str,
            t->tx_pkts, fmt_bytes(t->tx_bytes, tmp1, sizeof(tmp1)),
            t->rx_pkts, fmt_bytes(t->rx_bytes, tmp2, sizeof(tmp2)));
    }

    /* ── L2/L3 packet counters ─────────────────────────────────────── */
    p = append(buf, len, p, "\n--- packets ---\n");
    p = append(buf, len, p, "  tx_pkts: %-12"PRIu64"  tx_bytes: %s",
               t->tx_pkts, fmt_bytes(t->tx_bytes, tmp1, sizeof(tmp1)));
    if (duration_s > 0)
        p = append(buf, len, p, "  (%s, %.1f Mbps)",
                   fmt_pps((double)t->tx_pkts / dur, tmp2, sizeof(tmp2)),
                   (double)t->tx_bytes * 8.0 / dur / 1e6);
    p = append(buf, len, p, "\n");
    p = append(buf, len, p, "  rx_pkts: %-12"PRIu64"  rx_bytes: %s",
               t->rx_pkts, fmt_bytes(t->rx_bytes, tmp1, sizeof(tmp1)));
    if (duration_s > 0)
        p = append(buf, len, p, "  (%s)",
                   fmt_pps((double)t->rx_pkts / dur, tmp2, sizeof(tmp2)));
    p = append(buf, len, p, "\n");

    /* ── Warnings ──────────────────────────────────────────────────── */
    if (t->rx_bytes == 0 && t->tcp_payload_rx > 0)
        p = append(buf, len, p,
            "  ⚠ COUNTER MISMATCH: rx_bytes=0 but tcp_payload_rx=%"PRIu64"\n",
            t->tcp_payload_rx);

    /* ── TCP section (only if TCP was used) ─────────────────────────── */
    if (t->tcp_syn_sent > 0 || t->tcp_conn_open > 0) {
        p = append(buf, len, p, "\n--- tcp ---\n");
        /* Connection lifecycle */
        p = append(buf, len, p, "  tcp_conn_open:  %-8"PRIu64, t->tcp_conn_open);
        p = append(buf, len, p, "  tcp_conn_close: %"PRIu64, t->tcp_conn_close);
        if (t->tcp_conn_open > 0 && t->tcp_conn_close == 0)
            p = append(buf, len, p, " (0.0%%) ⚠ LEAK");
        else if (t->tcp_conn_open > 0)
            p = append(buf, len, p, " (%.1f%%)",
                       (double)t->tcp_conn_close * 100.0 / (double)t->tcp_conn_open);
        p = append(buf, len, p, "\n");

        p = append(buf, len, p, "  tcp_syn_sent:   %-8"PRIu64, t->tcp_syn_sent);
        p = append(buf, len, p, "  tcp_retransmit: %"PRIu64, t->tcp_retransmit);
        if (t->tcp_syn_sent > 0) {
            double rt_pct = (double)t->tcp_retransmit * 100.0 / (double)t->tcp_syn_sent;
            p = append(buf, len, p, " (%.1f%%)", rt_pct);
            if (rt_pct > 50.0) p = append(buf, len, p, " ⚠ HIGH");
        }
        p = append(buf, len, p, "\n");

        if (t->tcp_reset_rx || t->tcp_reset_sent)
            p = append(buf, len, p, "  tcp_reset_rx:   %-8"PRIu64
                       "  tcp_reset_sent: %"PRIu64"\n",
                       t->tcp_reset_rx, t->tcp_reset_sent);
        if (t->tcp_bad_cksum)
            p = append(buf, len, p, "  tcp_bad_cksum:  %"PRIu64" ⚠\n",
                       t->tcp_bad_cksum);
        if (t->tcp_duplicate_acks || t->tcp_ooo_pkts)
            p = append(buf, len, p, "  tcp_dup_acks:   %-8"PRIu64
                       "  tcp_ooo_pkts:   %"PRIu64"\n",
                       t->tcp_duplicate_acks, t->tcp_ooo_pkts);
        if (t->tcp_payload_tx || t->tcp_payload_rx) {
            p = append(buf, len, p, "  tcp_payload_tx: %-8s",
                       fmt_bytes(t->tcp_payload_tx, tmp1, sizeof(tmp1)));
            p = append(buf, len, p, "  tcp_payload_rx: %s\n",
                       fmt_bytes(t->tcp_payload_rx, tmp2, sizeof(tmp2)));
        }
    }

    /* ── HTTP section (only if HTTP was used) ───────────────────────── */
    if (t->http_req_tx > 0 || t->http_rsp_rx > 0) {
        p = append(buf, len, p, "\n--- http ---\n");
        p = append(buf, len, p, "  http_req_tx:    %-8"PRIu64, t->http_req_tx);
        p = append(buf, len, p, "  http_rsp_rx:    %"PRIu64, t->http_rsp_rx);
        if (t->http_req_tx > 0)
            p = append(buf, len, p, " (%.1f%%)",
                       (double)t->http_rsp_rx * 100.0 / (double)t->http_req_tx);
        p = append(buf, len, p, "\n");
        if (t->http_rsp_2xx) p = append(buf, len, p, "  2xx: %"PRIu64, t->http_rsp_2xx);
        if (t->http_rsp_3xx) p = append(buf, len, p, "  3xx: %"PRIu64, t->http_rsp_3xx);
        if (t->http_rsp_4xx) p = append(buf, len, p, "  4xx: %"PRIu64" ⚠", t->http_rsp_4xx);
        if (t->http_rsp_5xx) p = append(buf, len, p, "  5xx: %"PRIu64" ⚠", t->http_rsp_5xx);
        if (t->http_parse_err) p = append(buf, len, p, "  parse_err: %"PRIu64" ⚠", t->http_parse_err);
        if (t->http_rsp_2xx || t->http_rsp_3xx || t->http_rsp_4xx ||
            t->http_rsp_5xx || t->http_parse_err)
            p = append(buf, len, p, "\n");
        if (duration_s > 0)
            p = append(buf, len, p, "  req/s: %.1f  rsp/s: %.1f\n",
                       (double)t->http_req_tx / dur,
                       (double)t->http_rsp_rx / dur);
    }

    /* ── TLS section (only if TLS was used) ─────────────────────────── */
    if (t->tls_handshake_ok > 0 || t->tls_handshake_fail > 0) {
        p = append(buf, len, p, "\n--- tls ---\n");
        p = append(buf, len, p, "  tls_handshake_ok: %-6"PRIu64
                   "  tls_handshake_fail: %"PRIu64, t->tls_handshake_ok,
                   t->tls_handshake_fail);
        if (t->tls_handshake_fail) p = append(buf, len, p, " ⚠");
        p = append(buf, len, p, "\n");
        if (t->tls_records_tx || t->tls_records_rx)
            p = append(buf, len, p, "  tls_records_tx: %-6"PRIu64
                       "  tls_records_rx: %"PRIu64"\n",
                       t->tls_records_tx, t->tls_records_rx);
    }

    /* ── UDP section (only if UDP was used) ─────────────────────────── */
    if (t->udp_tx > 0 || t->udp_rx > 0) {
        p = append(buf, len, p, "\n--- udp ---\n");
        p = append(buf, len, p, "  udp_tx: %-12"PRIu64"  udp_rx: %"PRIu64"\n",
                   t->udp_tx, t->udp_rx);
        if (t->udp_bad_cksum)
            p = append(buf, len, p, "  udp_bad_cksum: %"PRIu64" ⚠\n",
                       t->udp_bad_cksum);
    }

    /* ── ARP/ICMP/IP errors (only if non-zero) ─────────────────────── */
    if (t->arp_reply_tx || t->arp_request_tx || t->arp_miss ||
        t->icmp_echo_tx || t->ip_bad_cksum || t->ip_frag_dropped ||
        t->ip_not_for_us) {
        p = append(buf, len, p, "\n--- other ---\n");
        if (t->arp_reply_tx || t->arp_request_tx)
            p = append(buf, len, p, "  arp_reply: %"PRIu64"  arp_request: %"PRIu64"\n",
                       t->arp_reply_tx, t->arp_request_tx);
        if (t->arp_miss)
            p = append(buf, len, p, "  arp_miss: %"PRIu64" ⚠\n", t->arp_miss);
        if (t->icmp_echo_tx)
            p = append(buf, len, p, "  icmp_echo_tx: %"PRIu64"\n", t->icmp_echo_tx);
        if (t->ip_bad_cksum)
            p = append(buf, len, p, "  ip_bad_cksum: %"PRIu64" ⚠\n", t->ip_bad_cksum);
        if (t->ip_not_for_us)
            p = append(buf, len, p, "  ip_not_for_us: %"PRIu64"\n", t->ip_not_for_us);
    }

    /* ── Latency (only if measured) ────────────────────────────────── */
    if (p50 || p95 || p99) {
        p = append(buf, len, p, "\n--- latency ---\n");
        p = append(buf, len, p, "  p50: %s  p95: %s  p99: %s\n",
                   fmt_lat(p50, tmp1, sizeof(tmp1)),
                   fmt_lat(p95, tmp2, sizeof(tmp2)),
                   fmt_lat(p99, tmp3, sizeof(tmp3)));
        if (snap->latency.total_count > 0) {
            uint64_t avg = snap->latency.total_sum_us / snap->latency.total_count;
            p = append(buf, len, p, "  min: %s  avg: %s  max: %s  samples: %"PRIu64"\n",
                       fmt_lat(snap->latency.min_us, tmp1, sizeof(tmp1)),
                       fmt_lat(avg, tmp2, sizeof(tmp2)),
                       fmt_lat(snap->latency.max_us, tmp3, sizeof(tmp3)),
                       snap->latency.total_count);
        }
    }

    return p;
}