/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TLS engine implementation (OpenSSL memory-BIO).
 */
#include "tls_engine.h"
#include "../telemetry/log.h"
#include <string.h>
#include <errno.h>

#ifndef HAVE_OPENSSL

int  tls_ctx_init(tls_ctx_t *c, const char *a, const char *b,
                  const char *d, bool e)
{ (void)c;(void)a;(void)b;(void)d;(void)e; return -ENOTSUP; }
void tls_ctx_fini(tls_ctx_t *c) { (void)c; }
int  tls_session_new(tls_session_t *s, tls_ctx_t *c,
                     uint32_t w, const char *n)
{ (void)s;(void)c;(void)w;(void)n; return -ENOTSUP; }
void tls_session_free(tls_session_t *s) { (void)s; }
int  tls_handshake(tls_session_t *s, const uint8_t *ci, size_t cl,
                   uint8_t *co, size_t *ol)
{ (void)s;(void)ci;(void)cl;(void)co;(void)ol; return -ENOTSUP; }
int  tls_encrypt(tls_session_t *s, const uint8_t *p, size_t pl,
                 uint8_t *c, size_t cb)
{ (void)s;(void)p;(void)pl;(void)c;(void)cb; return -ENOTSUP; }
int  tls_decrypt(tls_session_t *s, const uint8_t *c, size_t cl,
                 uint8_t *p, size_t pb)
{ (void)s;(void)c;(void)cl;(void)p;(void)pb; return -ENOTSUP; }
int  tls_shutdown(tls_session_t *s, uint8_t *co, size_t *ol)
{ (void)s;(void)co;(void)ol; return -ENOTSUP; }

#else /* HAVE_OPENSSL */

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */
static void
log_ssl_errors(const char *where)
{
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof(buf));
        TGEN_ERR(TGEN_LOG_TLS, "%s: %s\n", where, buf);
    }
}

/* ------------------------------------------------------------------ */
/* Context                                                              */
/* ------------------------------------------------------------------ */
int
tls_ctx_init(tls_ctx_t *ctx, const char *cert_pem, const char *key_pem,
             const char *ca_pem, bool is_server)
{
    OPENSSL_init_ssl(0, NULL);

    const SSL_METHOD *method =
        is_server ? TLS_server_method() : TLS_client_method();
    ctx->ssl_ctx = SSL_CTX_new(method);
    if (!ctx->ssl_ctx) {
        log_ssl_errors("SSL_CTX_new");
        return -ENOMEM;
    }
    ctx->is_server = is_server;

    /* Minimum TLS 1.2 */
    SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);

    /* Strong cipher suites only */
    SSL_CTX_set_cipher_list(ctx->ssl_ctx,
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384");

    if (cert_pem) {
        if (SSL_CTX_use_certificate_chain_file(ctx->ssl_ctx, cert_pem) != 1) {
            log_ssl_errors("use_certificate_chain_file");
            SSL_CTX_free(ctx->ssl_ctx); ctx->ssl_ctx = NULL;
            return -EINVAL;
        }
    }
    if (key_pem) {
        if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key_pem,
                                        SSL_FILETYPE_PEM) != 1) {
            log_ssl_errors("use_PrivateKey_file");
            SSL_CTX_free(ctx->ssl_ctx); ctx->ssl_ctx = NULL;
            return -EINVAL;
        }
    }
    if (ca_pem) {
        if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_pem, NULL) != 1) {
            log_ssl_errors("load_verify_locations");
            /* Non-fatal — continue without peer verification */
        }
        SSL_CTX_set_verify(ctx->ssl_ctx,
                           SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           NULL);
    }
    return 0;
}

void
tls_ctx_fini(tls_ctx_t *ctx)
{
    if (ctx && ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
        ctx->ssl_ctx = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Session                                                              */
/* ------------------------------------------------------------------ */
int
tls_session_new(tls_session_t *sess, tls_ctx_t *ctx,
                uint32_t worker_idx, const char *sni)
{
    memset(sess, 0, sizeof(*sess));
    sess->worker_idx = worker_idx;

    sess->rbio = BIO_new(BIO_s_mem());
    sess->wbio = BIO_new(BIO_s_mem());
    if (!sess->rbio || !sess->wbio) {
        BIO_free(sess->rbio); BIO_free(sess->wbio);
        return -ENOMEM;
    }

    sess->ssl = SSL_new(ctx->ssl_ctx);
    if (!sess->ssl) {
        BIO_free(sess->rbio); BIO_free(sess->wbio);
        log_ssl_errors("SSL_new");
        return -ENOMEM;
    }

    /* rbio: SSL reads ciphertext from here (we write to it)
     * wbio: SSL writes ciphertext here (we read from it)   */
    SSL_set_bio(sess->ssl, sess->rbio, sess->wbio);

    if (!ctx->is_server) {
        SSL_set_connect_state(sess->ssl);
        if (sni)
            SSL_set_tlsext_host_name(sess->ssl, sni);
    } else {
        SSL_set_accept_state(sess->ssl);
    }
    return 0;
}

void
tls_session_free(tls_session_t *sess)
{
    if (!sess) return;
    if (sess->ssl) {
        SSL_free(sess->ssl); /* also frees the BIOs */
        sess->ssl  = NULL;
        sess->rbio = NULL;
        sess->wbio = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Handshake                                                            */
/* ------------------------------------------------------------------ */
int
tls_handshake(tls_session_t *sess,
              const uint8_t *ciphertext_in,  size_t ct_in_len,
              uint8_t       *ciphertext_out, size_t *ct_out_len)
{
    *ct_out_len = 0;

    /* Feed incoming bytes into read BIO */
    if (ciphertext_in && ct_in_len > 0)
        BIO_write(sess->rbio, ciphertext_in, (int)ct_in_len);

    int rc = SSL_do_handshake(sess->ssl);
    int err = SSL_get_error(sess->ssl, rc);

    /* Drain outgoing ciphertext regardless of outcome */
    int pending = BIO_pending(sess->wbio);
    if (pending > 0 && ciphertext_out) {
        int got = BIO_read(sess->wbio, ciphertext_out, (int)*ct_out_len);
        *ct_out_len = got > 0 ? (size_t)got : 0;
    }

    if (rc == 1) {
        sess->handshake_done = true;
        return 1;
    }
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return 0; /* in progress */

    log_ssl_errors("SSL_do_handshake");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Encrypt / Decrypt                                                    */
/* ------------------------------------------------------------------ */
int
tls_encrypt(tls_session_t *sess,
            const uint8_t *plaintext, size_t pt_len,
            uint8_t *ciphertext, size_t ct_buf_len)
{
    int rc = SSL_write(sess->ssl, plaintext, (int)pt_len);
    if (rc <= 0) {
        log_ssl_errors("SSL_write");
        return -1;
    }
    int got = BIO_read(sess->wbio, ciphertext, (int)ct_buf_len);
    return got > 0 ? got : 0;
}

int
tls_decrypt(tls_session_t *sess,
            const uint8_t *ciphertext, size_t ct_len,
            uint8_t *plaintext, size_t pt_buf_len)
{
    if (ct_len > 0)
        BIO_write(sess->rbio, ciphertext, (int)ct_len);

    int got = SSL_read(sess->ssl, plaintext, (int)pt_buf_len);
    if (got <= 0) {
        int err = SSL_get_error(sess->ssl, got);
        if (err == SSL_ERROR_WANT_READ) return 0;
        log_ssl_errors("SSL_read");
        return -1;
    }
    return got;
}

/* ------------------------------------------------------------------ */
/* Shutdown                                                             */
/* ------------------------------------------------------------------ */
int
tls_shutdown(tls_session_t *sess,
             uint8_t *ciphertext_out, size_t *ct_out_len)
{
    *ct_out_len = 0;
    if (sess->shutdown_sent)
        return 0;
    SSL_shutdown(sess->ssl);
    sess->shutdown_sent = true;
    int got = BIO_read(sess->wbio, ciphertext_out, (int)*ct_out_len);
    *ct_out_len = got > 0 ? (size_t)got : 0;
    return 0;
}

#endif /* HAVE_OPENSSL */
