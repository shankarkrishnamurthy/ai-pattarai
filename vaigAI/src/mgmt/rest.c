/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: REST API server (libmicrohttpd).
 */
#include "rest.h"
#include "config_mgr.h"
#include "../core/core_assign.h"
#include "../core/worker_loop.h"
#include "../telemetry/metrics.h"
#include "../telemetry/export.h"
#include "../telemetry/log.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#ifdef HAVE_MICROHTTPD
# include <microhttpd.h>
# ifdef HAVE_JANSSON
#  include <jansson.h>
# endif

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */
static struct MHD_Daemon *g_daemon;

/* ------------------------------------------------------------------ */
/* Response helpers                                                     */
/* ------------------------------------------------------------------ */
static enum MHD_Result
send_text(struct MHD_Connection *conn,
          unsigned int status, const char *content_type,
          const char *body, size_t body_len)
{
    struct MHD_Response *rsp =
        MHD_create_response_from_buffer((size_t)body_len,
                                        (void *)body,
                                        MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(rsp, "Content-Type", content_type);
    MHD_add_response_header(rsp, "Access-Control-Allow-Origin", "*");
    enum MHD_Result ret = MHD_queue_response(conn, status, rsp);
    MHD_destroy_response(rsp);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Request handler                                                      */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_request(void *cls,
               struct MHD_Connection *connection,
               const char *url,
               const char *method,
               const char *version,
               const char *upload_data,
               size_t *upload_data_size,
               void **con_cls)
{
    (void)cls; (void)version;

    /* First call with empty body — signal readiness */
    if (*con_cls == NULL) {
        static int dummy;
        *con_cls = &dummy;
        return MHD_YES;
    }

    char buf[65536];
    int  n = 0;

    if (strcmp(url, "/api/v1/stats") == 0 && strcmp(method, "GET") == 0) {
        metrics_snapshot_t snap;
        uint32_t nw = g_core_map.num_workers ? g_core_map.num_workers : 1;
        metrics_snapshot(&snap, nw);
        n = export_json(&snap, buf, sizeof(buf));
        return send_text(connection, MHD_HTTP_OK, "application/json", buf,
                         n > 0 ? (size_t)n : 0);
    }

    if (strcmp(url, "/api/v1/metrics") == 0 && strcmp(method, "GET") == 0) {
        metrics_snapshot_t snap;
        uint32_t nw = g_core_map.num_workers ? g_core_map.num_workers : 1;
        metrics_snapshot(&snap, nw);
        n = export_prometheus(&snap, buf, sizeof(buf));
        return send_text(connection, MHD_HTTP_OK,
                         "text/plain; version=0.0.4", buf,
                         n > 0 ? (size_t)n : 0);
    }

    if (strcmp(url, "/api/v1/config") == 0 && strcmp(method, "GET") == 0) {
#ifdef HAVE_JANSSON
        /* Quick-dump the current config as JSON */
        const char *placeholder = "{\"status\":\"ok\"}";
        return send_text(connection, MHD_HTTP_OK, "application/json",
                         placeholder, strlen(placeholder));
#else
        const char *err = "{\"error\":\"jansson not compiled in\"}";
        return send_text(connection, MHD_HTTP_SERVICE_UNAVAILABLE,
                         "application/json", err, strlen(err));
#endif
    }

    if (strcmp(url, "/api/v1/config") == 0 && strcmp(method, "PUT") == 0) {
        if (*upload_data_size > 0) {
            config_apply_patch(upload_data);
            *upload_data_size = 0;
            return MHD_YES;
        }
        const char *ok = "{\"status\":\"applied\"}";
        return send_text(connection, MHD_HTTP_OK, "application/json",
                         ok, strlen(ok));
    }

    if (strcmp(url, "/api/v1/start") == 0 && strcmp(method, "POST") == 0) {
        /* Start traffic generation (toggle g_traffic; process stays alive). */
        g_traffic = 1;
        const char *ok = "{\"status\":\"started\"}";
        return send_text(connection, MHD_HTTP_OK, "application/json",
                         ok, strlen(ok));
    }

    if (strcmp(url, "/api/v1/stop") == 0 && strcmp(method, "POST") == 0) {
        /* Stop traffic generation only; process/REST server remain alive. */
        g_traffic = 0;
        const char *ok = "{\"status\":\"stopped\"}";
        return send_text(connection, MHD_HTTP_OK, "application/json",
                         ok, strlen(ok));
    }

    const char *not_found = "{\"error\":\"not found\"}";
    return send_text(connection, MHD_HTTP_NOT_FOUND, "application/json",
                     not_found, strlen(not_found));
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
int
rest_server_start(uint16_t port)
{
    g_daemon = MHD_start_daemon(
            MHD_USE_INTERNAL_POLLING_THREAD,
            port,
            NULL, NULL,
            &handle_request, NULL,
            MHD_OPTION_END);
    if (!g_daemon) {
        TGEN_ERR(TGEN_LOG_MGMT, "Failed to start REST server on port %u\n",
                 port);
        return -EIO;
    }
    TGEN_INFO(TGEN_LOG_MGMT, "REST server listening on port %u\n", port);
    return 0;
}

void
rest_server_stop(void)
{
    if (g_daemon) {
        MHD_stop_daemon(g_daemon);
        g_daemon = NULL;
    }
}

#else /* !HAVE_MICROHTTPD */

int rest_server_start(uint16_t port) {
    (void)port;
    TGEN_WARN(TGEN_LOG_MGMT,
              "REST server disabled (libmicrohttpd not available)\n");
    return 0;
}
void rest_server_stop(void) {}

#endif /* HAVE_MICROHTTPD */
