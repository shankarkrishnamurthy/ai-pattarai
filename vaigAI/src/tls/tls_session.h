/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Per-connection TLS session store (§4.2).
 *
 * Wraps tls_engine.h with per-lcore pre-allocated session arrays to
 * avoid heap allocations in the data path.
 */
#ifndef TGEN_TLS_SESSION_H
#define TGEN_TLS_SESSION_H

#include "tls_engine.h"
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum TLS sessions per worker (matches max TCP connections). */
#ifndef TGEN_MAX_TLS_SESSIONS
# define TGEN_MAX_TLS_SESSIONS 1000000u
#endif

/**
 * Initialise TLS session storage for all workers.
 * @param client_ctx  TLS context for outbound connections.
 * @param server_ctx  TLS context for inbound connections (may be NULL).
 */
int  tls_session_store_init(tls_ctx_t *client_ctx, tls_ctx_t *server_ctx);
void tls_session_store_fini(void);

/**
 * Get the TLS session for a given (worker, connection) index.
 * 'conn_idx' is the same index used in the TCB store.
 * Returns NULL if TLS is not active on that connection.
 */
tls_session_t *tls_session_get(uint32_t worker_idx, uint32_t conn_idx);

/**
 * Attach a new TLS session to connection conn_idx on worker worker_idx.
 * Caller specifies client or server mode via 'is_server'.
 */
int tls_session_attach(uint32_t worker_idx, uint32_t conn_idx,
                       bool is_server, const char *sni);

/** Detach and free the TLS session for a connection. */
void tls_session_detach(uint32_t worker_idx, uint32_t conn_idx);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TLS_SESSION_H */
