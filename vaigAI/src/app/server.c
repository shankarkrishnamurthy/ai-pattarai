/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Server mode — listener table and application handlers.
 */
#include "server.h"
#include "http11.h"
#include "../net/tcp_fsm.h"
#include "../net/tcp_tcb.h"
#include "../tls/tls_session.h"
#include "../tls/tls_engine.h"
#include "../telemetry/metrics.h"
#include "../common/util.h"

#include <string.h>
#include <rte_log.h>
#include <rte_cycles.h>

#define RTE_LOGTYPE_TGEN_SRV RTE_LOGTYPE_USER5

/* ── Global state ─────────────────────────────────────────────────────────── */
srv_table_t g_srv_tables[TGEN_MAX_WORKERS];
char g_srv_tls_cert_path[256];
char g_srv_tls_key_path[256];
srv_ipc_payload_t g_srv_active_cfg;
bool              g_srv_active;

/* ── Handler name table ───────────────────────────────────────────────────── */
static const char *handler_names[] = {
    [SRV_HANDLER_NONE]     = "none",
    [SRV_HANDLER_ECHO]     = "echo",
    [SRV_HANDLER_DISCARD]  = "discard",
    [SRV_HANDLER_CHARGEN]  = "chargen",
    [SRV_HANDLER_HTTP]     = "http",
    [SRV_HANDLER_HTTPS]    = "https",
    [SRV_HANDLER_TLS_ECHO] = "tls_echo",
};

const char *srv_handler_name(srv_handler_t h)
{
    if (h < SRV_HANDLER_MAX)
        return handler_names[h];
    return "unknown";
}

/* ── Build pre-built HTTP response ────────────────────────────────────────── */
void srv_build_http_response(srv_table_t *tbl, uint32_t body_size)
{
    if (body_size == 0) body_size = 1024;
    if (body_size > 3000) body_size = 3000; /* fit in 4096 with headers */

    /* Build a body of repeating 'A' characters */
    uint8_t body[3000];
    memset(body, 'A', body_size);

    int len = http11_tx_response(tbl->http_response, sizeof(tbl->http_response),
                                  200, "OK", "text/plain",
                                  body, body_size);
    tbl->http_response_len = (len > 0) ? (uint32_t)len : 0;
}

/* ── Configure listener table ─────────────────────────────────────────────── */
void srv_table_configure(uint32_t worker_idx, const srv_ipc_payload_t *cfg)
{
    srv_table_t *tbl = &g_srv_tables[worker_idx];

    /* Reset existing listeners */
    memset(tbl->listeners, 0, sizeof(tbl->listeners));
    tbl->count = 0;

    for (uint32_t i = 0; i < cfg->count && i < SRV_MAX_LISTENERS; i++) {
        srv_listener_t *l = &tbl->listeners[i];
        l->port    = cfg->specs[i].port;
        l->handler = cfg->specs[i].handler;
        l->active  = true;
        tbl->count++;
    }

    /* Build pre-built HTTP response */
    srv_build_http_response(tbl, cfg->http_body_size);

    /* Fill chargen buffer with printable ASCII pattern */
    for (uint32_t i = 0; i < sizeof(tbl->chargen_buf); i++)
        tbl->chargen_buf[i] = (uint8_t)(' ' + (i % 95));

    tbl->serving = (tbl->count > 0);

    RTE_LOG(INFO, TGEN_SRV, "Worker %u: configured %u listener(s)\n",
            worker_idx, tbl->count);
}

/* ── Stop all ─────────────────────────────────────────────────────────────── */
void srv_table_stop_all(uint32_t worker_idx)
{
    srv_table_t *tbl = &g_srv_tables[worker_idx];
    for (uint32_t i = 0; i < tbl->count; i++)
        tbl->listeners[i].active = false;
    tbl->serving = false;
}

/* ── Stop one ─────────────────────────────────────────────────────────────── */
void srv_table_stop_one(uint32_t worker_idx, uint32_t listener_idx)
{
    srv_table_t *tbl = &g_srv_tables[worker_idx];
    if (listener_idx < tbl->count)
        tbl->listeners[listener_idx].active = false;

    /* Check if any remain active */
    tbl->serving = false;
    for (uint32_t i = 0; i < tbl->count; i++) {
        if (tbl->listeners[i].active) {
            tbl->serving = true;
            break;
        }
    }
}

/* ── Lookup ───────────────────────────────────────────────────────────────── */
srv_handler_t srv_lookup_port(uint32_t worker_idx, uint16_t port)
{
    srv_table_t *tbl = &g_srv_tables[worker_idx];
    for (uint32_t i = 0; i < tbl->count; i++) {
        if (tbl->listeners[i].port == port && tbl->listeners[i].active)
            return tbl->listeners[i].handler;
    }
    return SRV_HANDLER_NONE;
}

int srv_find_listener(uint32_t worker_idx, uint16_t port)
{
    srv_table_t *tbl = &g_srv_tables[worker_idx];
    for (uint32_t i = 0; i < tbl->count; i++) {
        if (tbl->listeners[i].port == port)
            return (int)i;
    }
    return -1;
}

/* ── Connection established handler ───────────────────────────────────────── */
void srv_on_established(uint32_t worker_idx, void *tcb_ptr,
                        uint16_t listen_port)
{
    tcb_t *tcb = (tcb_t *)tcb_ptr;
    srv_table_t *tbl = &g_srv_tables[worker_idx];
    int li = srv_find_listener(worker_idx, listen_port);
    if (li < 0) return;

    srv_listener_t *l = &tbl->listeners[li];
    l->conns_accepted++;
    l->conns_active++;

    switch (l->handler) {
    case SRV_HANDLER_CHARGEN:
        /* Start pumping data immediately.
         * Use app_state = 10 as "chargen active" marker. */
        tcb->app_state = 10;
        tcp_fsm_send(worker_idx, tcb,
                     tbl->chargen_buf, sizeof(tbl->chargen_buf));
        l->tx_bytes += sizeof(tbl->chargen_buf);
        break;

    case SRV_HANDLER_HTTPS:
    case SRV_HANDLER_TLS_ECHO: {
        /* Initiate TLS accept (server-side handshake).
         * app_state = 20 means "TLS server handshaking" */
        uint32_t conn_idx = (uint32_t)(tcb - g_tcb_stores[worker_idx].tcbs);
        int trc = tls_session_attach(worker_idx, conn_idx, true, NULL);
        if (trc == 0) {
            tcb->app_state = 20; /* TLS server handshaking */
            tcb->tls_hs_start_tsc = rte_rdtsc();
        } else {
            RTE_LOG(WARNING, TGEN_SRV,
                    "Worker %u: TLS attach failed for port %u\n",
                    worker_idx, listen_port);
            tcp_fsm_reset(worker_idx, tcb);
        }
        break;
    }

    case SRV_HANDLER_ECHO:
    case SRV_HANDLER_DISCARD:
    case SRV_HANDLER_HTTP:
        /* These handlers activate on data arrival, nothing to do now */
        break;

    default:
        break;
    }
}

/* ── Data arrival handler ─────────────────────────────────────────────────── */
int srv_on_data(uint32_t worker_idx, void *tcb_ptr,
                const uint8_t *data, uint32_t len,
                uint16_t listen_port)
{
    tcb_t *tcb = (tcb_t *)tcb_ptr;
    srv_table_t *tbl = &g_srv_tables[worker_idx];
    int li = srv_find_listener(worker_idx, listen_port);
    if (li < 0) return 0;

    srv_listener_t *l = &tbl->listeners[li];
    l->rx_bytes += len;

    switch (l->handler) {
    case SRV_HANDLER_ECHO:
        /* Reflect data back to sender */
        tcp_fsm_send(worker_idx, tcb, data, len);
        l->tx_bytes += len;
        return (int)len;

    case SRV_HANDLER_DISCARD:
        /* Data consumed, nothing to send back (ACK handled by TCP FSM) */
        return (int)len;

    case SRV_HANDLER_CHARGEN:
        /* Chargen ignores received data, keep pumping.
         * Send another MSS of data on each ACK. */
        tcp_fsm_send(worker_idx, tcb,
                     tbl->chargen_buf, sizeof(tbl->chargen_buf));
        l->tx_bytes += sizeof(tbl->chargen_buf);
        return (int)len;

    case SRV_HANDLER_HTTP: {
        /* Look for end of HTTP request headers (\r\n\r\n) */
        const uint8_t *end = memmem(data, len, "\r\n\r\n", 4);
        if (end && tbl->http_response_len > 0) {
            /* Send pre-built HTTP response */
            tcp_fsm_send(worker_idx, tcb,
                         tbl->http_response, tbl->http_response_len);
            l->tx_bytes += tbl->http_response_len;
            l->http_resps_sent++;
            worker_metrics_add_http_req(worker_idx); /* reuse as server req_rx */
        }
        return (int)len;
    }

    case SRV_HANDLER_HTTPS:
    case SRV_HANDLER_TLS_ECHO: {
        uint32_t conn_idx = (uint32_t)(tcb - g_tcb_stores[worker_idx].tcbs);
        tls_session_t *ts = tls_session_get(worker_idx, conn_idx);
        if (!ts) return (int)len;

        if (tcb->app_state == 20) {
            /* TLS handshake in progress */
            uint8_t tls_out[4096];
            size_t  tls_out_len = sizeof(tls_out);
            int hr = tls_handshake(ts, data, len, tls_out, &tls_out_len);
            if (tls_out_len > 0)
                tcp_fsm_send(worker_idx, tcb,
                             tls_out, (uint32_t)tls_out_len);
            if (hr == 1) {
                /* TLS established */
                tcb->app_state = 21; /* TLS server established */
                worker_metrics_add_tls_ok(worker_idx);
                uint64_t lat_us = (rte_rdtsc() - tcb->tls_hs_start_tsc)
                                  * 1000000ULL / rte_get_tsc_hz();
                hist_record(&g_latency_hist[worker_idx], lat_us);
            } else if (hr < 0) {
                worker_metrics_add_tls_fail(worker_idx);
                tcp_fsm_reset(worker_idx, tcb);
            }
            return (int)len;
        }

        if (tcb->app_state == 21) {
            /* TLS established — decrypt and handle */
            uint8_t plain[4096];
            int plen = tls_decrypt(ts, data, len, plain, sizeof(plain));
            if (plen <= 0) return (int)len;
            worker_metrics_add_tls_rx(worker_idx);

            if (l->handler == SRV_HANDLER_TLS_ECHO) {
                /* Encrypt and echo back */
                uint8_t ct[4096];
                int ct_len = tls_encrypt(ts, plain, (uint32_t)plen,
                                          ct, sizeof(ct));
                if (ct_len > 0) {
                    tcp_fsm_send(worker_idx, tcb,
                                 ct, (uint32_t)ct_len);
                    l->tx_bytes += ct_len;
                    worker_metrics_add_tls_tx(worker_idx);
                }
            } else {
                /* HTTPS: look for HTTP request in plaintext */
                const uint8_t *end = memmem(plain, plen, "\r\n\r\n", 4);
                if (end && tbl->http_response_len > 0) {
                    uint8_t ct[4096];
                    int ct_len = tls_encrypt(ts,
                                             tbl->http_response,
                                             tbl->http_response_len,
                                             ct, sizeof(ct));
                    if (ct_len > 0) {
                        tcp_fsm_send(worker_idx, tcb,
                                     ct, (uint32_t)ct_len);
                        l->tx_bytes += ct_len;
                        l->http_resps_sent++;
                        worker_metrics_add_tls_tx(worker_idx);
                        worker_metrics_add_http_req(worker_idx);
                    }
                }
            }
            return (int)len;
        }
        return (int)len;
    }

    default:
        return (int)len;
    }
}
