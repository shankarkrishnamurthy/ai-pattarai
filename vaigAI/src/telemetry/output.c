/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Structured NDJSON output implementation.
 *
 * Each event is a single JSON line appended to the output file.
 * All functions are no-ops when g_output_fp is NULL.
 */
#include "output.h"
#include "histogram.h"
#include "../core/core_assign.h"
#include "../port/port_init.h"
#include "../mgmt/config_mgr.h"
#include "../net/arp.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/utsname.h>
#include <rte_version.h>
#include <rte_ethdev.h>

/* ── Global output file handle ─────────────────────────────────────── */
static FILE *g_output_fp;

/* ── ISO-8601 timestamp helper ─────────────────────────────────────── */
static void
ts_now(char *buf, size_t len)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int n = snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec,
                     ts.tv_nsec / 1000000L);
    (void)n;
}

/* ── JSON string escaper (for user-supplied strings) ───────────────── */
static void
json_str(FILE *fp, const char *s)
{
    fputc('"', fp);
    if (s) {
        for (; *s; s++) {
            switch (*s) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n",  fp); break;
            case '\r': fputs("\\r",  fp); break;
            case '\t': fputs("\\t",  fp); break;
            default:
                if ((unsigned char)*s < 0x20)
                    fprintf(fp, "\\u%04x", (unsigned char)*s);
                else
                    fputc(*s, fp);
                break;
            }
        }
    }
    fputc('"', fp);
}

/* ── IPv4 formatter ────────────────────────────────────────────────── */
static const char *
ip_str(uint32_t ip_be, char *buf, size_t len)
{
    uint32_t ip = rte_be_to_cpu_32(ip_be);
    snprintf(buf, len, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >>  8) & 0xFF,  ip        & 0xFF);
    return buf;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

int
output_init(const char *path)
{
    if (!path || path[0] == '\0')
        return 0;

    g_output_fp = fopen(path, "w");
    if (!g_output_fp) {
        fprintf(stderr, "[TGEN] cannot open output file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    /* Line-buffered so every event is flushed immediately */
    setvbuf(g_output_fp, NULL, _IOLBF, 0);
    return 0;
}

void
output_fini(void)
{
    if (g_output_fp) {
        fclose(g_output_fp);
        g_output_fp = NULL;
    }
}

bool
output_enabled(void)
{
    return g_output_fp != NULL;
}

/* ── meta ──────────────────────────────────────────────────────────── */
void
output_meta(int argc, char **argv)
{
    if (!g_output_fp) return;
    char ts[64];
    ts_now(ts, sizeof(ts));

    struct utsname uts;
    uname(&uts);

    fprintf(g_output_fp,
        "{\"ts\":\"%s\",\"type\":\"meta\""
        ",\"version\":\"" TGEN_VERSION "\""
        ",\"dpdk_version\":\"%s\""
        ",\"hostname\":\"%s\""
        ",\"kernel\":\"%s\""
        ",\"arch\":\"%s\""
        ",\"cmdline\":",
        ts, rte_version(), uts.nodename, uts.release, uts.machine);

    /* cmdline as JSON array */
    fputc('[', g_output_fp);
    for (int i = 0; i < argc; i++) {
        if (i > 0) fputc(',', g_output_fp);
        json_str(g_output_fp, argv[i]);
    }
    fputs("]}\n", g_output_fp);
}

/* ── config ────────────────────────────────────────────────────────── */
void
output_config(void)
{
    if (!g_output_fp) return;
    char ts[64], ip_buf[16], gw_buf[16], nm_buf[16];
    ts_now(ts, sizeof(ts));

    uint32_t src_ip = 0, gw = 0, nm = 0;
    if (g_n_ports > 0) {
        src_ip = g_arp[0].local_ip;
        gw     = g_arp[0].gateway_ip;
        nm     = g_arp[0].netmask;
    }

    fprintf(g_output_fp,
        "{\"ts\":\"%s\",\"type\":\"config\""
        ",\"workers\":%u"
        ",\"mgmt_cores\":%u"
        ",\"ports\":%u"
        ",\"src_ip\":\"%s\""
        ",\"gateway\":\"%s\""
        ",\"netmask\":\"%s\""
        ",\"server_mode\":%s"
        ",\"tls_enabled\":%s"
        ",\"max_conn\":%u"
        "}\n",
        ts,
        g_core_map.num_workers,
        g_core_map.num_mgmt,
        g_n_ports,
        ip_str(src_ip, ip_buf, sizeof(ip_buf)),
        ip_str(gw, gw_buf, sizeof(gw_buf)),
        ip_str(nm, nm_buf, sizeof(nm_buf)),
        g_config.server_mode ? "true" : "false",
        g_config.tls_enabled ? "true" : "false",
        g_config.max_concurrent);
}

/* ── port ──────────────────────────────────────────────────────────── */
void
output_port(uint16_t port_id)
{
    if (!g_output_fp) return;
    if (port_id >= g_n_ports) return;

    char ts[64];
    ts_now(ts, sizeof(ts));

    port_caps_t *cap = &g_port_caps[port_id];
    struct rte_eth_link link;
    int rc = rte_eth_link_get_nowait(port_id, &link);
    if (rc < 0) memset(&link, 0, sizeof(link));

    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             cap->mac_addr.addr_bytes[0], cap->mac_addr.addr_bytes[1],
             cap->mac_addr.addr_bytes[2], cap->mac_addr.addr_bytes[3],
             cap->mac_addr.addr_bytes[4], cap->mac_addr.addr_bytes[5]);

    fprintf(g_output_fp,
        "{\"ts\":\"%s\",\"type\":\"port\""
        ",\"port_id\":%u"
        ",\"driver\":\"%s\""
        ",\"mac\":\"%s\""
        ",\"link_speed\":%u"
        ",\"link_up\":%s"
        ",\"rx_queues\":%u"
        ",\"tx_queues\":%u"
        ",\"rss\":%s"
        "}\n",
        ts, port_id, cap->driver_name, mac,
        link.link_speed,
        link.link_status ? "true" : "false",
        cap->max_rx_queues, cap->max_tx_queues,
        cap->has_rss ? "true" : "false");
}

/* ── cmd ───────────────────────────────────────────────────────────── */
void
output_cmd(const char *input)
{
    if (!g_output_fp) return;
    char ts[64];
    ts_now(ts, sizeof(ts));

    fprintf(g_output_fp, "{\"ts\":\"%s\",\"type\":\"cmd\",\"input\":", ts);
    json_str(g_output_fp, input);
    fputs("}\n", g_output_fp);
}

/* ── start ─────────────────────────────────────────────────────────── */
void
output_start(uint32_t stream_idx, const char *proto,
             const char *dst_ip, uint16_t dst_port,
             uint32_t duration, uint64_t rate, uint16_t size,
             bool tls, uint32_t streams, bool reuse)
{
    if (!g_output_fp) return;
    char ts[64];
    ts_now(ts, sizeof(ts));

    fprintf(g_output_fp,
        "{\"ts\":\"%s\",\"type\":\"start\""
        ",\"stream_idx\":%u"
        ",\"proto\":\"%s\""
        ",\"dst_ip\":\"%s\""
        ",\"dst_port\":%u"
        ",\"duration\":%u"
        ",\"rate\":%"PRIu64
        ",\"size\":%u"
        ",\"tls\":%s"
        ",\"streams\":%u"
        ",\"reuse\":%s"
        "}\n",
        ts, stream_idx, proto, dst_ip, dst_port,
        duration, rate, size,
        tls ? "true" : "false",
        streams, reuse ? "true" : "false");
}

/* ── serve ─────────────────────────────────────────────────────────── */
void
output_serve(const char *listeners, const char *ciphers)
{
    if (!g_output_fp) return;
    char ts[64];
    ts_now(ts, sizeof(ts));

    fprintf(g_output_fp,
        "{\"ts\":\"%s\",\"type\":\"serve\",\"listeners\":", ts);
    json_str(g_output_fp, listeners);
    if (ciphers) {
        fputs(",\"ciphers\":", g_output_fp);
        json_str(g_output_fp, ciphers);
    }
    fputs("}\n", g_output_fp);
}

/* ── Helper: write metrics object ──────────────────────────────────── */
static void
write_metrics(FILE *fp, const worker_metrics_t *t, const histogram_t *lat)
{
    fprintf(fp,
        "{\"tx_pkts\":%"PRIu64
        ",\"tx_bytes\":%"PRIu64
        ",\"rx_pkts\":%"PRIu64
        ",\"rx_bytes\":%"PRIu64
        ",\"tcp_conn_open\":%"PRIu64
        ",\"tcp_conn_close\":%"PRIu64
        ",\"tcp_syn_sent\":%"PRIu64
        ",\"tcp_retransmit\":%"PRIu64
        ",\"tcp_reset_rx\":%"PRIu64
        ",\"tcp_reset_sent\":%"PRIu64
        ",\"tcp_bad_cksum\":%"PRIu64
        ",\"tcp_syn_queue_drops\":%"PRIu64
        ",\"tcp_duplicate_acks\":%"PRIu64
        ",\"tcp_ooo_pkts\":%"PRIu64
        ",\"tcp_payload_tx\":%"PRIu64
        ",\"tcp_payload_rx\":%"PRIu64
        ",\"udp_tx\":%"PRIu64
        ",\"udp_rx\":%"PRIu64
        ",\"http_req_tx\":%"PRIu64
        ",\"http_rsp_rx\":%"PRIu64
        ",\"http_rsp_2xx\":%"PRIu64
        ",\"http_rsp_4xx\":%"PRIu64
        ",\"http_rsp_5xx\":%"PRIu64
        ",\"http_parse_err\":%"PRIu64
        ",\"tls_handshake_ok\":%"PRIu64
        ",\"tls_handshake_fail\":%"PRIu64
        ",\"tls_records_tx\":%"PRIu64
        ",\"tls_records_rx\":%"PRIu64
        "}",
        t->tx_pkts, t->tx_bytes,
        t->rx_pkts, t->rx_bytes,
        t->tcp_conn_open, t->tcp_conn_close,
        t->tcp_syn_sent, t->tcp_retransmit,
        t->tcp_reset_rx, t->tcp_reset_sent,
        t->tcp_bad_cksum, t->tcp_syn_queue_drops,
        t->tcp_duplicate_acks, t->tcp_ooo_pkts,
        t->tcp_payload_tx, t->tcp_payload_rx,
        t->udp_tx, t->udp_rx,
        t->http_req_tx, t->http_rsp_rx,
        t->http_rsp_2xx, t->http_rsp_4xx, t->http_rsp_5xx,
        t->http_parse_err,
        t->tls_handshake_ok, t->tls_handshake_fail,
        t->tls_records_tx, t->tls_records_rx);

    /* Latency as a separate nested object if histogram has data */
    if (lat && lat->total_count > 0) {
        uint64_t avg = lat->total_sum_us / lat->total_count;
        /* caller already wrote the metrics object, so we need to
         * NOT close with } yet — but we already did. This helper writes
         * a standalone metrics object, latency is written separately. */
        (void)avg; /* latency written by caller */
    }
}

/* ── progress ──────────────────────────────────────────────────────── */
void
output_progress(uint32_t stream_idx, uint64_t elapsed_s,
                const metrics_snapshot_t *snap)
{
    if (!g_output_fp) return;
    char ts[64];
    ts_now(ts, sizeof(ts));

    const worker_metrics_t *t = &snap->total;
    fprintf(g_output_fp,
        "{\"ts\":\"%s\",\"type\":\"progress\""
        ",\"stream_idx\":%u"
        ",\"elapsed_s\":%"PRIu64
        ",\"tx_pkts\":%"PRIu64
        ",\"rx_pkts\":%"PRIu64
        ",\"tcp_conn_open\":%"PRIu64
        ",\"http_rsp_rx\":%"PRIu64
        "}\n",
        ts, stream_idx, elapsed_s,
        t->tx_pkts, t->rx_pkts, t->tcp_conn_open, t->http_rsp_rx);
}

/* ── result ────────────────────────────────────────────────────────── */
void
output_result(uint32_t stream_idx, const char *proto,
              double actual_s, const metrics_snapshot_t *snap)
{
    if (!g_output_fp) return;
    char ts[64];
    ts_now(ts, sizeof(ts));

    const worker_metrics_t *t = &snap->total;
    uint64_t errs = t->tcp_bad_cksum + t->ip_bad_cksum + t->http_parse_err
                    + t->tls_handshake_fail;
    const char *status = "OK";
    if (t->http_req_tx > 0 && t->http_rsp_rx < t->http_req_tx)
        status = "PARTIAL";
    if (errs > 0)
        status = "ERRORS";

    fprintf(g_output_fp,
        "{\"ts\":\"%s\",\"type\":\"result\""
        ",\"stream_idx\":%u"
        ",\"proto\":\"%s\""
        ",\"status\":\"%s\""
        ",\"actual_duration_s\":%.3f"
        ",\"metrics\":",
        ts, stream_idx, proto, status, actual_s);

    write_metrics(g_output_fp, t, &snap->latency);

    /* Latency */
    uint64_t p50  = hist_percentile(&snap->latency, 50.0);
    uint64_t p90  = hist_percentile(&snap->latency, 90.0);
    uint64_t p95  = hist_percentile(&snap->latency, 95.0);
    uint64_t p99  = hist_percentile(&snap->latency, 99.0);
    uint64_t p999 = hist_percentile(&snap->latency, 99.9);
    fprintf(g_output_fp,
        ",\"latency\":{"
        "\"p50\":%"PRIu64
        ",\"p90\":%"PRIu64
        ",\"p95\":%"PRIu64
        ",\"p99\":%"PRIu64
        ",\"p999\":%"PRIu64,
        p50, p90, p95, p99, p999);
    if (snap->latency.total_count > 0) {
        uint64_t avg = snap->latency.total_sum_us / snap->latency.total_count;
        fprintf(g_output_fp,
            ",\"min\":%"PRIu64
            ",\"avg\":%"PRIu64
            ",\"max\":%"PRIu64
            ",\"samples\":%"PRIu64,
            snap->latency.min_us, avg, snap->latency.max_us,
            snap->latency.total_count);
    }
    fputc('}', g_output_fp);

    /* Per-worker summary */
    fprintf(g_output_fp, ",\"workers\":%u", snap->n_workers);
    if (snap->n_workers > 1) {
        fputs(",\"per_worker\":[", g_output_fp);
        for (uint32_t w = 0; w < snap->n_workers; w++) {
            if (w > 0) fputc(',', g_output_fp);
            const worker_metrics_t *wm = &snap->per_worker[w];
            fprintf(g_output_fp,
                "{\"w\":%u,\"tx_pkts\":%"PRIu64
                ",\"rx_pkts\":%"PRIu64
                ",\"tcp_conn_open\":%"PRIu64
                ",\"tcp_retransmit\":%"PRIu64"}",
                w, wm->tx_pkts, wm->rx_pkts,
                wm->tcp_conn_open, wm->tcp_retransmit);
        }
        fputc(']', g_output_fp);
    }

    fputs("}\n", g_output_fp);
}

/* ── error ─────────────────────────────────────────────────────────── */
void
output_error(const char *severity, const char *module,
             const char *message)
{
    if (!g_output_fp) return;
    char ts[64];
    ts_now(ts, sizeof(ts));

    fprintf(g_output_fp,
        "{\"ts\":\"%s\",\"type\":\"error\""
        ",\"severity\":\"%s\""
        ",\"module\":\"%s\""
        ",\"message\":", ts, severity, module);
    json_str(g_output_fp, message);
    fputs("}\n", g_output_fp);
}

/* ── end ───────────────────────────────────────────────────────────── */
void
output_end(int exit_code, uint64_t uptime_s)
{
    if (!g_output_fp) return;
    char ts[64];
    ts_now(ts, sizeof(ts));

    fprintf(g_output_fp,
        "{\"ts\":\"%s\",\"type\":\"end\""
        ",\"exit_code\":%d"
        ",\"uptime_s\":%"PRIu64
        "}\n",
        ts, exit_code, uptime_s);
}
