/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TLS engine — OpenSSL BIO-pair per connection (§4.1).
 *
 * Each TCP connection optionally has a TLS session.  OpenSSL is used in
 * memory-BIO mode so no file descriptors or blocking I/O are needed.
 * The worker feeds plaintext through SSL_write() and ciphertext is read
 * from the BIO and pushed onto the wire; incoming ciphertext is written
 * to the BIO and plaintext is read with SSL_read().
 *
 * TLS 1.2 and 1.3 are supported; TLS 1.0/1.1 disabled at context
 * creation (§4.1).
 */
#ifndef TGEN_TLS_ENGINE_H
#define TGEN_TLS_ENGINE_H

#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Global TLS context (one per role: client / server)                   */
/* ------------------------------------------------------------------ */
typedef struct {
#ifdef HAVE_OPENSSL
    SSL_CTX *ssl_ctx;
#else
    void    *ssl_ctx; /* placeholder when compiled without OpenSSL */
#endif
    bool     is_server;
} tls_ctx_t;

/**
 * Initialise a global TLS context.
 * @param cert_pem  Path to PEM certificate (server) or NULL (client).
 * @param key_pem   Path to PEM private key  (server) or NULL (client).
 * @param ca_pem    Path to CA bundle for peer verification, or NULL.
 * @param is_server True for server contexts.
 */
int  tls_ctx_init(tls_ctx_t *ctx,
                  const char *cert_pem, const char *key_pem,
                  const char *ca_pem,   bool is_server);

void tls_ctx_fini(tls_ctx_t *ctx);

/* ------------------------------------------------------------------ */
/* Per-connection TLS session                                           */
/* ------------------------------------------------------------------ */
typedef struct {
#ifdef HAVE_OPENSSL
    SSL *ssl;
    BIO *rbio;  /* ciphertext input  (worker writes, SSL reads)  */
    BIO *wbio;  /* ciphertext output (SSL writes, worker reads)  */
#else
    void *ssl, *rbio, *wbio;
#endif
    bool     handshake_done;
    bool     shutdown_sent;
    uint32_t worker_idx;
} tls_session_t;

/** Allocate and attach a TLS session to an existing TCP connection. */
int  tls_session_new(tls_session_t *sess, tls_ctx_t *ctx,
                     uint32_t worker_idx, const char *sni);
void tls_session_free(tls_session_t *sess);

/**
 * Drive the TLS handshake.  Call repeatedly until returns 1 (done)
 * or negative (fatal error).  Returns 0 while still in progress.
 * @param ciphertext_in   Incoming ciphertext bytes from wire (may be NULL).
 * @param ct_in_len       Length of ciphertext_in.
 * @param ciphertext_out  Buffer to receive outgoing handshake bytes.
 * @param ct_out_len      In: buffer size.  Out: bytes written.
 */
int  tls_handshake(tls_session_t *sess,
                   const uint8_t *ciphertext_in,  size_t ct_in_len,
                   uint8_t       *ciphertext_out, size_t *ct_out_len);

/**
 * Encrypt plaintext → ciphertext (post-handshake).
 * Returns bytes written to ciphertext_out, or negative on error.
 */
int  tls_encrypt(tls_session_t *sess,
                 const uint8_t *plaintext,   size_t pt_len,
                 uint8_t       *ciphertext,  size_t ct_buf_len);

/**
 * Decrypt ciphertext → plaintext (post-handshake).
 * Returns bytes written to plaintext_out, or negative on error.
 */
int  tls_decrypt(tls_session_t *sess,
                 const uint8_t *ciphertext,  size_t ct_len,
                 uint8_t       *plaintext,   size_t pt_buf_len);

/** Initiate a TLS close_notify shutdown. */
int  tls_shutdown(tls_session_t *sess,
                  uint8_t *ciphertext_out, size_t *ct_out_len);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TLS_ENGINE_H */
