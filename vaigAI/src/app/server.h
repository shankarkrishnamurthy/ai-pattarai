/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Server mode — listener table and application handlers (§5.2).
 *
 * In server mode, vaigai accepts incoming TCP connections and dispatches
 * them to protocol-specific handlers (echo, discard, chargen, HTTP, HTTPS,
 * TLS echo).  The listener table is per-worker (no locks in hot path).
 *
 * Lifecycle:
 *   CLI "serve --listen ..."  →  mgmt parses & broadcasts CFG_CMD_SERVE
 *   →  each worker populates its local listener table
 *   →  tcp_fsm passive open checks listener table on SYN
 *   →  ESTABLISHED: dispatch received data to handler
 */
#ifndef TGEN_SERVER_H
#define TGEN_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Handler types ────────────────────────────────────────────────────────── */
typedef enum {
    SRV_HANDLER_NONE     = 0,
    SRV_HANDLER_ECHO,        /* reflect received data back to sender       */
    SRV_HANDLER_DISCARD,     /* ACK + drop payload                         */
    SRV_HANDLER_CHARGEN,     /* TX bulk data after connect                 */
    SRV_HANDLER_HTTP,        /* parse HTTP/1.1 request, send pre-built rsp */
    SRV_HANDLER_HTTPS,       /* TLS accept + HTTP response                 */
    SRV_HANDLER_TLS_ECHO,    /* TLS accept + echo plaintext                */
    SRV_HANDLER_MAX,
} srv_handler_t;

/* ── Single listener entry ────────────────────────────────────────────────── */
#define SRV_MAX_LISTENERS 16

typedef struct {
    uint16_t       port;        /* TCP port (host byte order)              */
    srv_handler_t  handler;     /* what to do with connections              */
    bool           active;      /* currently accepting connections          */

    /* Per-listener stats (single-writer per worker, aggregated by mgmt) */
    uint64_t       conns_accepted;
    uint64_t       conns_active;
    uint64_t       rx_bytes;
    uint64_t       tx_bytes;
    uint64_t       http_resps_sent;
} srv_listener_t;

/* ── Per-worker listener table ────────────────────────────────────────────── */
typedef struct {
    srv_listener_t listeners[SRV_MAX_LISTENERS];
    uint32_t       count;
    bool           serving;     /* true when at least one listener is active */

    /* Pre-built HTTP response (shared across all HTTP/HTTPS listeners) */
    uint8_t        http_response[18000];
    uint32_t       http_response_len;

    /* Chargen pattern buffer (1 MSS worth of data) */
    uint8_t        chargen_buf[1460];
} srv_table_t;

extern srv_table_t g_srv_tables[TGEN_MAX_WORKERS];

/* ── Configuration (sent from mgmt to workers via IPC) ────────────────────── */
typedef struct {
    uint16_t       port;
    srv_handler_t  handler;
} srv_listen_spec_t;

#define SRV_CFG_MAX_LISTENERS SRV_MAX_LISTENERS

typedef struct {
    srv_listen_spec_t specs[SRV_CFG_MAX_LISTENERS];
    uint32_t          count;
    uint32_t          http_body_size;   /* 0 = default 1024               */
    char              tls_cert[128];    /* path to PEM cert               */
    char              tls_key[128];     /* path to PEM key                */
} srv_cfg_t;

/* Ensure srv_cfg_t fits in IPC payload (248 bytes).
 * specs:  16 * 8  = 128
 * count:           4
 * http_body_size:  4
 * tls_cert:      128  — too large, need to shrink or use pointer.
 * Actually: 128 + 4 + 4 + 128 + 128 = 392.  Too large for 248 bytes.
 * Solution: store cert/key paths in a global, not in IPC payload. */

/* Reduced IPC payload — cert/key paths are set globally before broadcast */
typedef struct {
    srv_listen_spec_t specs[SRV_MAX_LISTENERS];
    uint32_t          count;
    uint32_t          http_body_size;
} srv_ipc_payload_t;

_Static_assert(sizeof(srv_ipc_payload_t) <= 248,
               "srv_ipc_payload_t must fit in IPC payload");

/* ── Global cert/key paths (set by CLI before IPC broadcast) ──────────────── */
extern char g_srv_tls_cert_path[256];
extern char g_srv_tls_key_path[256];

/* ── Mgmt-side shadow of active serve config (for CLI display) ────────────── */
extern srv_ipc_payload_t g_srv_active_cfg;
extern bool              g_srv_active;

/* ── API ──────────────────────────────────────────────────────────────────── */

/** Populate per-worker listener table from IPC payload. */
void srv_table_configure(uint32_t worker_idx, const srv_ipc_payload_t *cfg);

/** Stop all listeners for a worker. */
void srv_table_stop_all(uint32_t worker_idx);

/** Stop a specific listener by index. */
void srv_table_stop_one(uint32_t worker_idx, uint32_t listener_idx);

/** Check if a port has an active listener; returns handler or NONE. */
srv_handler_t srv_lookup_port(uint32_t worker_idx, uint16_t port);

/** Find listener index for a port; returns -1 if not found. */
int srv_find_listener(uint32_t worker_idx, uint16_t port);

/**
 * Server-side data handler: called from tcp_fsm when data arrives on
 * a server-mode connection.  Returns bytes consumed.
 */
int srv_on_data(uint32_t worker_idx, void *tcb_ptr,
                const uint8_t *data, uint32_t len,
                uint16_t listen_port);

/**
 * Server-side connection established: called when 3WHS completes
 * on a server-mode connection.  May start chargen TX or TLS accept.
 */
void srv_on_established(uint32_t worker_idx, void *tcb_ptr,
                        uint16_t listen_port);

/** Get handler name string for display. */
const char *srv_handler_name(srv_handler_t h);

/** Build pre-built HTTP response into the listener table. */
void srv_build_http_response(srv_table_t *tbl, uint32_t body_size);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_SERVER_H */
