/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TLS session store implementation.
 */
#include "tls_session.h"
#include "../telemetry/log.h"
#include "../core/core_assign.h"
#include <rte_malloc.h>
#include <string.h>

static tls_ctx_t    *g_client_ctx;
static tls_ctx_t    *g_server_ctx;

/* Per-worker flat arrays; index = conn_idx.
 * NULL entry = no TLS on that connection. */
static tls_session_t **g_sessions[TGEN_MAX_WORKERS];

int
tls_session_store_init(tls_ctx_t *client_ctx, tls_ctx_t *server_ctx)
{
    g_client_ctx = client_ctx;
    g_server_ctx = server_ctx;

    uint32_t n_workers = g_core_map.num_workers ? g_core_map.num_workers
                                                 : 1;
    for (uint32_t w = 0; w < n_workers; w++) {
        g_sessions[w] = rte_zmalloc_socket("tls_sess_ptrs",
                TGEN_MAX_TLS_SESSIONS * sizeof(tls_session_t *),
                RTE_CACHE_LINE_SIZE, SOCKET_ID_ANY);
        if (!g_sessions[w]) {
            TGEN_ERR(TGEN_LOG_TLS, "OOM tls session store w=%u\n", w);
            return -ENOMEM;
        }
    }
    return 0;
}

void
tls_session_store_fini(void)
{
    uint32_t n_workers = g_core_map.num_workers ? g_core_map.num_workers : 1;
    for (uint32_t w = 0; w < n_workers; w++) {
        if (!g_sessions[w]) continue;
        for (uint32_t i = 0; i < TGEN_MAX_TLS_SESSIONS; i++) {
            if (g_sessions[w][i]) {
                tls_session_free(g_sessions[w][i]);
                rte_free(g_sessions[w][i]);
            }
        }
        rte_free(g_sessions[w]);
        g_sessions[w] = NULL;
    }
}

tls_session_t *
tls_session_get(uint32_t worker_idx, uint32_t conn_idx)
{
    if (worker_idx >= TGEN_MAX_WORKERS || conn_idx >= TGEN_MAX_TLS_SESSIONS)
        return NULL;
    return g_sessions[worker_idx][conn_idx];
}

int
tls_session_attach(uint32_t worker_idx, uint32_t conn_idx,
                   bool is_server, const char *sni)
{
    if (worker_idx >= TGEN_MAX_WORKERS || conn_idx >= TGEN_MAX_TLS_SESSIONS)
        return -EINVAL;

    tls_ctx_t *ctx = is_server ? g_server_ctx : g_client_ctx;
    if (!ctx) return -ENOENT;

    tls_session_t *sess = rte_zmalloc_socket("tls_sess",
            sizeof(tls_session_t), RTE_CACHE_LINE_SIZE, SOCKET_ID_ANY);
    if (!sess) return -ENOMEM;

    int rc = tls_session_new(sess, ctx, worker_idx, sni);
    if (rc < 0) {
        rte_free(sess);
        return rc;
    }
    g_sessions[worker_idx][conn_idx] = sess;
    return 0;
}

void
tls_session_detach(uint32_t worker_idx, uint32_t conn_idx)
{
    if (worker_idx >= TGEN_MAX_WORKERS || conn_idx >= TGEN_MAX_TLS_SESSIONS)
        return;
    tls_session_t *sess = g_sessions[worker_idx][conn_idx];
    if (!sess) return;
    tls_session_free(sess);
    rte_free(sess);
    g_sessions[worker_idx][conn_idx] = NULL;
}
