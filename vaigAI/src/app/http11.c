/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: HTTP/1.1 framing, request builder, response parser.
 */
#include "http11.h"
#include "../telemetry/metrics.h"
#include "../telemetry/log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */
static const char *method_str[] = { "GET","POST","PUT","DELETE","HEAD" };

static int
append(uint8_t *buf, size_t *pos, size_t cap, const char *s, size_t slen)
{
    if (*pos + slen >= cap) return -ENOSPC;
    memcpy(buf + *pos, s, slen);
    *pos += slen;
    return 0;
}

#define APPC(buf, pos, cap, s) append(buf, pos, cap, s, sizeof(s)-1)
#define APPS(buf, pos, cap, s) append(buf, pos, cap, s, strlen(s))

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */
void
http11_conn_init(http_conn_t *conn)
{
    memset(conn, 0, sizeof(*conn));
    conn->state = HTTP_IDLE;
}

/* ------------------------------------------------------------------ */
/* TX: request builder                                                  */
/* ------------------------------------------------------------------ */
int
http11_tx_request(http_conn_t *conn, const http_request_t *req)
{
    uint8_t *b  = conn->tx_hdr;
    size_t   p  = 0;
    size_t   cap = sizeof(conn->tx_hdr);
    char     tmp[64];

    /* Request line */
    APPS(b, &p, cap, method_str[req->method]);
    APPC(b, &p, cap, " ");
    APPS(b, &p, cap, req->url ? req->url : "/");
    APPC(b, &p, cap, " HTTP/1.1\r\n");

    /* Mandatory Host header */
    APPC(b, &p, cap, "Host: ");
    APPS(b, &p, cap, req->host ? req->host : "localhost");
    APPC(b, &p, cap, "\r\n");

    /* Connection */
    if (req->keep_alive)
        APPC(b, &p, cap, "Connection: keep-alive\r\n");
    else
        APPC(b, &p, cap, "Connection: close\r\n");

    /* Body headers */
    if (req->body && req->body_len > 0) {
        if (req->content_type) {
            APPC(b, &p, cap, "Content-Type: ");
            APPS(b, &p, cap, req->content_type);
            APPC(b, &p, cap, "\r\n");
        }
        int n = snprintf(tmp, sizeof(tmp),
                         "Content-Length: %"PRIu32"\r\n", req->body_len);
        append(b, &p, cap, tmp, (size_t)n);
    }

    APPC(b, &p, cap, "\r\n");

    /* Append body inline if present */
    if (req->body && req->body_len > 0) {
        if (p + req->body_len >= cap) return -ENOSPC;
        memcpy(b + p, req->body, req->body_len);
        p += req->body_len;
    }

    conn->tx_hdr_len = (uint32_t)p;
    conn->pipeline_depth++;
    conn->state = HTTP_WAIT_STATUS;
    return (int)p;
}

/* ------------------------------------------------------------------ */
/* RX: response parser state machine                                   */
/* ------------------------------------------------------------------ */

/* Scan for \r\n and return pointer to first char after it, or NULL. */
static const char *
find_crlf(const char *s, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++)
        if (s[i] == '\r' && s[i+1] == '\n')
            return s + i + 2;
    return NULL;
}

static int
parse_status_line(http_response_t *rsp, const char *line, size_t len)
{
    /* "HTTP/1.1 200 OK\r\n" */
    if (len < 12) return -1;
    if (memcmp(line, "HTTP/1.", 7) != 0) return -1;
    rsp->status_code = (uint16_t)atoi(line + 9);
    return 0;
}

static int
parse_header(http_response_t *rsp, char *line, size_t len)
{
    /* Destructively null-terminate name and value */
    char *colon = memchr(line, ':', len);
    if (!colon) return 0;
    *colon = '\0';
    char *val = colon + 1;
    while (*val == ' ') val++;
    /* Strip trailing \r\n */
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen-1] == '\r' || val[vlen-1] == '\n'))
        val[--vlen] = '\0';

    /* Check for significant headers */
    if (strcasecmp(line, "content-length") == 0) {
        rsp->content_length = (uint32_t)strtoul(val, NULL, 10);
    } else if (strcasecmp(line, "transfer-encoding") == 0 &&
               strstr(val, "chunked")) {
        rsp->chunked = true;
    } else if (strcasecmp(line, "connection") == 0) {
        rsp->keep_alive = (strcasestr(val, "keep-alive") != NULL);
    }

    if (rsp->header_count < HTTP_MAX_HEADERS) {
        rsp->headers[rsp->header_count].name  = line;
        rsp->headers[rsp->header_count].value = val;
        rsp->header_count++;
    }
    return 0;
}

int
http11_rx_data(http_conn_t *conn, uint32_t worker_idx, uint32_t conn_idx,
               const uint8_t *data, uint32_t len,
               http_response_cb_t cb, void *user_data)
{
    (void)conn_idx;

    /* Append to ring buffer */
    if (conn->rx_head + len > sizeof(conn->rx_buf)) {
        worker_metrics_add_http_parse_err(worker_idx);
        return -ENOSPC;
    }
    memcpy(conn->rx_buf + conn->rx_head, data, len);
    conn->rx_head += len;

    /* Loop while data remains *or* we need to fire the HTTP_DONE callback
     * (state may become DONE on the same iteration that drains rx_buf). */
    while (conn->rx_parsed < conn->rx_head || conn->state == HTTP_DONE) {
        char    *buf  = (char *)(conn->rx_buf + conn->rx_parsed);
        size_t   avail = conn->rx_head - conn->rx_parsed;

        if (conn->state == HTTP_IDLE || conn->state == HTTP_WAIT_STATUS) {
            const char *eol = find_crlf(buf, avail);
            if (!eol) break;  /* wait for more data */

            static http_response_t rsp_ctx;
            memset(&rsp_ctx, 0, sizeof(rsp_ctx));
            rsp_ctx.keep_alive = false;

            if (parse_status_line(&rsp_ctx, buf, (size_t)(eol - buf - 2)) < 0){
                worker_metrics_add_http_parse_err(worker_idx);
                return -EBADMSG;
            }
            conn->rx_parsed += (uint32_t)(eol - buf);
            conn->state = HTTP_WAIT_HEADERS;

            /* Store parsed response in context (one per call — simplified) */
            *(http_response_t *)(conn->rx_buf + sizeof(conn->rx_buf)
                                - sizeof(http_response_t)) = rsp_ctx;

        } else if (conn->state == HTTP_WAIT_HEADERS) {
            http_response_t *rsp = (http_response_t *)(conn->rx_buf
                    + sizeof(conn->rx_buf) - sizeof(http_response_t));

            const char *eol = find_crlf(buf, avail);
            if (!eol) break;

            size_t line_len = (size_t)(eol - buf - 2);
            if (line_len == 0) {
                /* Blank line = end of headers */
                conn->rx_parsed += 2; /* skip \r\n */
                conn->state = rsp->chunked ? HTTP_WAIT_CHUNK : HTTP_WAIT_BODY;
            } else {
                /* Parse header (destructive) */
                char tmp[512];
                size_t cplen = line_len < sizeof(tmp)-1 ? line_len : sizeof(tmp)-1;
                memcpy(tmp, buf, cplen);
                tmp[cplen] = '\0';
                parse_header(rsp, tmp, cplen);
                conn->rx_parsed += (uint32_t)(eol - buf);
            }

        } else if (conn->state == HTTP_WAIT_BODY) {
            http_response_t *rsp = (http_response_t *)(conn->rx_buf
                    + sizeof(conn->rx_buf) - sizeof(http_response_t));

            if (rsp->content_length == 0) {
                rsp->body     = NULL;
                rsp->body_len = 0;
                conn->state   = HTTP_DONE;
            } else if (avail >= rsp->content_length) {
                rsp->body     = (const uint8_t *)buf;
                rsp->body_len = rsp->content_length;
                conn->rx_parsed += rsp->content_length;
                conn->state   = HTTP_DONE;
            } else {
                break; /* wait for more */
            }

        } else if (conn->state == HTTP_WAIT_CHUNK) {
            /* Minimal chunked decoder */
            http_response_t *rsp = (http_response_t *)(conn->rx_buf
                    + sizeof(conn->rx_buf) - sizeof(http_response_t));
            /* Find chunk size line */
            const char *eol = find_crlf(buf, avail);
            if (!eol) break;
            uint32_t chunk_sz = (uint32_t)strtoul(buf, NULL, 16);
            conn->rx_parsed += (uint32_t)(eol - buf);
            if (chunk_sz == 0) {
                /* Last chunk */
                conn->rx_parsed += 2; /* trailing \r\n */
                conn->state = HTTP_DONE;
            } else {
                buf   = (char *)(conn->rx_buf + conn->rx_parsed);
                avail = conn->rx_head - conn->rx_parsed;
                if (avail < chunk_sz + 2) break;
                rsp->body     = (const uint8_t *)buf;
                rsp->body_len = chunk_sz;
                conn->rx_parsed += chunk_sz + 2; /* +2 for \r\n after chunk */
            }

        } else { /* HTTP_DONE */
            http_response_t *rsp = (http_response_t *)(conn->rx_buf
                    + sizeof(conn->rx_buf) - sizeof(http_response_t));

            /* Fire completion callback */
            if (cb)
                cb(worker_idx, conn_idx, rsp, user_data);

            worker_metrics_add_http_rsp(worker_idx, rsp->status_code);
            conn->pipeline_depth--;

            /* Compact consumed bytes */
            uint32_t remaining = conn->rx_head - conn->rx_parsed;
            if (remaining > 0)
                memmove(conn->rx_buf, conn->rx_buf + conn->rx_parsed,
                        remaining);
            conn->rx_head   = remaining;
            conn->rx_parsed = 0;

            conn->state = conn->pipeline_depth > 0 ?
                          HTTP_WAIT_STATUS : HTTP_IDLE;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* TX: server response builder                                          */
/* ------------------------------------------------------------------ */
int
http11_tx_response(uint8_t *buf, size_t buf_len,
                   uint16_t status, const char *status_str,
                   const char *content_type,
                   const uint8_t *body, uint32_t body_len)
{
    char tmp[128];
    size_t p = 0;
    size_t cap = buf_len;

    int n = snprintf(tmp, sizeof(tmp), "HTTP/1.1 %u %s\r\n",
                     status, status_str ? status_str : "");
    append(buf, &p, cap, tmp, (size_t)n);

    if (content_type) {
        APPC(buf, &p, cap, "Content-Type: ");
        APPS(buf, &p, cap, content_type);
        APPC(buf, &p, cap, "\r\n");
    }
    n = snprintf(tmp, sizeof(tmp), "Content-Length: %"PRIu32"\r\n", body_len);
    append(buf, &p, cap, tmp, (size_t)n);
    APPC(buf, &p, cap, "Connection: keep-alive\r\n");
    APPC(buf, &p, cap, "\r\n");

    if (body && body_len > 0) {
        if (p + body_len >= cap) return -ENOSPC;
        memcpy(buf + p, body, body_len);
        p += body_len;
    }
    return (int)p;
}
