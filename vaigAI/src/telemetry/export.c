/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Metrics export implementations.
 */
#include "export.h"
#include "histogram.h"
#include "cpu_stats.h"
#include "mem_stats.h"
#include "../core/core_assign.h"
#include "../core/worker_loop.h"
#include "../net/tcp_tcb.h"
#include "../port/port_init.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <rte_ethdev.h>

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

/* ================================================================== */
/* CPU stats — human-readable text                                     */
/* ================================================================== */
static const char *
fmt_loops(uint64_t loops, char *tmp, size_t sz)
{
    if (loops >= 1000000000ULL)
        snprintf(tmp, sz, "%.1fG", (double)loops / 1e9);
    else if (loops >= 1000000ULL)
        snprintf(tmp, sz, "%.1fM", (double)loops / 1e6);
    else if (loops >= 1000ULL)
        snprintf(tmp, sz, "%.1fK", (double)loops / 1e3);
    else
        snprintf(tmp, sz, "%"PRIu64, loops);
    return tmp;
}

int
export_cpu_text(const cpu_stats_snapshot_t *snap, int core,
                char *buf, size_t len)
{
    int p = 0;
    uint64_t hz = snap->tsc_hz ? snap->tsc_hz : 1;

    p = append(buf, len, p,
        "──────────────────────────────────────────────────────────────────\n"
        "Core   Lcore  Socket  Role     Busy%%   RX%%    TX%%   Timer%%  Idle%%\n"
        "──────────────────────────────────────────────────────────────────\n");

    uint32_t start = 0, end = snap->n_workers;
    if (core >= 0 && (uint32_t)core < snap->n_workers) {
        start = (uint32_t)core;
        end   = start + 1;
    }

    uint64_t tot_busy = 0, tot_idle = 0, tot_total = 0;
    uint64_t tot_rx = 0, tot_tx = 0, tot_timer = 0;
    uint32_t active = 0;

    for (uint32_t w = start; w < end; w++) {
        const cpu_stats_t *cs = &snap->per_worker[w];
        if (cs->cycles_total == 0) {
            p = append(buf, len, p,
                "W%-5u %-6u %-7u worker   --      --     --    --      --\n",
                w, g_worker_ctx[w].lcore_id, g_worker_ctx[w].socket_id);
            continue;
        }
        active++;
        double total = (double)cs->cycles_total;
        double busy_pct = (1.0 - (double)cs->cycles_idle / total) * 100.0;
        double rx_pct   = (double)cs->cycles_rx    / total * 100.0;
        double tx_pct   = (double)cs->cycles_tx    / total * 100.0;
        double tmr_pct  = (double)cs->cycles_timer / total * 100.0;
        double idle_pct = (double)cs->cycles_idle  / total * 100.0;

        p = append(buf, len, p,
            "W%-5u %-6u %-7u worker   %5.1f   %5.1f  %5.1f  %5.1f   %5.1f\n",
            w, g_worker_ctx[w].lcore_id, g_worker_ctx[w].socket_id,
            busy_pct, rx_pct, tx_pct, tmr_pct, idle_pct);

        tot_busy  += cs->cycles_total - cs->cycles_idle;
        tot_idle  += cs->cycles_idle;
        tot_total += cs->cycles_total;
        tot_rx    += cs->cycles_rx;
        tot_tx    += cs->cycles_tx;
        tot_timer += cs->cycles_timer;
    }

    p = append(buf, len, p,
        "──────────────────────────────────────────────────────────────────\n");

    if (active > 1 && core < 0) {
        double t = (double)tot_total;
        if (t > 0) {
            p = append(buf, len, p,
                "Total (workers)                %5.1f   %5.1f  %5.1f  %5.1f   %5.1f\n",
                (double)tot_busy / t * 100.0,
                (double)tot_rx / t * 100.0,
                (double)tot_tx / t * 100.0,
                (double)tot_timer / t * 100.0,
                (double)tot_idle / t * 100.0);
        }
    }

    /* Loop rate line */
    p = append(buf, len, p, "\nLoop rate: ");
    char tmp[32];
    for (uint32_t w = start; w < end; w++) {
        const cpu_stats_t *cs = &snap->per_worker[w];
        if (cs->cycles_total == 0) continue;
        double secs = (double)cs->cycles_total / (double)hz;
        uint64_t lps = secs > 0 ? (uint64_t)((double)cs->loop_count / secs) : 0;
        p = append(buf, len, p, " W%u=%s/s", w, fmt_loops(lps, tmp, sizeof(tmp)));
    }

    /* Uptime */
    const cpu_stats_t *first = &snap->per_worker[start];
    if (first->cycles_total > 0) {
        uint64_t uptime_s = first->cycles_total / hz;
        p = append(buf, len, p, "   Uptime: %"PRIu64"s", uptime_s);
    }
    p = append(buf, len, p, "\n");

    return p;
}

/* ================================================================== */
/* Memory stats — human-readable text                                  */
/* ================================================================== */
int
export_mem_text(const mem_stats_snapshot_t *snap, int core,
                char *buf, size_t len)
{
    int p = 0;

    /* ── Packet buffers ────────────────────────────────────────────── */
    p = append(buf, len, p,
        "\n--- packet buffers ---\n"
        "Pool         Total    In-Use   Avail    Use%%\n");

    uint32_t pool_total = 0, pool_inuse = 0;
    for (uint32_t i = 0; i < snap->n_pools; i++) {
        if (core >= 0 && (int)i != core) continue;
        const mempool_info_t *pi = &snap->pools[i];
        double pct = pi->total > 0 ? (double)pi->in_use * 100.0 / (double)pi->total : 0;
        p = append(buf, len, p, "%-12s %-8u %-8u %-8u %5.1f%%\n",
                   pi->name, pi->total, pi->in_use, pi->avail, pct);
        pool_total += pi->total;
        pool_inuse += pi->in_use;
    }
    if (core < 0 && snap->n_pools > 1) {
        double pct = pool_total > 0 ? (double)pool_inuse * 100.0 / (double)pool_total : 0;
        p = append(buf, len, p,
            "                                        ─────\n"
            "Total        %-8u %-8u %-8u %5.1f%%\n",
            pool_total, pool_inuse, pool_total - pool_inuse, pct);
    }

    /* ── DPDK heap (only in aggregate view) ────────────────────────── */
    if (core < 0 && snap->n_heaps > 0) {
        p = append(buf, len, p,
            "\n--- dpdk heap ---\n"
            "Socket   Heap Size    Allocated    Free         Use%%\n");
        for (uint32_t i = 0; i < snap->n_heaps; i++) {
            const heap_info_t *hi = &snap->heaps[i];
            double pct = hi->heap_size > 0
                ? (double)hi->alloc_size * 100.0 / (double)hi->heap_size : 0;
            char h1[32], h2[32], h3[32];
            p = append(buf, len, p, "%-8d %-12s %-12s %-12s %5.1f%%\n",
                       hi->socket_id,
                       fmt_bytes(hi->heap_size,  h1, sizeof(h1)),
                       fmt_bytes(hi->alloc_size, h2, sizeof(h2)),
                       fmt_bytes(hi->free_size,  h3, sizeof(h3)),
                       pct);
        }
    }

    /* ── Connections ───────────────────────────────────────────────── */
    p = append(buf, len, p, "\n--- connections ---\n");
    if (core >= 0 && (uint32_t)core < snap->n_tcbs) {
        const tcb_info_t *ti = &snap->tcbs[core];
        double pct = ti->capacity > 0
            ? (double)ti->active * 100.0 / (double)ti->capacity : 0;
        p = append(buf, len, p, "Active: %u / %u (%.1f%%)\n",
                   ti->active, ti->capacity, pct);
    } else {
        p = append(buf, len, p, "Worker   Active   Capacity   Use%%\n");
        for (uint32_t i = 0; i < snap->n_tcbs; i++) {
            const tcb_info_t *ti = &snap->tcbs[i];
            double pct = ti->capacity > 0
                ? (double)ti->active * 100.0 / (double)ti->capacity : 0;
            p = append(buf, len, p, "W%-7u %-8u %-10u %5.1f%%\n",
                       i, ti->active, ti->capacity, pct);
        }
    }

    /* ── Hugepages (only in aggregate view) ────────────────────────── */
    if (core < 0 && snap->n_hugepages > 0) {
        p = append(buf, len, p,
            "\n--- hugepages ---\n"
            "Size     Total   Free   In-Use   Use%%\n");
        for (uint32_t i = 0; i < snap->n_hugepages; i++) {
            const hugepage_info_t *hp = &snap->hugepages[i];
            const char *sz_str = hp->size_kb >= 1048576 ? "1 GB" : "2 MB";
            if (hp->total == 0) {
                p = append(buf, len, p, "%-8s %-7"PRIu64" %-6"PRIu64" %-8"PRIu64"  --\n",
                           sz_str, hp->total, hp->free, hp->in_use);
            } else {
                double pct = (double)hp->in_use * 100.0 / (double)hp->total;
                p = append(buf, len, p, "%-8s %-7"PRIu64" %-6"PRIu64" %-8"PRIu64" %5.1f%%\n",
                           sz_str, hp->total, hp->free, hp->in_use, pct);
            }
        }
    }

    return p;
}

/* ================================================================== */
/* Port stats — human-readable text                                    */
/* ================================================================== */
int
export_port_text(char *buf, size_t len)
{
    int p = 0;
    uint32_t n_ports = g_n_ports;

    p = append(buf, len, p,
        "─────────────────────────────────────────────────────────────────────\n"
        "Port  Driver     Link       RX pkts     RX bytes   RX miss  RX err\n"
        "                            TX pkts     TX bytes   TX err\n"
        "─────────────────────────────────────────────────────────────────────\n");

    char tmp[32];
    for (uint32_t i = 0; i < n_ports; i++) {
        struct rte_eth_link link;
        int link_rc = rte_eth_link_get_nowait(i, &link);
        if (link_rc < 0)
            memset(&link, 0, sizeof(link));

        struct rte_eth_stats stats;
        rte_eth_stats_get(i, &stats);

        char link_str[32];
        if (link.link_status)
            snprintf(link_str, sizeof(link_str), "UP %uG",
                     link.link_speed / 1000);
        else
            snprintf(link_str, sizeof(link_str), "DOWN");

        p = append(buf, len, p,
            "%-5u %-10s %-10s %-11"PRIu64" %-10s %-8"PRIu64" %"PRIu64"\n",
            i, g_port_caps[i].driver_name, link_str,
            stats.ipackets, fmt_bytes(stats.ibytes, tmp, sizeof(tmp)),
            stats.imissed, stats.ierrors);
        p = append(buf, len, p,
            "                            %-11"PRIu64" %-10s %"PRIu64"\n",
            stats.opackets, fmt_bytes(stats.obytes, tmp, sizeof(tmp)),
            stats.oerrors);
    }

    p = append(buf, len, p,
        "─────────────────────────────────────────────────────────────────────\n");

    return p;
}

/* ================================================================== */
/* Stat summary — brief one-liner from each domain                     */
/* ================================================================== */
int
export_stat_summary(const cpu_stats_snapshot_t *cpu,
                    const mem_stats_snapshot_t *mem,
                    const metrics_snapshot_t *net,
                    char *buf, size_t len)
{
    int p = 0;
    char tmp1[32], tmp2[32];
    uint64_t hz = cpu->tsc_hz ? cpu->tsc_hz : 1;

    /* CPU summary */
    uint64_t tot_busy = 0, tot_total = 0;
    uint32_t active = 0;
    for (uint32_t w = 0; w < cpu->n_workers; w++) {
        const cpu_stats_t *cs = &cpu->per_worker[w];
        if (cs->cycles_total == 0) continue;
        active++;
        tot_busy += cs->cycles_total - cs->cycles_idle;
        tot_total += cs->cycles_total;
    }
    double avg_busy = tot_total > 0 ? (double)tot_busy / (double)tot_total * 100.0 : 0;
    uint64_t uptime = tot_total > 0
        ? cpu->per_worker[0].cycles_total / hz : 0;

    p = append(buf, len, p,
        "\n--- cpu ---\n"
        "Workers: %u   Avg Busy: %.1f%%   Avg Idle: %.1f%%   Uptime: %"PRIu64"s\n",
        active, avg_busy, 100.0 - avg_busy, uptime);

    /* Memory summary */
    uint32_t pool_total = 0, pool_inuse = 0;
    for (uint32_t i = 0; i < mem->n_pools; i++) {
        pool_total += mem->pools[i].total;
        pool_inuse += mem->pools[i].in_use;
    }
    uint64_t heap_alloc = 0, heap_total = 0;
    for (uint32_t i = 0; i < mem->n_heaps; i++) {
        heap_alloc += mem->heaps[i].alloc_size;
        heap_total += mem->heaps[i].heap_size;
    }
    double pool_pct = pool_total > 0
        ? (double)pool_inuse * 100.0 / (double)pool_total : 0;
    double heap_pct = heap_total > 0
        ? (double)heap_alloc * 100.0 / (double)heap_total : 0;

    p = append(buf, len, p,
        "\n--- mem ---\n"
        "Packet buffers: %u/%u (%.1f%%)   DPDK heap: %s/%s (%.1f%%)\n",
        pool_inuse, pool_total, pool_pct,
        fmt_bytes(heap_alloc, tmp1, sizeof(tmp1)),
        fmt_bytes(heap_total, tmp2, sizeof(tmp2)),
        heap_pct);

    /* Net summary */
    const worker_metrics_t *t = &net->total;
    p = append(buf, len, p,
        "\n--- net ---\n"
        "TX: %"PRIu64" pkts (%s)   RX: %"PRIu64" pkts (%s)   Errors: %"PRIu64"\n",
        t->tx_pkts, fmt_bytes(t->tx_bytes, tmp1, sizeof(tmp1)),
        t->rx_pkts, fmt_bytes(t->rx_bytes, tmp2, sizeof(tmp2)),
        t->tcp_bad_cksum + t->ip_bad_cksum + t->http_parse_err);

    /* Port summary */
    p = append(buf, len, p, "\n--- port ---\n");
    for (uint32_t i = 0; i < g_n_ports; i++) {
        struct rte_eth_link link;
        int link_rc = rte_eth_link_get_nowait(i, &link);
        if (link_rc < 0)
            memset(&link, 0, sizeof(link));
        struct rte_eth_stats stats;
        rte_eth_stats_get(i, &stats);
        p = append(buf, len, p,
            "Port %u: %s  RX %s  TX %s  miss %"PRIu64"  err %"PRIu64"\n",
            i, link.link_status ? "UP" : "DOWN",
            fmt_loops(stats.ipackets, tmp1, sizeof(tmp1)),
            fmt_loops(stats.opackets, tmp2, sizeof(tmp2)),
            stats.imissed, stats.ierrors);
    }

    return p;
}

/* ================================================================== */
/* Per-worker net stats + TCP state distribution                       */
/* ================================================================== */
int
export_net_core_text(const metrics_snapshot_t *snap, uint32_t core,
                     char *buf, size_t len)
{
    int p = 0;
    if (core >= snap->n_workers) {
        p = append(buf, len, p, "Worker %u does not exist\n", core);
        return p;
    }
    char tmp[32];
    const worker_metrics_t *wm = &snap->per_worker[core];

    p = append(buf, len, p,
        "\n--- worker %u (lcore %u) ---\n",
        core, g_worker_ctx[core].lcore_id);
    p = append(buf, len, p, "  tx_pkts: %-12"PRIu64"  tx_bytes: %s\n",
               wm->tx_pkts, fmt_bytes(wm->tx_bytes, tmp, sizeof(tmp)));
    p = append(buf, len, p, "  rx_pkts: %-12"PRIu64"  rx_bytes: %s\n",
               wm->rx_pkts, fmt_bytes(wm->rx_bytes, tmp, sizeof(tmp)));

    if (wm->tcp_conn_open || wm->tcp_syn_sent) {
        p = append(buf, len, p,
            "  tcp_conn_open: %-6"PRIu64"  tcp_conn_close: %"PRIu64"\n",
            wm->tcp_conn_open, wm->tcp_conn_close);
        p = append(buf, len, p,
            "  tcp_syn_sent:  %-6"PRIu64"  tcp_retransmit: %"PRIu64"\n",
            wm->tcp_syn_sent, wm->tcp_retransmit);
    }

    /* TCP connection state distribution */
    if (core < TGEN_MAX_WORKERS) {
        tcb_store_t *store = &g_tcb_stores[core];
        uint32_t n_est = 0, n_syn = 0, n_tw = 0, n_fin = 0, n_other = 0;
        for (uint32_t i = 0; i < store->capacity; i++) {
            if (!store->tcbs[i].in_use) continue;
            switch (store->tcbs[i].state) {
            case TCP_ESTABLISHED: n_est++;   break;
            case TCP_SYN_SENT:    n_syn++;   break;
            case TCP_TIME_WAIT:   n_tw++;    break;
            case TCP_FIN_WAIT_1:
            case TCP_FIN_WAIT_2:  n_fin++;   break;
            default:                    n_other++; break;
            }
        }
        uint32_t total = store->count;
        if (total > 0 || wm->tcp_conn_open > 0) {
            p = append(buf, len, p,
                "\n--- tcp connections (W%u) ---\n"
                "State          Count\n", core);
            if (n_est)   p = append(buf, len, p, "ESTABLISHED    %u\n", n_est);
            if (n_syn)   p = append(buf, len, p, "SYN_SENT       %u\n", n_syn);
            if (n_tw)    p = append(buf, len, p, "TIME_WAIT      %u\n", n_tw);
            if (n_fin)   p = append(buf, len, p, "FIN_WAIT       %u\n", n_fin);
            if (n_other) p = append(buf, len, p, "OTHER          %u\n", n_other);
            p = append(buf, len, p, "TOTAL          %u / %u\n",
                       total, store->capacity);
        }
    }

    return p;
}