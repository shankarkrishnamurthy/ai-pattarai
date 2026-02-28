/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: DPDK Cryptodev integration (§4.4) — async symmetric crypto.
 *
 * Used for bulk AES-GCM encryption/decryption when a hardware crypto
 * PMD is available (e.g. QAT, NITROX, SNOW3G).  Falls back to
 * OpenSSL software path when no crypto device is present.
 */
#ifndef TGEN_CRYPTODEV_H
#define TGEN_CRYPTODEV_H

#include <stdint.h>
#include <stddef.h>
#include <rte_crypto.h>
#include <rte_cryptodev.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRYPTODEV_MAX_DEVS  8u
#define CRYPTODEV_QP_DEPTH 2048u

typedef enum {
    CRYPTO_DIR_ENCRYPT = 0,
    CRYPTO_DIR_DECRYPT = 1,
} crypto_dir_t;

typedef struct {
    uint8_t  key[32];       /**< AES key (16, 24, or 32 bytes valid) */
    uint8_t  key_len;       /**< 16 / 24 / 32                         */
    uint8_t  iv[12];        /**< GCM IV (96 bit)                      */
    uint8_t  aad[64];       /**< Additional authenticated data        */
    uint16_t aad_len;
    uint8_t  digest[16];    /**< GCM authentication tag               */
    crypto_dir_t dir;
} crypto_op_params_t;

/**
 * Initialise all available DPDK crypto devices.
 * Returns number of devices initialised (0 = software fallback only).
 */
int cryptodev_init(void);

/** Release all crypto devices. */
void cryptodev_fini(void);

/**
 * Submit an AES-128/256-GCM crypto operation.
 * Completion is polled via cryptodev_poll_completions().
 * @param worker_idx  Determines which QP to use.
 * @param src         Input data.
 * @param dst         Output data.
 * @param len         Data length in bytes.
 * @param params      Key, IV, AAD.
 * @param user_data   Opaque pointer returned in completion.
 * Returns 0 on success, negative on error.
 */
int cryptodev_submit(uint32_t worker_idx,
                     const uint8_t *src, uint8_t *dst, uint32_t len,
                     const crypto_op_params_t *params, void *user_data);

/**
 * Poll completed crypto operations for this worker.
 * Calls 'cb(user_data, rc)' for each completed op.
 * Returns number of completions processed.
 */
typedef void (*cryptodev_cb_t)(void *user_data, int rc);
uint16_t cryptodev_poll_completions(uint32_t worker_idx, cryptodev_cb_t cb);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_CRYPTODEV_H */
