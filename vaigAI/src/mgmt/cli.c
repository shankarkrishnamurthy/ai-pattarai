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
    (void)argc; (void)argv;
    printf("Available commands:\n");
    for (uint32_t i = 0; i < g_n_cmds; i++)
        printf("  %-24s  %s\n", g_cmds[i].name, g_cmds[i].help);
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
    bool watch;     /* --watch: continuous refresh (implies rate) */
    int  core;      /* --core N (-1 = all) */
} stat_opts_t;

static void
stat_parse_opts(int argc, char **argv, int start, stat_opts_t *opts)
{
    opts->rate  = false;
    opts->watch = false;
    opts->core  = -1;
    for (int i = start; i < argc; i++) {
        if (strcmp(argv[i], "--rate") == 0)
            opts->rate = true;
        else if (strcmp(argv[i], "--watch") == 0) {
            opts->watch = true;
            opts->rate  = true;
        } else if (strcmp(argv[i], "--core") == 0 && i + 1 < argc) {
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

    /* --watch wraps in a loop */
    if (opts.watch) {
        /* When dispatched from a remote CLI client (vaigai --attach),
         * stdout is a memstream with no file descriptor.  The remote
         * client has no way to press a key to exit the loop, so fall
         * back to a single --rate sample instead of blocking the
         * management core indefinitely. */
        if (fileno(stdout) < 0) {
            opts.watch = false;
            /* opts.rate is already true (--watch implies --rate);
             * fall through to the single-shot path below. */
        } else if (!isatty(STDOUT_FILENO)) {
            printf("--watch requires a TTY\n");
            return;
        }
    }

    if (opts.watch) {
        while (g_run) {
            printf("\033[H\033[2J"); /* clear screen */
            if (strcmp(sub, "cpu") == 0)       stat_cpu(&opts);
            else if (strcmp(sub, "mem") == 0)  stat_mem(&opts);
            else if (strcmp(sub, "net") == 0)  stat_net(&opts);
            else if (strcmp(sub, "port") == 0) stat_port(&opts);
            else { printf("Unknown stat sub-command: %s\n", sub); return; }
            fflush(stdout);

            /* Service ARP / ICMP / pcap-trace between iterations.
             * The stat_*() functions already slept ~1 s for rate
             * sampling; this loop adds ~100 ms of management ticks
             * so the control plane (ARP replies, ICMP echo, packet
             * capture flush) keeps working and does not starve
             * ongoing traffic generation. */
            struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
            bool quit = false;
            for (int t = 0; t < 10 && !quit; t++) {
                arp_mgmt_tick();
                icmp_mgmt_tick();
                pktrace_flush();
                if (poll(&pfd, 1, 10) > 0)
                    quit = true;
            }
            if (quit) break;
        }
        return;
    }

    if (strcmp(sub, "cpu") == 0)       stat_cpu(&opts);
    else if (strcmp(sub, "mem") == 0)  stat_mem(&opts);
    else if (strcmp(sub, "net") == 0)  stat_net(&opts);
    else if (strcmp(sub, "port") == 0) stat_port(&opts);
    else printf("Unknown stat sub-command: %s\n"
                "Usage: stat [cpu|mem|net|port] [--rate] [--watch] [--core N]\n",
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
           "             [--rate <pps>] [--size <bytes>]\n"
           "             [--reuse] [--streams <N>]\n"
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
    uint16_t port_id = 0;

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
            if (arp_lookup(port_id, nexthop, &dst_mac)) break;
            rte_delay_ms(10);
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

    /* Pipe mode: keep blocking behavior for scripts that expect start
     * to block until completion (e.g. `echo "start ..." | vaigai`). */
    bool pipe_mode = !isatty(STDIN_FILENO);
    if (pipe_mode) {
        mgmt_traffic_start(&tgs);
        while (g_traffic_state.active && g_run) {
            arp_mgmt_tick();
            icmp_mgmt_tick();
            pktrace_flush();
            cli_server_poll(dispatch);
            /* Check completion inline */
            uint64_t now = rte_rdtsc();
            uint64_t hz  = rte_get_tsc_hz();
            uint64_t elapsed_s = (now - g_traffic_state.start_tsc) / hz;
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
                    done = (ms.total.http_rsp_rx >= 1 &&
                            ms.total.tcp_conn_close >= 1);
                else
                    done = (ms.total.tcp_conn_close >= 1);
                if (done) { mgmt_traffic_stop(); break; }
            }
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
            nanosleep(&ts, NULL);
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
        config_update_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmd = CFG_CMD_STOP;
        cmd.seq = 2;
        tgen_ipc_broadcast(&cmd);
        printf("Traffic generation stopped.\n");
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
                    rte_delay_ms(5);
            }
        }
    }

    /* Wait for RSTs to be transmitted and remote side to process them */
    rte_delay_ms(200);

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
    if (argc < 2 || strcmp(argv[1], "ip") != 0) {
        printf("Usage: set ip <port> <ip> <gateway> <netmask>\n"
               "  Example: set ip 0 10.88.33.65 10.88.32.1 255.255.252.0\n");
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

/* ── Show interface details ──────────────────────────────────────────────── */
static void
cmd_show(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "interface") != 0) {
        printf("Usage: show interface [port_id]\n");
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

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
void
cli_register(const char *name, const char *help, cli_cmd_fn_t fn)
{
    if (g_n_cmds >= MAX_CMDS) return;
    strncpy(g_cmds[g_n_cmds].name, name, sizeof(g_cmds[0].name)-1);
    strncpy(g_cmds[g_n_cmds].help, help, sizeof(g_cmds[0].help)-1);
    g_cmds[g_n_cmds].fn = fn;
    g_n_cmds++;
}

void
cli_print_stats(void)
{
    metrics_snapshot_t snap;
    metrics_snapshot(&snap, g_core_map.num_workers);

    /* Human-readable display */
    char buf[16384];
    export_net_text(&snap, buf, sizeof(buf));
    puts(buf);

    /* Machine-parseable JSON for tests and scripts */
    char json[4096];
    export_json(&snap, json, sizeof(json));
    puts(json);
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
    cli_register("help",     "Show this help",           cmd_help);
    cli_register("stat",     "Statistics: stat [cpu|mem|net|port] [--rate] [--watch] [--core N]", cmd_stat);
    cli_register("stats",    "Alias for 'stat net'",     cmd_stats);
    cli_register("ping",     "ICMP ping: ping <ip> [count] [size] [ms] [port]", cmd_ping);
    cli_register("start",    "Start traffic: start --ip <ip> --port <N> --duration <s> [flags]", cmd_start);
    cli_register("stop",     "Stop active traffic generation",          cmd_stop_gen);
    cli_register("reset",   "Reset all TCP state (connections, ports)", cmd_reset);
    cli_register("trace",    "Packet capture: trace start/stop",          cmd_trace);
    cli_register("show",     "Show interface details: show interface [port]", cmd_show);
    cli_register("set",      "Set config: set ip <port> <ip> <gateway> <netmask>", cmd_set);

    /* Start CLI socket server for remote attach */
    cli_server_init(NULL);

    /* Delegate to the cooperative event loop */
    mgmt_loop_run(dispatch);

    cli_server_destroy();
}
