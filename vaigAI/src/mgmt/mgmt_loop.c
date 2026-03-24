/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Management lcore cooperative event loop.
 *
 * Runs like a worker lcore — a tight run-to-completion poll loop that
 * never blocks (when active) and processes its tasks cooperatively.
 */
#include "mgmt_loop.h"
#include "cli_server.h"
#include "../net/arp.h"
#include "../net/icmp.h"
#include "../net/icmpv6.h"
#include "../net/ndp.h"
#include "../net/tcp_fsm.h"
#include "../net/tcp_tcb.h"
#include "../net/tcp_port_pool.h"
#include "../core/ipc.h"
#include "../core/core_assign.h"
#include "../core/worker_loop.h"
#include "../telemetry/pktrace.h"
#include "../telemetry/metrics.h"
#include "../telemetry/export.h"
#include "../telemetry/log.h"
#include "../telemetry/output.h"
#include "config_mgr.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <rte_cycles.h>
#include <rte_pause.h>
#include <rte_ethdev.h>

/* Delay for the given number of milliseconds while continuously draining the
 * pktrace capture ring.  Any mgmt-lcore code that needs to "sleep" must use
 * this instead of rte_delay_ms() to prevent capture drops at high rates. */
void
mgmt_delay_ms_flush(uint32_t ms)
{
    uint64_t end = rte_rdtsc() + (uint64_t)ms * rte_get_tsc_hz() / 1000ULL;
    while (rte_rdtsc() < end) {
        pktrace_flush();
        rte_pause();
    }
}

/* Local alias so mgmt_loop.c can use the short name internally */
static inline void delay_ms_flush(uint32_t ms) { mgmt_delay_ms_flush(ms); }

#ifdef HAVE_READLINE
# include <readline/readline.h>
# include <readline/history.h>
#endif

/* ── Globals ──────────────────────────────────────────────────────── */
mgmt_cpu_stats_t    g_mgmt_cpu_stats;
traffic_gen_state_t g_client_flows[TGEN_MAX_CLIENT_FLOWS];
uint32_t            g_client_flow_count;

/* Dispatch function stored for callback use */
static mgmt_dispatch_fn_t g_dispatch;

/* ── IPC ACK drain ────────────────────────────────────────────────── */
static void
ipc_ack_drain(void)
{
    ipc_ack_t ack;
    for (uint32_t w = 0; w < g_core_map.num_workers; w++) {
        while (tgen_ipc_collect_ack(w, &ack)) {
            if (ack.rc != 0)
                TGEN_WARN(TGEN_LOG_MGMT,
                    "IPC ACK from worker %u seq=%u rc=%d\n",
                    ack.worker_idx, ack.seq, ack.rc);
        }
    }
}

/* ── Async traffic gen tick (per-flow) ────────────────────────────── */
static void
traffic_gen_tick_flow(traffic_gen_state_t *ts)
{
    if (!ts->active)
        return;

    uint64_t now = rte_rdtsc();
    uint64_t hz  = rte_get_tsc_hz();
    uint64_t elapsed_s = (now - ts->start_tsc) / hz;

    /* --one: check completion condition */
    if (ts->one_shot) {
        metrics_snapshot_t ms;
        metrics_snapshot(&ms, ts->n_workers);
        bool done = false;
        if (strcmp(ts->proto, "icmp") == 0 ||
            strcmp(ts->proto, "udp") == 0)
            done = (ms.total.tx_pkts >= 1);
        else if (strcmp(ts->proto, "http") == 0 ||
                 strcmp(ts->proto, "https") == 0)
            done = (ms.total.http_rsp_rx >= 1 &&
                    ms.total.tcp_conn_close >= 1) ||
                   ms.total.tcp_reset_sent >= 1 ||
                   ms.total.tcp_reset_rx >= 1;
        else
            done = ms.total.tcp_conn_close >= 1 ||
                   ms.total.tcp_reset_sent >= 1 ||
                   ms.total.tcp_reset_rx >= 1;
        if (done) {
            mgmt_traffic_stop_flow(ts->flow_idx);
            return;
        }
    }

    /* Check duration */
    if (elapsed_s >= ts->duration_s) {
        mgmt_traffic_stop_flow(ts->flow_idx);
        return;
    }

    /* Progress (throttle to 1/s) */
    uint64_t since_last = (now - ts->last_progress) / hz;
    if (since_last >= 1) {
        /* Structured output: always emit if enabled */
        if (output_enabled()) {
            metrics_snapshot_t snap;
            metrics_snapshot(&snap, ts->n_workers);
            output_progress(ts->flow_idx, elapsed_s + 1, &snap);
        }

        /* TTY progress print (only if no partial input buffered) */
        if (ts->is_tty && !ts->reuse && !ts->one_shot) {
            bool can_print = true;
#ifdef HAVE_READLINE
            if (rl_line_buffer && rl_line_buffer[0] != '\0')
                can_print = false;
#endif
            if (can_print) {
                metrics_snapshot_t snap;
                metrics_snapshot(&snap, ts->n_workers);
                printf("\r  [#%u %"PRIu64"/%us] %"PRIu64" pkts",
                       ts->flow_idx,
                       elapsed_s + 1, ts->duration_s,
                       snap.total.tx_pkts);
                fflush(stdout);
            }
        }
        ts->last_progress = now;
    }
}

static void
traffic_gen_tick(void)
{
    for (uint32_t i = 0; i < TGEN_MAX_CLIENT_FLOWS; i++)
        traffic_gen_tick_flow(&g_client_flows[i]);
}

void
mgmt_traffic_start(const traffic_gen_state_t *state)
{
    uint32_t si = state->flow_idx;
    if (si >= TGEN_MAX_CLIENT_FLOWS)
        return;

    g_client_flows[si] = *state;
    g_client_flows[si].active = true;
    g_client_flows[si].start_tsc = rte_rdtsc();
    g_client_flows[si].last_progress = rte_rdtsc();

    if (si >= g_client_flow_count)
        g_client_flow_count = si + 1;
}

void
mgmt_traffic_stop_flow(uint32_t flow_idx)
{
    if (flow_idx >= TGEN_MAX_CLIENT_FLOWS)
        return;

    traffic_gen_state_t *ts = &g_client_flows[flow_idx];
    if (!ts->active)
        return;

    ts->active = false;
    ts->stop_tsc = rte_rdtsc(); /* record actual stop time */

    /* Broadcast STOP_FLOW to workers for this specific flow */
    config_update_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = CFG_CMD_STOP_FLOW;
    uint32_t si = flow_idx;
    memcpy(cmd.payload, &si, sizeof(si));
    tgen_ipc_broadcast(&cmd);

    delay_ms_flush(ts->reuse ? 1000 : 100);
    if (ts->is_tty && !ts->reuse)
        printf("\n");

    /* Snapshot results */
    metrics_snapshot_t snap;
    metrics_snapshot(&snap, ts->n_workers);

    /* Print results */
    uint64_t hz_s = rte_get_tsc_hz();
    double actual_s = (ts->stop_tsc > ts->start_tsc)
        ? (double)(ts->stop_tsc - ts->start_tsc) / (double)hz_s
        : 0.0;
    if (actual_s > (double)ts->duration_s)
        actual_s = (double)ts->duration_s;

    printf("--- flow #%u stopped ---\n", flow_idx);

    if (ts->reuse) {
        uint64_t total_bytes = snap.total.tcp_payload_tx;
        double mbps = (actual_s > 0.0)
            ? (double)total_bytes * 8.0 / (actual_s * 1e6) : 0.0;
        double mb   = (double)total_bytes / (1024.0 * 1024.0);
        printf("[SUM]  0.00-%.2fs    %.0f MB       %.1f Mbps\n",
               actual_s, mb, mbps);
    } else {
        char dur_str[32];
        if (actual_s < 1.0)
            snprintf(dur_str, sizeof(dur_str), "%.0f ms", actual_s * 1000.0);
        else
            snprintf(dur_str, sizeof(dur_str), "%.1f s", actual_s);
        printf("Protocol: %s, Duration: %s, Rate: %s\n"
               "%"PRIu64" packets transmitted\n",
               ts->proto, dur_str,
               ts->rate ? "limited" : "unlimited",
               snap.total.tx_pkts);
        if (actual_s > 0.0 && snap.total.tx_pkts > 0) {
            double pps = (double)snap.total.tx_pkts / actual_s;
            printf("Throughput: %.1f pps\n", pps);
        }
    }

    /* Export summary */
    char summary[8192];
    export_summary(&snap, actual_s, ts->proto,
                   summary, sizeof(summary));
    puts(summary);

    /* Structured output */
    output_result(flow_idx, ts->proto, actual_s, &snap);

    /* NIC stats — only print if this is the last active flow */
    if (!client_any_active()) {
        struct rte_eth_stats nic;
        if (rte_eth_stats_get(ts->port_id, &nic) == 0) {
            printf("\n--- NIC stats (port %u) ---\n", ts->port_id);
            printf("  opackets: %"PRIu64"  ipackets: %"PRIu64"\n",
                   nic.opackets, nic.ipackets);
            printf("  obytes:   %"PRIu64"  ibytes:   %"PRIu64"\n",
                   nic.obytes, nic.ibytes);
            printf("  imissed:  %"PRIu64"  ierrors:  %"PRIu64
                   "  oerrors:  %"PRIu64"\n",
                   nic.imissed, nic.ierrors, nic.oerrors);
            for (uint32_t q = 0; q < ts->n_workers; q++)
                printf("  q%u: rx=%"PRIu64" tx=%"PRIu64"\n",
                       q, nic.q_ipackets[q], nic.q_opackets[q]);
        }

        /* Per-worker TCP counters */
        if (ts->n_workers > 1) {
            printf("\n--- per-worker TCP ---\n");
            for (uint32_t w = 0; w < ts->n_workers; w++) {
                const worker_metrics_t *wm = &snap.per_worker[w];
                printf("  w%u: syn=%"PRIu64" open=%"PRIu64
                       " retx=%"PRIu64" tx=%"PRIu64" rx=%"PRIu64"\n",
                       w, wm->tcp_syn_sent, wm->tcp_conn_open,
                       wm->tcp_retransmit, wm->tx_pkts, wm->rx_pkts);
            }
        }
    }

    /* --one: reset TCBs, stores, metrics if no other flows active */
    if (ts->one_shot && !client_any_active()) {
        config_update_t rcmd;
        memset(&rcmd, 0, sizeof(rcmd));
        rcmd.cmd = CFG_CMD_RESET;
        rcmd.seq = 99;
        tgen_ipc_broadcast(&rcmd);
        delay_ms_flush(100);
        metrics_reset(ts->n_workers);
    }

    fflush(stdout);
}

void
mgmt_traffic_stop_all(void)
{
    /* Broadcast a blanket STOP_FLOW(all) first */
    config_update_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = CFG_CMD_STOP_FLOW;
    uint32_t all = UINT32_MAX;
    memcpy(cmd.payload, &all, sizeof(all));
    tgen_ipc_broadcast(&cmd);

    /* Now print summary for each flow that was active */
    bool any_was_active = false;
    uint64_t stop_tsc = rte_rdtsc();
    uint32_t last_n_workers = g_core_map.num_workers;

    for (uint32_t i = 0; i < TGEN_MAX_CLIENT_FLOWS; i++) {
        traffic_gen_state_t *ts = &g_client_flows[i];
        if (!ts->active)
            continue;
        any_was_active = true;
        ts->active = false;
        ts->stop_tsc = stop_tsc;
        last_n_workers = ts->n_workers;

        uint64_t hz_s = rte_get_tsc_hz();
        double actual_s = (ts->stop_tsc > ts->start_tsc)
            ? (double)(ts->stop_tsc - ts->start_tsc) / (double)hz_s
            : 0.0;
        if (actual_s > (double)ts->duration_s)
            actual_s = (double)ts->duration_s;

        char dur_str[32];
        if (actual_s < 1.0)
            snprintf(dur_str, sizeof(dur_str), "%.0f ms", actual_s * 1000.0);
        else
            snprintf(dur_str, sizeof(dur_str), "%.1f s", actual_s);

        printf("  #%u  %s → %s:%u  %s\n",
               i, ts->proto, ts->dst_ip_str, ts->dst_port, dur_str);
    }

    if (!any_was_active) {
        printf("No active client flows.\n");
        return;
    }

    delay_ms_flush(100);

    /* Aggregate stats */
    metrics_snapshot_t snap;
    metrics_snapshot(&snap, last_n_workers);
    printf("\n--- traffic statistics (all flows) ---\n"
           "%"PRIu64" packets transmitted\n",
           snap.total.tx_pkts);

    char summary[8192];
    export_summary(&snap, 0.0, "mixed",
                   summary, sizeof(summary));
    puts(summary);

    fflush(stdout);
}

/* ── Non-blocking stdin ───────────────────────────────────────────── */
#ifdef HAVE_READLINE
static bool g_readline_got_line;
static char *g_readline_line;

static void
readline_line_cb(char *line)
{
    g_readline_got_line = true;
    g_readline_line = line;
}
#endif

static bool g_stdin_initialized;
static bool g_stdin_eof;

static void
cli_stdin_init(mgmt_dispatch_fn_t dispatch)
{
    g_dispatch = dispatch;
    g_stdin_eof = false;

#ifdef HAVE_READLINE
    rl_callback_handler_install(
        g_config.server_mode ? "vaigai(server)> " : "vaigai> ",
        readline_line_cb);
    rl_bind_key('\t', rl_complete);
    g_readline_got_line = false;
    g_readline_line = NULL;
#endif

    g_stdin_initialized = true;
}

static void
cli_stdin_cleanup(void)
{
#ifdef HAVE_READLINE
    rl_callback_handler_remove();
#endif
    g_stdin_initialized = false;
}

static void
cli_stdin_poll(void)
{
    if (g_stdin_eof || !g_stdin_initialized)
        return;

    /* Check if stdin has data (zero-timeout = non-blocking) */
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0)
        return;

#ifdef HAVE_READLINE
    rl_callback_read_char();
    if (!g_readline_got_line)
        return;

    char *line = g_readline_line;
    g_readline_got_line = false;
    g_readline_line = NULL;

    if (!line) {
        /* EOF */
        g_stdin_eof = true;
        return;
    }
    if (*line)
        add_history(line);
    if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
        free(line);
        extern volatile int g_run;
        g_run = 0;
        return;
    }
    g_dispatch(line);
    free(line);
#else
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) {
        g_stdin_eof = true;
        return;
    }
    buf[strcspn(buf, "\n")] = '\0';
    if (strcmp(buf, "quit") == 0 || strcmp(buf, "exit") == 0) {
        extern volatile int g_run;
        g_run = 0;
        return;
    }
    g_dispatch(buf);
#endif
}

/* ── The event loop ───────────────────────────────────────────────── */
void
mgmt_loop_run(mgmt_dispatch_fn_t dispatch)
{
    extern volatile int g_run;
    mgmt_cpu_stats_t *cs = &g_mgmt_cpu_stats;
    memset(cs, 0, sizeof(*cs));

    /* Detect headless mode: stdin is /dev/null */
    struct stat st_in;
    bool headless = (fstat(STDIN_FILENO, &st_in) == 0 &&
                     S_ISCHR(st_in.st_mode) &&
                     !isatty(STDIN_FILENO));

    if (headless) {
        TGEN_INFO(TGEN_LOG_MGMT,
                  "stdin is /dev/null — running in headless mode "
                  "(send SIGTERM to stop, or use vaigai --attach)\n");
    } else {
        if (isatty(STDIN_FILENO))
            printf("vaigai CLI%s  (type 'help' for commands, "
                   "'help <cmd>' for details, 'quit' to exit)\n",
                   g_config.server_mode ? " [server mode]" : "");
        cli_stdin_init(dispatch);
    }

    while (g_run) {
        uint64_t t0 = rte_rdtsc();

        /* ── Priority 1: User input (never starved) ───────────── */
        if (!headless)
            cli_stdin_poll();
        cli_server_poll(dispatch);

        uint64_t t1 = rte_rdtsc();

        /* ── Priority 2: Protocol housekeeping ────────────────── */
        arp_mgmt_tick();
        icmp_mgmt_tick();
        ndp_mgmt_tick();
        icmpv6_mgmt_tick();
        ipc_ack_drain();

        uint64_t t2 = rte_rdtsc();

        /* ── Priority 3: Background work ──────────────────────── */
        pktrace_flush();
        traffic_gen_tick();

        uint64_t t3 = rte_rdtsc();

        /* ── CPU cycle accounting ─────────────────────────────── */
        cs->cycles_cli   += (t1 - t0);
        cs->cycles_proto += (t2 - t1);
        cs->cycles_bg    += (t3 - t2);
        cs->cycles_total += (t3 - t0);
        cs->loop_count++;

        /* ── Adaptive yield ───────────────────────────────────── */
        bool active = pktrace_is_active() || client_any_active();
        if (active) {
            cs->cycles_idle += 0; /* busy — no idle accounting */
            rte_pause();
        } else {
            /* Truly idle — yield CPU.  poll(stdin, 1ms) wakes
             * instantly on keystroke, so CLI latency is 0. */
            struct pollfd pfd = { .fd = STDIN_FILENO,
                                  .events = POLLIN };
            uint64_t idle_start = rte_rdtsc();
            poll(&pfd, 1, 1); /* 1ms timeout */
            cs->cycles_idle += (rte_rdtsc() - idle_start);
        }
    }

    if (!headless)
        cli_stdin_cleanup();
}
