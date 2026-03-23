/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: CLI implementation (readline when available, fallback fgets).
 */
#include "cli.h"
#include "cli_server.h"
#include "config_mgr.h"
#include "mgmt_loop.h"
#include "../net/icmp.h"
#include "../net/udp.h"
#include "../net/arp.h"
#include "../net/tcp_fsm.h"
#include "../net/tcp_tcb.h"
#include "../net/tcp_port_pool.h"
#include "../telemetry/pktrace.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"
#include "../telemetry/export.h"
#include "../telemetry/cpu_stats.h"
#include "../telemetry/mem_stats.h"
#include "../telemetry/log.h"
#include "../core/ipc.h"
#include "../core/tx_gen.h"
#include "../core/worker_loop.h"
#include "../core/core_assign.h"
#include "../port/port_init.h"
#include "../tls/tls_session.h"
#include "../tls/tls_engine.h"
#include "../app/http11.h"
#include "../app/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <poll.h>
#include <arpa/inet.h>
#include <rte_ethdev.h>

#ifdef HAVE_READLINE
# include <readline/readline.h>
# include <readline/history.h>
#endif

#define MAX_CMDS  64u
#define MAX_ARGS  32u

typedef struct {
    char           name[64];
    char           help[128];
    const char    *usage;
    cli_cmd_fn_t   fn;
} cli_entry_t;

static cli_entry_t g_cmds[MAX_CMDS];
static uint32_t    g_n_cmds;

/* ------------------------------------------------------------------ */
/* Built-in commands                                                    */
/* ------------------------------------------------------------------ */
static void
cmd_help(int argc, char **argv)
{
    if (argc >= 2) {
        for (uint32_t i = 0; i < g_n_cmds; i++) {
            if (strcmp(argv[1], g_cmds[i].name) == 0) {
                printf("  %s — %s\n", g_cmds[i].name, g_cmds[i].help);
                if (g_cmds[i].usage)
                    printf("\n%s", g_cmds[i].usage);
                return;
            }
        }
        printf("Unknown command: %s (type 'help' for list)\n", argv[1]);
        return;
    }
    printf("Available commands:\n");
    for (uint32_t i = 0; i < g_n_cmds; i++)
        printf("  %-24s  %s\n", g_cmds[i].name, g_cmds[i].help);
    printf("\nType 'help <command>' for detailed usage.\n");
}

/* Forward declarations */
static void cmd_stat(int argc, char **argv);
static void dispatch(char *line);

static void
cmd_stats(int argc, char **argv)
{
    /* Legacy alias: bare "stats" → "stat net" */
    char *stat_argv[] = { (char *)"stat", (char *)"net" };
    (void)argc; (void)argv;
    cmd_stat(2, stat_argv);
}

/* ── Shared stat flags ────────────────────────────────────────────────────── */
typedef struct {
    bool rate;      /* --rate: 1-second delta */
    int  core;      /* --core N (-1 = all) */
} stat_opts_t;

static void
stat_parse_opts(int argc, char **argv, int start, stat_opts_t *opts)
{
    opts->rate  = false;
    opts->core  = -1;
    for (int i = start; i < argc; i++) {
        if (strcmp(argv[i], "--rate") == 0)
            opts->rate = true;
        else if (strcmp(argv[i], "--core") == 0 && i + 1 < argc) {
            opts->core = atoi(argv[++i]);
        }
    }
}

/* ── stat cpu ──────────────────────────────────────────────────────────────── */
static void
stat_cpu(const stat_opts_t *opts)
{
    uint32_t nw = g_core_map.num_workers;
    char buf[8192];

    if (opts->rate) {
        cpu_stats_snapshot_t s1, s2;
        cpu_stats_snapshot(&s1, nw);
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        cpu_stats_snapshot(&s2, nw);

        /* Compute delta */
        cpu_stats_snapshot_t delta;
        memset(&delta, 0, sizeof(delta));
        delta.n_workers = nw;
        delta.tsc_hz = s2.tsc_hz;
        for (uint32_t w = 0; w < nw; w++) {
            delta.per_worker[w].cycles_rx    = s2.per_worker[w].cycles_rx    - s1.per_worker[w].cycles_rx;
            delta.per_worker[w].cycles_tx    = s2.per_worker[w].cycles_tx    - s1.per_worker[w].cycles_tx;
            delta.per_worker[w].cycles_timer = s2.per_worker[w].cycles_timer - s1.per_worker[w].cycles_timer;
            delta.per_worker[w].cycles_ipc   = s2.per_worker[w].cycles_ipc   - s1.per_worker[w].cycles_ipc;
            delta.per_worker[w].cycles_idle  = s2.per_worker[w].cycles_idle  - s1.per_worker[w].cycles_idle;
            delta.per_worker[w].cycles_total = s2.per_worker[w].cycles_total - s1.per_worker[w].cycles_total;
            delta.per_worker[w].loop_count   = s2.per_worker[w].loop_count   - s1.per_worker[w].loop_count;
        }
        export_cpu_text(&delta, opts->core, buf, sizeof(buf));
        printf("%s(1-second sample)\n", buf);
    } else {
        cpu_stats_snapshot_t snap;
        cpu_stats_snapshot(&snap, nw);
        export_cpu_text(&snap, opts->core, buf, sizeof(buf));
        puts(buf);
    }
}

/* ── stat mem ──────────────────────────────────────────────────────────────── */
static void
stat_mem(const stat_opts_t *opts)
{
    uint32_t nw = g_core_map.num_workers;
    char buf[8192];

    if (opts->rate) {
        mem_stats_snapshot_t s1, s2;
        mem_stats_snapshot(&s1, nw);
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        mem_stats_snapshot(&s2, nw);

        /* Print delta for in-use mbufs and connections */
        printf("--- memory rates (1-second delta) ---\n");
        for (uint32_t i = 0; i < s2.n_pools; i++) {
            if (opts->core >= 0 && (int)i != opts->core) continue;
            int32_t d = (int32_t)s2.pools[i].in_use - (int32_t)s1.pools[i].in_use;
            printf("  %-12s mbuf churn: %+d/s\n", s2.pools[i].name, d);
        }
        for (uint32_t i = 0; i < s2.n_tcbs; i++) {
            if (opts->core >= 0 && (int)i != opts->core) continue;
            int32_t d = (int32_t)s2.tcbs[i].active - (int32_t)s1.tcbs[i].active;
            printf("  W%-11u conn churn: %+d/s\n", i, d);
        }
    } else {
        mem_stats_snapshot_t snap;
        mem_stats_snapshot(&snap, nw);
        export_mem_text(&snap, opts->core, buf, sizeof(buf));
        puts(buf);
    }
}

/* ── stat net ──────────────────────────────────────────────────────────────── */
static void
stat_net(const stat_opts_t *opts)
{
    uint32_t nw = g_core_map.num_workers;
    char buf[16384];

    if (opts->core >= 0) {
        /* Per-worker breakdown with TCP state */
        if (opts->rate) {
            metrics_snapshot_t s1, s2;
            metrics_snapshot(&s1, nw);
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&ts, NULL);
            metrics_snapshot(&s2, nw);

            uint32_t c = (uint32_t)opts->core;
            if (c < nw) {
                const worker_metrics_t *w1 = &s1.per_worker[c];
                const worker_metrics_t *w2 = &s2.per_worker[c];
                printf("--- worker %u rates (1-second sample) ---\n", c);
                printf("  TX: %"PRIu64" pps   %.1f Mbps\n",
                       w2->tx_pkts - w1->tx_pkts,
                       (double)(w2->tx_bytes - w1->tx_bytes) * 8.0 / 1e6);
                printf("  RX: %"PRIu64" pps   %.1f Mbps\n",
                       w2->rx_pkts - w1->rx_pkts,
                       (double)(w2->rx_bytes - w1->rx_bytes) * 8.0 / 1e6);
                if (w2->tcp_conn_open > w1->tcp_conn_open)
                    printf("  TCP conn/s: %"PRIu64"\n",
                           w2->tcp_conn_open - w1->tcp_conn_open);
            }
        } else {
            metrics_snapshot_t snap;
            metrics_snapshot(&snap, nw);
            export_net_core_text(&snap, (uint32_t)opts->core, buf, sizeof(buf));
            puts(buf);
        }
    } else if (opts->rate) {
        metrics_snapshot_t s1, s2;
        metrics_snapshot(&s1, nw);
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        metrics_snapshot(&s2, nw);

        const worker_metrics_t *t1 = &s1.total;
        const worker_metrics_t *t2 = &s2.total;
        printf("--- rates (1-second sample) ---\n");
        printf("  TX: %"PRIu64" pps   %.1f Mbps\n",
               t2->tx_pkts - t1->tx_pkts,
               (double)(t2->tx_bytes - t1->tx_bytes) * 8.0 / 1e6);
        printf("  RX: %"PRIu64" pps   %.1f Mbps\n",
               t2->rx_pkts - t1->rx_pkts,
               (double)(t2->rx_bytes - t1->rx_bytes) * 8.0 / 1e6);
        if (t2->tcp_conn_open > t1->tcp_conn_open)
            printf("  TCP conn/s: %"PRIu64"   close/s: %"PRIu64"\n",
                   t2->tcp_conn_open - t1->tcp_conn_open,
                   t2->tcp_conn_close - t1->tcp_conn_close);
        if (t2->http_req_tx > t1->http_req_tx)
            printf("  HTTP req/s: %"PRIu64"   rsp/s: %"PRIu64"\n",
                   t2->http_req_tx - t1->http_req_tx,
                   t2->http_rsp_rx - t1->http_rsp_rx);
    } else {
        /* Same as old "stats" — JSON dump */
        cli_print_stats();
    }
}

/* ── stat port ─────────────────────────────────────────────────────────────── */
static void
stat_port(const stat_opts_t *opts)
{
    char buf[8192];

    if (opts->rate) {
        struct rte_eth_stats s1[TGEN_MAX_PORTS], s2[TGEN_MAX_PORTS];
        uint32_t np = g_n_ports;
        for (uint32_t i = 0; i < np; i++)
            rte_eth_stats_get(i, &s1[i]);

        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);

        for (uint32_t i = 0; i < np; i++)
            rte_eth_stats_get(i, &s2[i]);

        printf("─────────────────────────────────────────────────────────────────────\n"
               "Port  Link       RX pps     RX Mbps    RX miss/s   TX pps    TX Mbps\n"
               "─────────────────────────────────────────────────────────────────────\n");
        for (uint32_t i = 0; i < np; i++) {
            struct rte_eth_link link;
            int link_rc = rte_eth_link_get_nowait(i, &link);
            if (link_rc < 0)
                memset(&link, 0, sizeof(link));

            char link_str[32];
            if (link.link_status)
                snprintf(link_str, sizeof(link_str), "UP %uG",
                         link.link_speed / 1000);
            else
                snprintf(link_str, sizeof(link_str), "DOWN");

            uint64_t rx_pps = s2[i].ipackets - s1[i].ipackets;
            double rx_mbps = (double)(s2[i].ibytes - s1[i].ibytes) * 8.0 / 1e6;
            uint64_t rx_miss = s2[i].imissed - s1[i].imissed;
            uint64_t tx_pps = s2[i].opackets - s1[i].opackets;
            double tx_mbps = (double)(s2[i].obytes - s1[i].obytes) * 8.0 / 1e6;

            printf("%-5u %-10s %-10"PRIu64" %-10.1f %-11"PRIu64" %-9"PRIu64" %.1f\n",
                   i, link_str, rx_pps, rx_mbps, rx_miss, tx_pps, tx_mbps);
        }
        printf("─────────────────────────────────────────────────────────────────────\n"
               "(1-second sample)\n");
    } else {
        export_port_text(buf, sizeof(buf));
        puts(buf);
    }
}

/* ── stat (dispatcher) ─────────────────────────────────────────────────────── */
static void
cmd_stat(int argc, char **argv)
{
    stat_opts_t opts;

    if (argc < 2) {
        /* Bare "stat" → summary */
        stat_parse_opts(argc, argv, 2, &opts);

        uint32_t nw = g_core_map.num_workers;
        cpu_stats_snapshot_t cpu;
        mem_stats_snapshot_t mem;
        metrics_snapshot_t   net;

        cpu_stats_snapshot(&cpu, nw);
        mem_stats_snapshot(&mem, nw);
        metrics_snapshot(&net, nw);

        char buf[8192];
        export_stat_summary(&cpu, &mem, &net, buf, sizeof(buf));
        puts(buf);
        return;
    }

    const char *sub = argv[1];

    /* Check for flags directly after "stat" (no sub-command) */
    if (sub[0] == '-') {
        /* Treat as "stat" with flags */
        stat_parse_opts(argc, argv, 1, &opts);
        uint32_t nw = g_core_map.num_workers;
        cpu_stats_snapshot_t cpu;
        mem_stats_snapshot_t mem;
        metrics_snapshot_t   net;
        cpu_stats_snapshot(&cpu, nw);
        mem_stats_snapshot(&mem, nw);
        metrics_snapshot(&net, nw);
        char buf[8192];
        export_stat_summary(&cpu, &mem, &net, buf, sizeof(buf));
        puts(buf);
        return;
    }

    stat_parse_opts(argc, argv, 2, &opts);

    if (strcmp(sub, "cpu") == 0)       stat_cpu(&opts);
    else if (strcmp(sub, "mem") == 0)  stat_mem(&opts);
    else if (strcmp(sub, "net") == 0)  stat_net(&opts);
    else if (strcmp(sub, "port") == 0) stat_port(&opts);
    else printf("Unknown stat sub-command: %s\n"
                "Usage: stat [cpu|mem|net|port] [--rate] [--core N]\n",
                sub);
}

static void
cmd_trace(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: trace start <file.pcapng> [port=0] [queue=0]\n"
               "       trace stop\n");
        return;
    }
    if (strcmp(argv[1], "start") == 0) {
        if (argc < 3) {
            printf("Usage: trace start <file.pcapng> [port=0] [queue=0]\n");
            return;
        }
        const char *path = argv[2];
        uint16_t port  = (argc >= 4) ? (uint16_t)strtoul(argv[3], NULL, 10) : 0;
        uint16_t queue = (argc >= 5) ? (uint16_t)strtoul(argv[4], NULL, 10) : 0;
        int rc = pktrace_start(port, queue, 0, path);
        if (rc == 0)
            printf("Capture started on port %u queue %u → %s (unlimited)\n",
                   port, queue, path);
        else
            printf("trace start failed: %s\n", strerror(-rc));
    } else if (strcmp(argv[1], "stop") == 0) {
        if (!pktrace_is_active()) {
            printf("No active capture.\n");
            return;
        }
        uint32_t count = pktrace_count();
        pktrace_stop();
        printf("Capture stopped (%u packets captured)\n", count);
    } else {
        printf("Unknown trace sub-command '%s'\n", argv[1]);
    }
}

/* Callback for icmp_ping_start to allow remote CLI during ping */
static void
ping_poll_cb(void)
{
    pktrace_flush();
    cli_server_poll(dispatch);
}

static void
cmd_ping(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: ping <dst_ip> [count=5] [size=56] [interval_ms=1000] [port=0]\n");
        return;
    }
    uint32_t dst_ip = 0;
    if (tgen_parse_ipv4(argv[1], &dst_ip) < 0) {
        printf("ping: invalid IP address '%s'\n", argv[1]);
        return;
    }
    uint32_t count       = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 5;
    uint32_t size        = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 56;
    uint32_t interval_ms = (argc >= 5) ? (uint32_t)strtoul(argv[4], NULL, 10) : 1000;
    uint16_t port_id     = (argc >= 6) ? (uint16_t)strtoul(argv[5], NULL, 10) : 0;
    if (port_id >= g_n_ports) {
        printf("ping: port %u does not exist (have %u port(s))\n",
               port_id, g_n_ports);
        return;
    }
    printf("PING %s (port %u): %u bytes of data, %u packet(s)\n",
           argv[1], port_id, size, count);
    icmp_ping_start(port_id, dst_ip, count, size, interval_ms, ping_poll_cb);
}

/* ------------------------------------------------------------------ */
/* start — unified traffic generation (replaces tps + throughput)       */
/* ------------------------------------------------------------------ */

/* Parsed flags for the start command */
typedef struct {
    const char *ip;
    const char *proto;
    const char *url;
    const char *host;
    uint16_t    port;
    uint32_t    duration;
    uint64_t    rate;
    uint16_t    size;
    uint32_t    streams;
    uint32_t    ramp;       /* --ramp: ramp-up seconds (0 = instant) */
    uint32_t    txn_per_conn;  /* --txn-per-conn: HTTP txns per connection */
    uint32_t    think_time;    /* --think-time: ms between transactions */
    bool        reuse;
    bool        tls;
    bool        one;        /* --one: single request/handshake/connection */
    bool        has_ip;
    bool        has_port;
    bool        has_duration;
} start_args_t;

static const char *
start_usage(void)
{
    return "Usage: start --ip <addr> --port <N> --duration <secs>\n"
           "             [--proto tcp|http|https|udp|icmp|tls]\n"
           "             [--rate <pps>] [--cps <N>] [--ramp <secs>]\n"
           "             [--size <bytes>] [--reuse] [--streams <N>]\n"
           "             [--url <path>] [--host <name>] [--tls]\n"
           "             [--one]  (single request; mutually exclusive with --duration/--rate)\n";
}

static int
parse_start_args(int argc, char **argv, start_args_t *a)
{
    memset(a, 0, sizeof(*a));
    a->proto = "tcp";
    a->url   = "/";
    a->size  = 56;
    a->streams = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            a->ip = argv[++i]; a->has_ip = true;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a->port = (uint16_t)strtoul(argv[++i], NULL, 10); a->has_port = true;
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            a->duration = (uint32_t)strtoul(argv[++i], NULL, 10); a->has_duration = true;
        } else if (strcmp(argv[i], "--proto") == 0 && i + 1 < argc) {
            a->proto = argv[++i];
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            a->rate = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--cps") == 0 && i + 1 < argc) {
            a->rate = strtoull(argv[++i], NULL, 10); /* alias for --rate */
        } else if (strcmp(argv[i], "--ramp") == 0 && i + 1 < argc) {
            a->ramp = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--txn-per-conn") == 0 && i + 1 < argc) {
            a->txn_per_conn = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--think-time") == 0 && i + 1 < argc) {
            a->think_time = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            a->size = (uint16_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--streams") == 0 && i + 1 < argc) {
            a->streams = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            a->url = argv[++i];
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            a->host = argv[++i];
        } else if (strcmp(argv[i], "--reuse") == 0) {
            a->reuse = true;
        } else if (strcmp(argv[i], "--tls") == 0) {
            a->tls = true;
        } else if (strcmp(argv[i], "--one") == 0) {
            a->one = true;
        } else {
            printf("Unknown flag: %s\n", argv[i]);
            return -1;
        }
    }

    /* Auto-enable TLS for https/tls protocols */
    if (strcmp(a->proto, "https") == 0 || strcmp(a->proto, "tls") == 0)
        a->tls = true;

    /* Default host to IP if not specified */
    if (!a->host)
        a->host = a->ip;

    return 0;
}

static tx_gen_proto_t
start_resolve_proto(const start_args_t *a)
{
    if (a->reuse)
        return TX_GEN_PROTO_THROUGHPUT;
    if (strcmp(a->proto, "icmp") == 0)
        return TX_GEN_PROTO_ICMP;
    if (strcmp(a->proto, "udp") == 0)
        return TX_GEN_PROTO_UDP;
    if (strcmp(a->proto, "http") == 0 || strcmp(a->proto, "https") == 0)
        return TX_GEN_PROTO_HTTP;
    return TX_GEN_PROTO_TCP_SYN; /* tcp, tls */
}

static void
cmd_start(int argc, char **argv)
{
    extern volatile int g_run;

    if (argc < 2) { printf("%s", start_usage()); return; }

    start_args_t a;
    if (parse_start_args(argc, argv, &a) < 0) {
        printf("%s", start_usage());
        return;
    }

    /* Validate required flags */
    if (!a.has_ip)       { printf("start: --ip is required\n%s",       start_usage()); return; }
    if (!a.has_port)     { printf("start: --port is required\n%s",     start_usage()); return; }

    /* --one is mutually exclusive with --duration and --rate */
    if (a.one) {
        if (a.has_duration || a.rate) {
            printf("start: --one is mutually exclusive with --duration and --rate\n");
            return;
        }
        a.duration     = 10;  /* safety timeout */
        a.rate         = 1;
        a.has_duration = true;
    }

    if (!a.has_duration) { printf("start: --duration is required (or use --one)\n%s", start_usage()); return; }
    if (a.duration == 0) { printf("start: --duration must be > 0\n");  return; }

    /* Reject if traffic already running */
    if (g_traffic_state.active) {
        printf("start: traffic generation already active — run 'stop' first\n");
        return;
    }

    /* Parse destination IP */
    uint32_t dst_ip;
    if (tgen_parse_ipv4(a.ip, &dst_ip) < 0) {
        printf("start: invalid IP '%s'\n", a.ip);
        return;
    }

    tx_gen_proto_t proto = start_resolve_proto(&a);

    /* Select egress port: scan all configured ports, prefer the one whose
     * subnet contains dst_ip (on-link), fall back to any port with a
     * default gateway, then port 0. */
    uint16_t port_id = 0;
    bool port_matched = false;
    for (uint16_t p = 0; p < g_n_ports && !port_matched; p++) {
        rte_rwlock_read_lock(&g_arp[p].lock);
        uint32_t lip  = g_arp[p].local_ip;
        uint32_t lmask = g_arp[p].netmask;
        rte_rwlock_read_unlock(&g_arp[p].lock);
        if (lip && lmask && (dst_ip & lmask) == (lip & lmask)) {
            port_id = p;
            port_matched = true;
        }
    }
    if (!port_matched) {
        /* No on-link match — use the first port with a gateway configured */
        for (uint16_t p = 0; p < g_n_ports; p++) {
            rte_rwlock_read_lock(&g_arp[p].lock);
            uint32_t gw = g_arp[p].gateway_ip;
            rte_rwlock_read_unlock(&g_arp[p].lock);
            if (gw != 0) { port_id = p; break; }
        }
    }

    /* Clamp streams */
    if (a.streams > 16) a.streams = 16;
    if (a.streams == 0) a.streams = 1;

    /* ── ARP-resolve destination ────────────────────────────────────── */
    struct rte_ether_addr dst_mac;
    uint32_t nexthop = arp_nexthop(port_id, dst_ip);
    if (!arp_lookup(port_id, nexthop, &dst_mac)) {
        arp_request(port_id, nexthop);
        uint64_t deadline = rte_rdtsc() + 3ULL * rte_get_tsc_hz();
        while (rte_rdtsc() < deadline) {
            arp_mgmt_tick();
            pktrace_flush();          /* drain capture ring to avoid drops */
            if (arp_lookup(port_id, nexthop, &dst_mac)) break;
            mgmt_delay_ms_flush(10);
        }
    }
    if (!arp_lookup(port_id, nexthop, &dst_mac)) {
        printf("start: ARP resolution failed for %s\n", a.ip);
        return;
    }

    /* ── Build TX-gen config ────────────────────────────────────────── */
    tx_gen_config_t gcfg;
    memset(&gcfg, 0, sizeof(gcfg));
    gcfg.proto      = proto;
    gcfg.dst_ip     = dst_ip;
    gcfg.src_ip     = g_arp[port_id].local_ip;
    gcfg.dst_mac    = dst_mac;
    gcfg.src_mac    = g_arp[port_id].local_mac;
    gcfg.dst_port   = a.port;
    gcfg.src_port   = 12345;
    gcfg.pkt_size   = a.size;
    gcfg.port_id    = port_id;
    gcfg.rate_pps   = a.rate;
    gcfg.duration_s = a.duration;
    gcfg.ramp_s         = a.ramp;
    gcfg.txn_per_conn   = a.txn_per_conn;
    gcfg.think_time_us  = a.think_time * 1000; /* ms → µs */
    gcfg.max_initiations = a.one ? 1 : 0;
    gcfg.enable_tls = a.tls;

    if (a.reuse)
        gcfg.throughput_streams = (uint8_t)a.streams;

    /* HTTP-specific config */
    if (proto == TX_GEN_PROTO_HTTP) {
        gcfg.http_method = 0; /* GET */
        strncpy(gcfg.http_url,  a.url,  sizeof(gcfg.http_url) - 1);
        strncpy(gcfg.http_host, a.host, sizeof(gcfg.http_host) - 1);
    }

    /* ── Reset counters & push to workers ───────────────────────────── */
    uint32_t n_workers = g_core_map.num_workers;
    metrics_reset(n_workers);
    rte_eth_stats_reset(port_id);

    /* Apply RSS queue affinity so responses land on the correct worker */
    if (proto == TX_GEN_PROTO_TCP_SYN || proto == TX_GEN_PROTO_HTTP ||
        proto == TX_GEN_PROTO_THROUGHPUT) {
        struct rte_eth_dev_info dev_info;
        int _ri = rte_eth_dev_info_get(port_id, &dev_info); (void)_ri;
        uint16_t n_rxq = dev_info.nb_rx_queues;
        port_caps_t *pcap = &g_port_caps[port_id];
        uint8_t key_len = pcap->rss_key_size;
        if (key_len == 0 || key_len > tgen_rss_key_max_len())
            key_len = 40;
        /* Reset pools first then filter */
        for (uint32_t w = 0; w < n_workers; w++)
            tcp_port_pool_reset(w);
        tcp_port_pool_apply_rss_filter(n_workers, gcfg.src_ip, dst_ip,
                                       a.port, tgen_rss_key(), key_len,
                                       n_rxq);
    }

    /* ── Broadcast START command to all workers ───────────────────── */
    config_update_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = CFG_CMD_START;
    cmd.seq = 1;
    memcpy(cmd.payload, &gcfg, sizeof(gcfg));
    /* --one: only one worker should initiate the connection. Worker 0 is
     * chosen because tcp_port_pool_apply_rss_filter has already seeded
     * its pool with ports whose RSS hash routes responses back to queue 0.
     * Broadcasting max_initiations=1 to every worker would send N SYNs. */
    if (a.one)
        tgen_ipc_send(0, &cmd);
    else
        tgen_ipc_broadcast(&cmd);

    /* ── Print initial progress ──────────────────────────────────────── */
    if (a.reuse) {
        printf("Traffic %s → %s:%u  streams=%u  duration=%us%s\n",
               a.proto, a.ip, a.port, a.streams, a.duration,
               a.tls ? " [TLS]" : "");
        printf("[ ID]  Interval       Transfer     Throughput\n");
    } else if (a.one) {
        printf("Single %s → %s:%u%s\n",
               a.proto, a.ip, a.port, a.tls ? " [TLS]" : "");
    } else {
        printf("Traffic %s → %s:%u  %u-byte payload, %s, %u seconds%s\n",
               a.proto, a.ip, a.port, a.size,
               a.rate ? "rate-limited" : "unlimited",
               a.duration, a.tls ? " [TLS]" : "");
    }

    /* ── Set up async traffic gen state ──────────────────────────────── */
    traffic_gen_state_t tgs;
    memset(&tgs, 0, sizeof(tgs));
    tgs.one_shot   = a.one;
    tgs.reuse      = a.reuse;
    tgs.is_tty     = isatty(STDOUT_FILENO);
    tgs.duration_s = a.duration;
    tgs.n_workers  = n_workers;
    tgs.port_id    = port_id;
    tgs.rate       = a.rate;
    tgs.size       = a.size;
    tgs.tls        = a.tls;
    tgs.streams    = a.streams;
    strncpy(tgs.proto, a.proto, sizeof(tgs.proto) - 1);

    /* --one blocks until the single request completes (or times out),
     * then prints the result and returns the prompt. */
    bool pipe_mode = !isatty(STDIN_FILENO);
    (void)pipe_mode;
    if (a.one) {
        mgmt_traffic_start(&tgs);
        uint64_t hz        = rte_get_tsc_hz();
        uint64_t chk_itvl  = hz / 100;              /* 10 ms in TSC ticks  */
        uint64_t last_chk   = rte_rdtsc() - chk_itvl; /* check on 1st iter */
        while (g_traffic_state.active && g_run) {
            /* Tight loop: keep ARP/ICMP alive and drain the capture ring
             * on every iteration so pktrace never starves on busy ports. */
            arp_mgmt_tick();
            icmp_mgmt_tick();
            pktrace_flush();
            cli_server_poll(dispatch);
            /* Throttle the expensive completion/timeout check to 10 ms */
            uint64_t now = rte_rdtsc();
            if ((now - last_chk) >= chk_itvl) {
                last_chk = now;
                uint64_t elapsed_s =
                    (now - g_traffic_state.start_tsc) / hz;
                if (elapsed_s >= g_traffic_state.duration_s) {
                    mgmt_traffic_stop();
                    break;
                }
                if (g_traffic_state.one_shot) {
                    metrics_snapshot_t ms;
                    metrics_snapshot(&ms, n_workers);
                    bool done = false;
                    if (strcmp(a.proto, "icmp") == 0 ||
                        strcmp(a.proto, "udp") == 0)
                        done = (ms.total.tx_pkts >= 1);
                    else if (strcmp(a.proto, "http") == 0 ||
                             strcmp(a.proto, "https") == 0)
                        /* Done when the connection closes cleanly after the full
                         * response body is received.  The FSM sends FIN once
                         * Content-Length bytes have arrived (active close), so
                         * tcp_conn_close fires quickly.  Falls back to reset
                         * if Content-Length is absent (chunked) or on error.
                         * http_rsp_rx >= 1 is an additional safety exit so we
                         * never hang longer than needed on unusual responses. */
                        done = (ms.total.http_rsp_rx >= 1 &&
                                ms.total.tcp_conn_close >= 1) ||
                               ms.total.tcp_reset_sent >= 1 ||
                               ms.total.tcp_reset_rx >= 1;
                    else
                        done = ms.total.tcp_conn_close >= 1 ||
                               ms.total.tcp_reset_sent >= 1 ||
                               ms.total.tcp_reset_rx >= 1;
                    if (done) { mgmt_traffic_stop(); break; }
                }
            }
            rte_pause(); /* yield pipeline; matches mgmt_loop_run() behavior */
        }
        return;
    }

    /* Interactive mode: non-blocking — return to prompt immediately */
    mgmt_traffic_start(&tgs);
    printf("(running in background — use 'stat' to monitor, 'stop' to abort)\n");
}

static void
cmd_stop_gen(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (g_traffic_state.active) {
        mgmt_traffic_stop();
        printf("Traffic generation stopped.\n");
    } else {
        printf("It's already in stopped state.\n");
    }
}

/* ── Reset all TCP state ─────────────────────────────────────────────────── */
static void
cmd_reset(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Refuse if traffic generation is still active */
    uint32_t n_workers = g_core_map.num_workers;
    for (uint32_t w = 0; w < n_workers; w++) {
        if (__atomic_load_n(&g_worker_ctx[w].tx_gen.active,
                            __ATOMIC_ACQUIRE)) {
            printf("reset: traffic generation is still active — "
                   "run 'stop' first.\n");
            return;
        }
    }

    /* Send RST for all active TCBs so the remote side also cleans up.
     * Batch RSTs to avoid TX ring overflow (ring size is typically 2048). */
    for (uint32_t w = 0; w < n_workers; w++) {
        tcb_store_t *store = &g_tcb_stores[w];
        uint32_t batch = 0;
        for (uint32_t i = 0; i < store->capacity; i++) {
            tcb_t *tcb = &store->tcbs[i];
            if (tcb->in_use) {
                tcp_fsm_reset(w, tcb);
                batch++;
                if (batch % 1024 == 0)
                    mgmt_delay_ms_flush(5);
            }
        }
    }

    /* Wait for RSTs to be transmitted and remote side to process them */
    mgmt_delay_ms_flush(200);

    /* Full reset of TCB stores — clears tombstones left by tcb_free */
    for (uint32_t w = 0; w < n_workers; w++)
        tcb_store_reset(&g_tcb_stores[w]);

    /* Reset port pools (but preserve cursor so we don't reuse recent ports) */
    for (uint32_t w = 0; w < n_workers; w++)
        tcp_port_pool_reset(w);

    /* Reset metrics */
    metrics_reset(n_workers);

    printf("TCP state reset: all connections closed, ports freed, metrics cleared.\n");
}

/* ── Set runtime configuration ───────────────────────────────────────────── */
static void
cmd_set(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: set ip <port> <ip> <gateway> <netmask>\n"
               "       set rate <pps>   (change rate for running test)\n");
        return;
    }

    /* set rate <pps> — dynamic rate change for running test */
    if (strcmp(argv[1], "rate") == 0) {
        if (argc < 3) {
            printf("Usage: set rate <pps>\n");
            return;
        }
        uint64_t new_rate = strtoull(argv[2], NULL, 10);
        config_update_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmd = CFG_CMD_SET_RATE;
        memcpy(cmd.payload, &new_rate, sizeof(new_rate));
        tgen_ipc_broadcast(&cmd);
        printf("Rate set to %lu pps\n", (unsigned long)new_rate);
        return;
    }

    if (strcmp(argv[1], "ip") != 0) {
        printf("Usage: set ip <port> <ip> <gateway> <netmask>\n"
               "       set rate <pps>   (change rate for running test)\n");
        return;
    }

    if (argc < 6) {
        printf("Usage: set ip <port> <ip> <gateway> <netmask>\n");
        return;
    }

    uint16_t port_id = (uint16_t)strtoul(argv[2], NULL, 10);
    if (port_id >= g_n_ports) {
        printf("set ip: port %u does not exist (have %u port(s))\n",
               port_id, g_n_ports);
        return;
    }

    uint32_t new_ip = 0, new_gw = 0, new_mask = 0;
    if (tgen_parse_ipv4(argv[3], &new_ip) < 0) {
        printf("set ip: invalid IP '%s'\n", argv[3]);
        return;
    }
    if (tgen_parse_ipv4(argv[4], &new_gw) < 0) {
        printf("set ip: invalid gateway '%s'\n", argv[4]);
        return;
    }
    if (tgen_parse_ipv4(argv[5], &new_mask) < 0) {
        printf("set ip: invalid netmask '%s'\n", argv[5]);
        return;
    }

    /* Validate: gateway must be on the same subnet as the new IP.
     * 0.0.0.0 means "no gateway" (direct-link, same-subnet only). */
    if (new_gw != 0 && (new_ip & new_mask) != (new_gw & new_mask)) {
        printf("set ip: gateway is not on the same subnet as the IP\n");
        return;
    }

    rte_rwlock_write_lock(&g_arp[port_id].lock);
    g_arp[port_id].local_ip   = new_ip;
    g_arp[port_id].gateway_ip = new_gw;
    g_arp[port_id].netmask    = new_mask;
    rte_rwlock_write_unlock(&g_arp[port_id].lock);

    char ip_buf[INET_ADDRSTRLEN], gw_buf[INET_ADDRSTRLEN], nm_buf[INET_ADDRSTRLEN];
    tgen_ipv4_str(new_ip,   ip_buf, sizeof(ip_buf));
    tgen_ipv4_str(new_gw,   gw_buf, sizeof(gw_buf));
    tgen_ipv4_str(new_mask, nm_buf, sizeof(nm_buf));
    printf("Port %u: ip %s  gateway %s  netmask %s\n",
           port_id, ip_buf, gw_buf, nm_buf);
}

/* Forward declarations for show sub-commands */
static void cmd_listeners(int argc, char **argv);
static void cmd_connections(int argc, char **argv);

/* ── Show interface details ──────────────────────────────────────────────── */
static void
cmd_show(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: show interface [port_id]\n");
        if (g_config.server_mode)
            printf("       show listeners\n"
                   "       show connections\n");
        return;
    }

    if (strcmp(argv[1], "listeners") == 0) {
        cmd_listeners(argc - 1, argv + 1);
        return;
    }
    if (strcmp(argv[1], "connections") == 0) {
        cmd_connections(argc - 1, argv + 1);
        return;
    }

    if (strcmp(argv[1], "interface") != 0) {
        printf("Usage: show interface [port_id]\n");
        if (g_config.server_mode)
            printf("       show listeners\n"
                   "       show connections\n");
        return;
    }

    uint32_t start = 0, end = g_n_ports;
    if (argc >= 3) {
        uint32_t p = (uint32_t)strtoul(argv[2], NULL, 10);
        if (p >= g_n_ports) {
            printf("Port %u does not exist (have %u port(s))\n", p, g_n_ports);
            return;
        }
        start = p;
        end   = p + 1;
    }

    for (uint32_t p = start; p < end; p++) {
        port_caps_t *c = &g_port_caps[p];

        /* Link status */
        struct rte_eth_link link;
        int link_rc = rte_eth_link_get_nowait(p, &link);
        if (link_rc < 0)
            memset(&link, 0, sizeof(link));

        /* Packet stats */
        struct rte_eth_stats stats;
        rte_eth_stats_get(p, &stats);

        /* MAC */
        char mac_buf[18];
        tgen_mac_str(c->mac_addr.addr_bytes, mac_buf, sizeof(mac_buf));

        /* IP from ARP state */
        char ip_buf[INET_ADDRSTRLEN];
        if (g_arp[p].local_ip)
            tgen_ipv4_str(g_arp[p].local_ip, ip_buf, sizeof(ip_buf));
        else
            snprintf(ip_buf, sizeof(ip_buf), "not set");

        char gw_buf[INET_ADDRSTRLEN], nm_buf[INET_ADDRSTRLEN];
        if (g_arp[p].gateway_ip)
            tgen_ipv4_str(g_arp[p].gateway_ip, gw_buf, sizeof(gw_buf));
        else
            snprintf(gw_buf, sizeof(gw_buf), "not set");
        if (g_arp[p].netmask)
            tgen_ipv4_str(g_arp[p].netmask, nm_buf, sizeof(nm_buf));
        else
            snprintf(nm_buf, sizeof(nm_buf), "not set");

        printf("Port %u\n", p);
        printf("  Driver:      %s\n", c->driver_name);
        printf("  MAC:         %s\n", mac_buf);
        printf("  IP:          %s\n", ip_buf);
        printf("  Gateway:     %s\n", gw_buf);
        printf("  Netmask:     %s\n", nm_buf);
        printf("  Link:        %s  %u Mbps  %s\n",
               link.link_status ? "UP" : "DOWN",
               link.link_speed,
               link.link_duplex ? "full-duplex" : "half-duplex");
        printf("  NUMA socket: %u\n", c->socket_id);
        printf("  Mgmt TX Q:   %u\n", c->mgmt_tx_q);
        printf("  Offloads:    IPv4-cksum=%s TCP-cksum=%s UDP-cksum=%s\n"
               "               RSS=%s scatter=%s multi-seg=%s VLAN=%s\n",
               c->has_ipv4_cksum_offload ? "yes" : "no",
               c->has_tcp_cksum_offload  ? "yes" : "no",
               c->has_udp_cksum_offload  ? "yes" : "no",
               c->has_rss          ? "yes" : "no",
               c->has_scatter_rx   ? "yes" : "no",
               c->has_multi_seg_tx ? "yes" : "no",
               c->has_vlan_offload ? "yes" : "no");
        printf("  Statistics:\n"
               "    RX packets: %" PRIu64 "  bytes: %" PRIu64
               "  missed: %" PRIu64 "  errors: %" PRIu64 "\n"
               "    TX packets: %" PRIu64 "  bytes: %" PRIu64
               "  errors: %" PRIu64 "\n",
               stats.ipackets, stats.ibytes, stats.imissed, stats.ierrors,
               stats.opackets, stats.obytes, stats.oerrors);
        if (p + 1 < end)
            printf("\n");
    }
}

/* ── Server mode: serve command ───────────────────────────────────────────── */
static int
parse_listen_spec(const char *spec, srv_listen_spec_t *out)
{
    /* Format: proto:port[:handler]
     * e.g., tcp:5000:echo, http:80, https:443, tls:4433:echo */
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);

    char *proto_s = strtok(buf, ":");
    char *port_s  = strtok(NULL, ":");
    char *hdl_s   = strtok(NULL, ":");

    if (!proto_s || !port_s) return -1;

    out->port = (uint16_t)atoi(port_s);
    if (out->port == 0) return -1;

    if (strcmp(proto_s, "tcp") == 0) {
        if (!hdl_s || strcmp(hdl_s, "echo") == 0)
            out->handler = SRV_HANDLER_ECHO;
        else if (strcmp(hdl_s, "discard") == 0)
            out->handler = SRV_HANDLER_DISCARD;
        else if (strcmp(hdl_s, "chargen") == 0)
            out->handler = SRV_HANDLER_CHARGEN;
        else return -1;
    } else if (strcmp(proto_s, "http") == 0) {
        out->handler = SRV_HANDLER_HTTP;
    } else if (strcmp(proto_s, "https") == 0) {
        out->handler = SRV_HANDLER_HTTPS;
    } else if (strcmp(proto_s, "tls") == 0) {
        if (!hdl_s || strcmp(hdl_s, "echo") == 0)
            out->handler = SRV_HANDLER_TLS_ECHO;
        else return -1;
    } else {
        return -1;
    }
    return 0;
}

static void
cmd_serve(int argc, char **argv)
{
    if (!g_config.server_mode) {
        printf("serve: not available in client mode (use 'start' instead)\n");
        return;
    }

    srv_ipc_payload_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.http_body_size = 1024; /* default */

    const char *tls_cert = NULL;
    const char *tls_key  = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
            i++;
            if (cfg.count >= SRV_MAX_LISTENERS) {
                printf("serve: max %d listeners\n", SRV_MAX_LISTENERS);
                return;
            }
            if (parse_listen_spec(argv[i], &cfg.specs[cfg.count]) < 0) {
                printf("serve: invalid spec '%s'\n"
                       "  Format: proto:port[:handler]\n"
                       "  e.g., tcp:5000:echo, http:80, https:443\n",
                       argv[i]);
                return;
            }
            cfg.count++;
        } else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc) {
            tls_cert = argv[++i];
        } else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc) {
            tls_key = argv[++i];
        } else if (strcmp(argv[i], "--http-body-size") == 0 && i + 1 < argc) {
            cfg.http_body_size = (uint32_t)atoi(argv[++i]);
        } else {
            printf("serve: unknown option '%s'\n", argv[i]);
            return;
        }
    }

    if (cfg.count == 0) {
        printf("serve: at least one --listen spec required\n");
        return;
    }

    /* Check TLS requirements */
    bool needs_tls = false;
    for (uint32_t i = 0; i < cfg.count; i++) {
        if (cfg.specs[i].handler == SRV_HANDLER_HTTPS ||
            cfg.specs[i].handler == SRV_HANDLER_TLS_ECHO) {
            needs_tls = true;
            break;
        }
    }
    if (needs_tls && (!tls_cert || !tls_key)) {
        printf("serve: --tls-cert and --tls-key required for https/tls listeners\n");
        return;
    }

    /* Store TLS cert/key paths globally */
    if (tls_cert)
        snprintf(g_srv_tls_cert_path, sizeof(g_srv_tls_cert_path), "%s", tls_cert);
    if (tls_key)
        snprintf(g_srv_tls_key_path, sizeof(g_srv_tls_key_path), "%s", tls_key);

    /* Initialise server TLS context with the provided cert/key */
    if (needs_tls) {
        if (tls_server_ctx_load(tls_cert, tls_key) < 0) {
            printf("serve: failed to load TLS certificate/key\n");
            return;
        }
    }

    /* Save mgmt-side shadow for listeners display */
    g_srv_active_cfg = cfg;
    g_srv_active = true;

    /* Broadcast to all workers */
    config_update_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = CFG_CMD_SERVE;
    memcpy(cmd.payload, &cfg, sizeof(cfg));
    tgen_ipc_broadcast(&cmd);

    printf("Listening on %u endpoint(s):\n", cfg.count);
    for (uint32_t i = 0; i < cfg.count; i++) {
        const char *hname = srv_handler_name(cfg.specs[i].handler);
        /* Reconstruct spec string */
        const char *proto = "tcp";
        if (cfg.specs[i].handler == SRV_HANDLER_HTTP)     proto = "http";
        if (cfg.specs[i].handler == SRV_HANDLER_HTTPS)    proto = "https";
        if (cfg.specs[i].handler == SRV_HANDLER_TLS_ECHO) proto = "tls";
        printf("  #%u  %s:%u:%s\n", i, proto, cfg.specs[i].port, hname);
    }
}

/* ── Server mode: listeners command ──────────────────────────────────────── */
static void
cmd_listeners(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!g_config.server_mode) {
        printf("listeners: not available in client mode\n");
        return;
    }

    /* Read from mgmt-side shadow config */
    if (!g_srv_active || g_srv_active_cfg.count == 0) {
        printf("No active listeners. Use 'serve --listen ...' to start.\n");
        return;
    }

    srv_ipc_payload_t *scfg = &g_srv_active_cfg;
    uint32_t n_workers = g_core_map.num_workers;

    printf("  #  %-20s %-8s %7s %12s %12s %10s\n",
           "SPEC", "STATE", "CONNS", "RX bytes", "TX bytes", "HTTP resps");
    printf("  -  %-20s %-8s %7s %12s %12s %10s\n",
           "----", "-----", "-----", "--------", "--------", "----------");

    for (uint32_t i = 0; i < scfg->count; i++) {
        srv_listen_spec_t *sp = &scfg->specs[i];

        /* Aggregate stats across all workers */
        uint64_t conns = 0, rx = 0, tx = 0, http = 0;
        for (uint32_t w = 0; w < n_workers; w++) {
            if (i < g_srv_tables[w].count) {
                srv_listener_t *lw = &g_srv_tables[w].listeners[i];
                conns += lw->conns_accepted;
                rx    += lw->rx_bytes;
                tx    += lw->tx_bytes;
                http  += lw->http_resps_sent;
            }
        }
        /* Read per-listener active state from worker 0 */
        bool active = (i < g_srv_tables[0].count) &&
                       g_srv_tables[0].listeners[i].active;

        /* Build spec string */
        char spec[32];
        const char *proto = "tcp";
        if (sp->handler == SRV_HANDLER_HTTP)     proto = "http";
        if (sp->handler == SRV_HANDLER_HTTPS)    proto = "https";
        if (sp->handler == SRV_HANDLER_TLS_ECHO) proto = "tls";
        const char *hname = srv_handler_name(sp->handler);
        if (sp->handler == SRV_HANDLER_HTTP || sp->handler == SRV_HANDLER_HTTPS)
            snprintf(spec, sizeof(spec), "%s:%u", proto, sp->port);
        else
            snprintf(spec, sizeof(spec), "%s:%u:%s", proto, sp->port, hname);

        /* Format byte counts */
        char rx_s[16], tx_s[16];
        if (rx >= (1ULL << 30))
            snprintf(rx_s, sizeof(rx_s), "%.1f GB", (double)rx / (1ULL << 30));
        else if (rx >= (1ULL << 20))
            snprintf(rx_s, sizeof(rx_s), "%.1f MB", (double)rx / (1ULL << 20));
        else if (rx >= (1ULL << 10))
            snprintf(rx_s, sizeof(rx_s), "%.1f KB", (double)rx / (1ULL << 10));
        else
            snprintf(rx_s, sizeof(rx_s), "%lu B", (unsigned long)rx);

        if (tx >= (1ULL << 30))
            snprintf(tx_s, sizeof(tx_s), "%.1f GB", (double)tx / (1ULL << 30));
        else if (tx >= (1ULL << 20))
            snprintf(tx_s, sizeof(tx_s), "%.1f MB", (double)tx / (1ULL << 20));
        else if (tx >= (1ULL << 10))
            snprintf(tx_s, sizeof(tx_s), "%.1f KB", (double)tx / (1ULL << 10));
        else
            snprintf(tx_s, sizeof(tx_s), "%lu B", (unsigned long)tx);

        printf("  %u  %-20s %-8s %7lu %12s %12s %10lu\n",
               i, spec, active ? "active" : "stopped",
               (unsigned long)conns, rx_s, tx_s, (unsigned long)http);
    }
}

/* ── Server mode: connections command ────────────────────────────────────── */
static void
cmd_connections(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!g_config.server_mode) {
        printf("connections: not available in client mode\n");
        return;
    }

    uint32_t n_workers = g_core_map.num_workers;
    uint32_t total_active = 0;

    printf("  Worker  Active TCBs  Capacity\n");
    printf("  ------  -----------  --------\n");
    for (uint32_t w = 0; w < n_workers; w++) {
        tcb_store_t *store = &g_tcb_stores[w];
        uint32_t active = store->count;
        total_active += active;
        printf("  %6u  %11u  %8u\n", w, active, store->capacity);
    }
    printf("  ------  -----------  --------\n");
    printf("  Total   %11u\n", total_active);
}

/* ── Mode-gated wrappers ─────────────────────────────────────────────────── */
static void
cmd_start_gated(int argc, char **argv)
{
    if (g_config.server_mode) {
        printf("start: not available in server mode (use 'serve' instead)\n");
        return;
    }
    cmd_start(argc, argv);
}

static void
cmd_ping_gated(int argc, char **argv)
{
    if (g_config.server_mode) {
        printf("ping: not available in server mode\n");
        return;
    }
    cmd_ping(argc, argv);
}

static void
cmd_stop_gated(int argc, char **argv)
{
    if (g_config.server_mode) {
        /* Server mode: stop listeners */
        if (argc >= 2) {
            /* stop by spec or index */
            /* Try as numeric index first */
            char *endp;
            long idx = strtol(argv[1], &endp, 10);
            if (*endp == '\0' && idx >= 0) {
                /* Stop by index */
                config_update_t cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.cmd = CFG_CMD_STOP_LISTENER;
                uint32_t li = (uint32_t)idx;
                memcpy(cmd.payload, &li, sizeof(li));
                tgen_ipc_broadcast(&cmd);
                printf("Stopped listener #%ld\n", idx);
            } else {
                /* Stop by spec — find matching port */
                srv_listen_spec_t spec;
                if (parse_listen_spec(argv[1], &spec) == 0) {
                    int li = srv_find_listener(0, spec.port);
                    if (li >= 0) {
                        config_update_t cmd;
                        memset(&cmd, 0, sizeof(cmd));
                        cmd.cmd = CFG_CMD_STOP_LISTENER;
                        uint32_t liu = (uint32_t)li;
                        memcpy(cmd.payload, &liu, sizeof(liu));
                        tgen_ipc_broadcast(&cmd);
                        printf("Stopped listener %s\n", argv[1]);
                    } else {
                        printf("stop: no listener matching '%s'\n", argv[1]);
                    }
                } else {
                    printf("stop: invalid spec '%s'\n", argv[1]);
                }
            }
        } else {
            /* Stop all listeners */
            config_update_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.cmd = CFG_CMD_STOP_LISTENER;
            uint32_t all = UINT32_MAX;
            memcpy(cmd.payload, &all, sizeof(all));
            tgen_ipc_broadcast(&cmd);
            g_srv_active = false;
            printf("Stopped all listeners.\n");
        }
        return;
    }
    cmd_stop_gen(argc, argv);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
void
cli_register(const char *name, const char *help, const char *usage,
             cli_cmd_fn_t fn)
{
    if (g_n_cmds >= MAX_CMDS) return;
    strncpy(g_cmds[g_n_cmds].name, name, sizeof(g_cmds[0].name)-1);
    strncpy(g_cmds[g_n_cmds].help, help, sizeof(g_cmds[0].help)-1);
    g_cmds[g_n_cmds].usage = usage;
    g_cmds[g_n_cmds].fn = fn;
    g_n_cmds++;
}

void
cli_print_stats(void)
{
    metrics_snapshot_t snap;
    metrics_snapshot(&snap, g_core_map.num_workers);

    char buf[16384];
    export_net_text(&snap, buf, sizeof(buf));
    puts(buf);
}

static void
dispatch(char *line)
{
    /* Tokenise */
    char *argv[MAX_ARGS];
    int   argc = 0;
    char *tok  = strtok(line, " \t\r\n");
    while (tok && (uint32_t)argc < MAX_ARGS) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    if (argc == 0) return;

    for (uint32_t i = 0; i < g_n_cmds; i++) {
        if (strcmp(argv[0], g_cmds[i].name) == 0) {
            g_cmds[i].fn(argc, argv);
            fflush(stdout);
            return;
        }
    }
    printf("Unknown command: %s (type 'help' for list)\n", argv[0]);
    fflush(stdout);
}

void
cli_run(void)
{
    /* Register built-ins */
    cli_register("help",     "Show this help",
        "Usage: help [command]\n"
        "\n"
        "Without arguments, lists all available commands.\n"
        "With a command name, shows detailed usage for that command.\n",
        cmd_help);

    cli_register("stat",     "Statistics: stat [cpu|mem|net|port] [--rate] [--core N]",
        "Usage: stat [cpu|mem|net|port] [--rate] [--core N]\n"
        "\n"
        "Sub-commands:\n"
        "  cpu    Per-core CPU utilisation (RX%, TX%, Timer%, Idle%)\n"
        "  mem    Memory usage: mbufs, heap, connections, hugepages\n"
        "  net    Network packet counters (same as 'stats')\n"
        "  port   Per-NIC hardware statistics from the DPDK driver\n"
        "\n"
        "Flags:\n"
        "  --rate       1-second delta sample (pps, Mbps, %)\n"
        "  --core N     Filter output to worker N\n"
        "\n"
        "Without a sub-command, prints a brief summary of all domains.\n",
        cmd_stat);

    cli_register("stats",    "Alias for 'stat net'", NULL, cmd_stats);

    cli_register("ping",     "ICMP ping: ping <ip> [count] [size] [ms] [port]",
        "Usage: ping <dst_ip> [count] [size] [interval_ms] [port]\n"
        "\n"
        "Arguments:\n"
        "  dst_ip        Destination IPv4 address (required)\n"
        "  count         Number of echo requests (default: 5)\n"
        "  size          Payload size in bytes (default: 56)\n"
        "  interval_ms   Interval between requests in ms (default: 1000)\n"
        "  port          DPDK port ID to send from (default: 0)\n"
        "\n"
        "Examples:\n"
        "  ping 10.0.0.2\n"
        "  ping 10.0.0.2 10 128 500\n"
        "  ping 10.10.10.10 3 56 1000 1\n",
        cmd_ping_gated);

    cli_register("start",    "Start traffic: start --ip <ip> --port <N> --duration <s> [flags]",
        "Usage: start --ip <addr> --port <N> --duration <secs>\n"
        "             [--proto tcp|http|https|udp|icmp|tls]\n"
        "             [--rate <pps>] [--cps <N>] [--ramp <secs>]\n"
        "             [--size <bytes>] [--reuse] [--streams <N>]\n"
        "             [--url <path>] [--host <name>] [--tls]\n"
        "             [--one]\n"
        "\n"
        "Required:\n"
        "  --ip <addr>       Destination IPv4 address\n"
        "  --port <N>        Destination TCP/UDP port\n"
        "  --duration <s>    Test duration in seconds (not needed with --one)\n"
        "\n"
        "Optional:\n"
        "  --proto <name>    Protocol: tcp, http, https, udp, icmp, tls (default: tcp)\n"
        "  --rate <pps>      Rate limit in packets/sec (0 = unlimited)\n"
        "  --cps <N>         Connections per second (alias for --rate in TCP/HTTP)\n"
        "  --ramp <secs>     Gradual ramp-up from 0 to target rate\n"
        "  --size <bytes>    Payload size in bytes (default: 56)\n"
        "  --streams <N>     Concurrent streams, max 16 (default: 1)\n"
        "  --reuse           Enable connection reuse (throughput mode)\n"
        "  --url <path>      HTTP request path (default: /)\n"
        "  --host <name>     HTTP Host header (default: --ip value)\n"
        "  --tls             Enable TLS encryption\n"
        "  --one             Single request/handshake then stop\n"
        "  --txn-per-conn <N>  HTTP transactions per connection (enables keep-alive)\n"
        "  --think-time <ms>   Delay between transactions on same connection\n"
        "\n"
        "Examples:\n"
        "  start --ip 10.0.0.2 --port 5000 --duration 10\n"
        "  start --ip 10.0.0.2 --port 80 --proto http --duration 5 --cps 1000\n"
        "  start --ip 10.0.0.2 --port 80 --proto http --cps 5000 --ramp 5 --duration 30\n"
        "  start --ip 10.0.0.2 --port 80 --proto http --txn-per-conn 10 --think-time 100\n"
        "  start --ip 10.0.0.2 --port 443 --proto https --one --url /\n",
        cmd_start_gated);

    cli_register("stop",     "Stop traffic/listeners: stop [spec|#]", NULL, cmd_stop_gated);

    cli_register("reset",   "Reset all TCP state (connections, ports)",
        "Usage: reset\n"
        "\n"
        "Sends RST for all active connections, clears TCB stores,\n"
        "frees port pools, and resets metrics.\n"
        "Traffic generation must be stopped first.\n",
        cmd_reset);

    cli_register("trace",    "Packet capture: trace start/stop",
        "Usage: trace start <file.pcapng> [port] [queue]\n"
        "       trace stop\n"
        "\n"
        "Arguments (start):\n"
        "  file.pcapng   Output file path (required)\n"
        "  port          DPDK port ID to capture on (default: 0)\n"
        "  queue         Queue index to capture on (default: 0)\n"
        "\n"
        "Examples:\n"
        "  trace start capture.pcapng\n"
        "  trace start /tmp/debug.pcapng 0 0\n"
        "  trace stop\n",
        cmd_trace);

    cli_register("show",     "Show details: show interface|listeners|connections",
        "Usage: show interface [port_id]\n"
        "       show listeners           (server mode)\n"
        "       show connections          (server mode)\n"
        "\n"
        "show interface:\n"
        "  Displays driver, MAC, IP, gateway, netmask, link status,\n"
        "  offload capabilities, and packet statistics.\n"
        "  Without port_id, shows all ports.\n"
        "\n"
        "show listeners:\n"
        "  Shows active listeners with stats (SPEC is copy-pasteable for 'stop').\n"
        "\n"
        "show connections:\n"
        "  Shows per-worker active TCB count.\n"
        "\n"
        "Examples:\n"
        "  show interface\n"
        "  show interface 0\n"
        "  show listeners\n"
        "  show connections\n",
        cmd_show);

    cli_register("set",      "Set config: set ip ... | set rate <pps>",
        "Usage: set ip <port> <ip> <gateway> <netmask>\n"
        "       set rate <pps>\n"
        "\n"
        "Sub-commands:\n"
        "  ip      Set IP address, gateway, and netmask for a port\n"
        "  rate    Change rate limit for running test (broadcast to workers)\n"
        "\n"
        "Examples:\n"
        "  set ip 0 10.88.33.65 10.88.32.1 255.255.252.0\n"
        "  set rate 5000\n",
        cmd_set);

    /* ── Server-mode commands ──────────────────────────────────────────── */
    cli_register("serve",    "Start listeners: serve --listen <spec> [--listen ...] [opts]",
        "Usage: serve --listen <spec> [--listen <spec> ...]\n"
        "             [--tls-cert <path>] [--tls-key <path>]\n"
        "             [--http-body-size <bytes>]\n"
        "\n"
        "  <spec> = proto:port[:handler]\n"
        "\n"
        "  Proto     Handler     Description\n"
        "  -----     -------     -----------\n"
        "  tcp       echo        Reflect received data\n"
        "  tcp       discard     ACK + drop payload\n"
        "  tcp       chargen     Send bulk data\n"
        "  http      (implicit)  HTTP/1.1 response\n"
        "  https     (implicit)  TLS + HTTP response\n"
        "  tls       echo        TLS + echo\n"
        "\n"
        "Examples:\n"
        "  serve --listen tcp:5000:echo --listen http:80\n"
        "  serve --listen https:443 --tls-cert /path/cert.pem --tls-key /path/key.pem\n",
        cmd_serve);

    /* Start CLI socket server for remote attach */
    cli_server_init(NULL);

    /* Delegate to the cooperative event loop */
    mgmt_loop_run(dispatch);

    cli_server_destroy();
}
