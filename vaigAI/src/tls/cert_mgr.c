/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Certificate manager implementation.
 */
#include "cert_mgr.h"
#include "../telemetry/log.h"
#include <string.h>

int
cert_mgr_init(const cert_cfg_t *cfg,
              tls_ctx_t *client_out, tls_ctx_t *server_out)
{
    int rc;

    /* Client context (no cert/key required) */
    rc = tls_ctx_init(client_out, NULL, NULL,
                      cfg->ca_pem[0] ? cfg->ca_pem : NULL, false);
    if (rc < 0) {
        TGEN_ERR(TGEN_LOG_TLS, "Failed to init client TLS context\n");
        return rc;
    }

    /* Server context */
    rc = tls_ctx_init(server_out,
                      cfg->cert_pem[0] ? cfg->cert_pem : NULL,
                      cfg->key_pem[0]  ? cfg->key_pem  : NULL,
                      cfg->ca_pem[0]   ? cfg->ca_pem   : NULL,
                      true);
    if (rc < 0) {
        TGEN_WARN(TGEN_LOG_TLS,
                  "Failed to init server TLS context (rc=%d) — "
                  "server TLS disabled\n", rc);
        memset(server_out, 0, sizeof(*server_out));
        /* Not fatal — server role may not be used */
    }

#ifdef HAVE_OPENSSL
    if (cfg->enable_session_resumption && client_out->ssl_ctx) {
        SSL_CTX_set_session_cache_mode(client_out->ssl_ctx,
                                       SSL_SESS_CACHE_CLIENT);
    }
#endif
    return 0;
}

int
cert_mgr_reload(const cert_cfg_t *cfg,
                tls_ctx_t *client_out, tls_ctx_t *server_out)
{
    /* Create new contexts */
    tls_ctx_t new_client, new_server;
    int rc = cert_mgr_init(cfg, &new_client, &new_server);
    if (rc < 0) return rc;

    /* Swap atomically (pointer assignments are naturally atomic on x86_64) */
    tls_ctx_fini(client_out);
    tls_ctx_fini(server_out);
    *client_out = new_client;
    *server_out = new_server;

    TGEN_INFO(TGEN_LOG_TLS, "Certificates reloaded successfully\n");
    return 0;
}

void
cert_mgr_fini(tls_ctx_t *client_ctx, tls_ctx_t *server_ctx)
{
    tls_ctx_fini(client_ctx);
    tls_ctx_fini(server_ctx);
}
