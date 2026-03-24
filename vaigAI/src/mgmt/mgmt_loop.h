/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Management lcore cooperative event loop.
 *
 * The management core runs a tight poll loop — structurally identical
 * to a DPDK worker lcore — but drains CLI input, protocol rings, and
 * background tasks instead of NIC RX/TX queues.
 *
 * No function in the loop body may block.  When all subsystems are idle,
 * the loop yields via poll(stdin, 1ms) which wakes instantly on keystroke.
 */
#ifndef TGEN_MGMT_LOOP_H
#define TGEN_MGMT_LOOP_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Per-flow traffic generation state (async, non-blocking) ────────── */
typedef struct {
    bool        active;         /* traffic gen in progress */
    bool        one_shot;       /* --one mode */
    bool        reuse;          /* --reuse (throughput) mode */
    bool        is_tty;         /* stdout is a TTY */
    uint64_t    start_tsc;      /* TSC at start */
    uint64_t    stop_tsc;       /* TSC at stop (for actual elapsed) */
    uint32_t    duration_s;     /* requested duration */
    uint32_t    n_workers;      /* worker count at start time */
    uint16_t    port_id;        /* DPDK port used */
    uint64_t    last_progress;  /* TSC of last progress print */
    char        proto[16];      /* protocol name for summary */
    uint32_t    streams;        /* TCP connection count (throughput mode) */
    uint64_t    rate;           /* rate limit (pps) */
    uint16_t    size;           /* packet size */
    bool        tls;            /* TLS enabled */
    uint32_t    flow_idx;       /* slot index in g_client_flows[] */
    char        dst_ip_str[16]; /* destination IP for display */
    uint16_t    dst_port;       /* destination port for display */
} traffic_gen_state_t;

/* ── Client flow table (mirrors srv_table_t pattern) ───────────────── */
extern traffic_gen_state_t g_client_flows[TGEN_MAX_CLIENT_FLOWS];
extern uint32_t            g_client_flow_count; /* high-water mark */

/** Check if any client flow is active. */
static inline bool client_any_active(void) {
    for (uint32_t i = 0; i < TGEN_MAX_CLIENT_FLOWS; i++)
        if (g_client_flows[i].active) return true;
    return false;
}

/* Backward-compat alias — points to flow 0 for legacy callers */
#define g_traffic_state g_client_flows[0]

/* ── Management core CPU stats ────────────────────────────────────── */
typedef struct {
    uint64_t cycles_cli;        /* CLI stdin + server polling */
    uint64_t cycles_proto;      /* ARP + ICMP + IPC ACK drain */
    uint64_t cycles_bg;         /* pktrace flush + traffic gen tick */
    uint64_t cycles_idle;       /* iterations with no work done */
    uint64_t cycles_total;      /* total cycles in loop */
    uint64_t loop_count;        /* number of loop iterations */
} mgmt_cpu_stats_t;

extern mgmt_cpu_stats_t g_mgmt_cpu_stats;

/**
 * Run the management event loop.  Called from cli_run() after command
 * registration and server init.  Returns when g_run becomes 0.
 *
 * @param dispatch  CLI command dispatch function.
 */
typedef void (*mgmt_dispatch_fn_t)(char *line);
void mgmt_loop_run(mgmt_dispatch_fn_t dispatch);

/**
 * Start async traffic generation for a specific flow slot.
 * Called by cmd_start after ARP resolve and IPC broadcast.
 */
void mgmt_traffic_start(const traffic_gen_state_t *state);

/**
 * Stop a specific client flow by index.  Broadcasts CFG_CMD_STOP_FLOW
 * to workers and prints summary for that flow.
 */
void mgmt_traffic_stop_flow(uint32_t flow_idx);

/**
 * Stop all active client flows.
 */
void mgmt_traffic_stop_all(void);

/**
 * Delay for @p ms milliseconds while continuously draining the pktrace
 * capture ring.  Any mgmt-lcore code that needs to "sleep" must use this
 * instead of rte_delay_ms() to prevent capture drops at high rates.
 */
void mgmt_delay_ms_flush(uint32_t ms);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_MGMT_LOOP_H */
