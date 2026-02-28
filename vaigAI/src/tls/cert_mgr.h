/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Certificate manager (§4.3) — load, rotate, OCSP stapling stub.
 */
#ifndef TGEN_CERT_MGR_H
#define TGEN_CERT_MGR_H

#include "tls_engine.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CERT_PATH_MAX 512

typedef struct {
    char cert_pem[CERT_PATH_MAX];
    char key_pem[CERT_PATH_MAX];
    char ca_pem[CERT_PATH_MAX];
    bool verify_peer;
    bool enable_session_resumption;  /* TLS session tickets */
} cert_cfg_t;

/**
 * Initialise global TLS contexts from certificate configuration.
 * Creates both a client and a server context.
 */
int cert_mgr_init(const cert_cfg_t *cfg,
                  tls_ctx_t *client_out, tls_ctx_t *server_out);

/**
 * Hot-reload certificates without dropping connections.
 * New SSL objects will use the new context; existing sessions continue
 * with the old one until they close naturally.
 */
int cert_mgr_reload(const cert_cfg_t *cfg,
                    tls_ctx_t *client_out, tls_ctx_t *server_out);

/** Gracefully tear down both contexts. */
void cert_mgr_fini(tls_ctx_t *client_ctx, tls_ctx_t *server_ctx);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_CERT_MGR_H */
