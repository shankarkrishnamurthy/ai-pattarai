/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: HTTP/1.1 client and server (§5.1).
 *
 * Both roles share the same zero-copy parser and template engine.
 * Persistent connections (keep-alive) and pipelining are supported.
 * HTTP/2 and HTTP/3 are out of scope for this release.
 *
 * The HTTP engine sits above the TCP+TLS layers:
 *   app calls http11_tx_request()  → http11 → tcp_fsm_send()
 *   tcp delivers data              → http11_rx_data()  → app callback
 */
#ifndef TGEN_HTTP11_H
#define TGEN_HTTP11_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* HTTP request descriptor                                              */
/* ------------------------------------------------------------------ */
typedef enum {
    HTTP_METHOD_GET     = 0,
    HTTP_METHOD_POST    = 1,
    HTTP_METHOD_PUT     = 2,
    HTTP_METHOD_DELETE  = 3,
    HTTP_METHOD_HEAD    = 4,
} http_method_t;

typedef struct {
    http_method_t  method;
    const char    *url;          /**< null-terminated, e.g. "/index.html" */
    const char    *host;         /**< Host: header value                  */
    const char    *content_type; /**< NULL = no body                      */
    const uint8_t *body;         /**< request body (may be NULL)          */
    uint32_t       body_len;
    bool           keep_alive;   /**< Connection: keep-alive              */
} http_request_t;

/* ------------------------------------------------------------------ */
/* HTTP response descriptor (parsed from wire)                          */
/* ------------------------------------------------------------------ */
#define HTTP_MAX_HEADERS 32u
#define HTTP_MAX_BODY    (1u << 20) /* 1 MB max body buffer */

typedef struct {
    uint16_t   status_code;
    uint32_t   content_length;  /**< 0 = chunked or unknown              */
    bool       keep_alive;
    bool       chunked;
    /* First HTTP_MAX_HEADERS name:value pairs (pointers into rx_buf) */
    struct { const char *name; const char *value; } headers[HTTP_MAX_HEADERS];
    uint8_t    header_count;
    const uint8_t *body;        /**< pointer into rx buffer (zero-copy)  */
    uint32_t   body_len;
} http_response_t;

/* ------------------------------------------------------------------ */
/* Per-connection HTTP state                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    /* RX ring-buffer for reassembling HTTP messages */
    uint8_t  rx_buf[HTTP_MAX_BODY + 4096];
    uint32_t rx_head;   /* bytes in rx_buf         */
    uint32_t rx_parsed; /* bytes consumed by parser */

    /* TX scatter-gather list (built by http11_tx_request) */
    uint8_t  tx_hdr[2048]; /* serialised request headers */
    uint32_t tx_hdr_len;

    /* Pipeline depth: number of requests in flight */
    uint8_t  pipeline_depth;

    /* State machine */
    enum {
        HTTP_IDLE = 0,
        HTTP_WAIT_STATUS,
        HTTP_WAIT_HEADERS,
        HTTP_WAIT_BODY,
        HTTP_WAIT_CHUNK,
        HTTP_DONE,
    } state;
} http_conn_t;

/* ------------------------------------------------------------------ */
/* Completion callback                                                  */
/* ------------------------------------------------------------------ */
typedef void (*http_response_cb_t)(uint32_t worker_idx,
                                   uint32_t conn_idx,
                                   const http_response_t *rsp,
                                   void *user_data);

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/** Initialise a new per-connection HTTP state. */
void http11_conn_init(http_conn_t *conn);

/**
 * Build and enqueue an HTTP/1.1 request for transmission.
 * The serialised headers are placed in conn->tx_hdr.
 * Caller then passes them to tcp_fsm_send().
 * Returns bytes written to conn->tx_hdr, or negative on error.
 */
int http11_tx_request(http_conn_t *conn, const http_request_t *req);

/**
 * Feed incoming TCP payload data into the HTTP parser.
 * Calls 'cb' for each fully received HTTP response.
 * Returns 0 on success, negative on parse error.
 */
int http11_rx_data(http_conn_t *conn, uint32_t worker_idx, uint32_t conn_idx,
                   const uint8_t *data, uint32_t len,
                   http_response_cb_t cb, void *user_data);

/**
 * Build a minimal HTTP/1.1 response (server mode).
 * Writes headers + optional body into 'buf'.
 * Returns bytes written, or negative on error.
 */
int http11_tx_response(uint8_t *buf, size_t buf_len,
                       uint16_t status, const char *status_str,
                       const char *content_type,
                       const uint8_t *body, uint32_t body_len);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_HTTP11_H */
