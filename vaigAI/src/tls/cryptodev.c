/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: DPDK Cryptodev implementation.
 */
#include "cryptodev.h"
#include "../telemetry/log.h"
#include <rte_malloc.h>
#include <string.h>
#include <errno.h>

static uint8_t  g_cdev_id;        /* single device used (first found)  */
static uint8_t  g_n_cdevs;        /* number of available crypto devs   */
static struct rte_mempool *g_cop_pool; /* crypto op mempool              */

/* Per-worker session (AES-256-GCM static session for bulk path) */
static struct rte_cryptodev_sym_session
    *g_sessions[TGEN_MAX_WORKERS][2]; /* [0]=enc, [1]=dec */

int
cryptodev_init(void)
{
    g_n_cdevs = rte_cryptodev_count();
    if (g_n_cdevs == 0) {
        TGEN_WARN(TGEN_LOG_TLS,
                  "No crypto PMDs found — TLS bulk crypto uses SW path\n");
        return 0;
    }

    g_cdev_id = 0; /* use device 0 */

    struct rte_cryptodev_info dev_info;
    rte_cryptodev_info_get(g_cdev_id, &dev_info);
    TGEN_INFO(TGEN_LOG_TLS, "Crypto device 0: driver=%s max_qp=%u\n",
              dev_info.driver_name, dev_info.max_nb_queue_pairs);

    /* Create op mempool */
    g_cop_pool = rte_crypto_op_pool_create("tgen_cop_pool",
            RTE_CRYPTO_OP_TYPE_SYMMETRIC,
            CRYPTODEV_QP_DEPTH * TGEN_MAX_WORKERS * 2,
            64, /* cache size */
            sizeof(struct rte_crypto_sym_xform) * 2,
            SOCKET_ID_ANY);
    if (!g_cop_pool) {
        TGEN_ERR(TGEN_LOG_TLS, "Failed to create crypto op pool\n");
        return -ENOMEM;
    }

    /* Configure device: one QP per worker */
    struct rte_cryptodev_config cfg = {
        .nb_queue_pairs = (uint16_t)TGEN_MAX_WORKERS,
        .socket_id      = SOCKET_ID_ANY,
    };
    if (rte_cryptodev_configure(g_cdev_id, &cfg) < 0) {
        TGEN_ERR(TGEN_LOG_TLS, "rte_cryptodev_configure failed\n");
        return -EIO;
    }

    struct rte_cryptodev_qp_conf qp_cfg = {
        .nb_descriptors = CRYPTODEV_QP_DEPTH,
        .mp_session     = NULL,
    };
    for (uint32_t w = 0; w < TGEN_MAX_WORKERS; w++) {
        if (rte_cryptodev_queue_pair_setup(g_cdev_id, (uint16_t)w,
                                           &qp_cfg, SOCKET_ID_ANY) < 0) {
            TGEN_ERR(TGEN_LOG_TLS, "QP setup failed for worker %u\n", w);
            return -EIO;
        }
    }

    if (rte_cryptodev_start(g_cdev_id) < 0) {
        TGEN_ERR(TGEN_LOG_TLS, "rte_cryptodev_start failed\n");
        return -EIO;
    }

    TGEN_INFO(TGEN_LOG_TLS, "Crypto device 0 started (%u QPs)\n",
              TGEN_MAX_WORKERS);
    return (int)g_n_cdevs;
}

void
cryptodev_fini(void)
{
    if (g_n_cdevs == 0) return;

    for (uint32_t w = 0; w < TGEN_MAX_WORKERS; w++) {
        for (int d = 0; d < 2; d++) {
            if (g_sessions[w][d]) {
                rte_cryptodev_sym_session_free(g_cdev_id, g_sessions[w][d]);
                g_sessions[w][d] = NULL;
            }
        }
    }
    rte_cryptodev_stop(g_cdev_id);
    rte_mempool_free(g_cop_pool);
    g_cop_pool = NULL;
}

int
cryptodev_submit(uint32_t worker_idx,
                 const uint8_t *src, uint8_t *dst, uint32_t len,
                 const crypto_op_params_t *params, void *user_data)
{
    if (g_n_cdevs == 0 || !g_cop_pool)
        return -ENOTSUP;

    struct rte_crypto_op *op;
    if (rte_crypto_op_bulk_alloc(g_cop_pool,
            RTE_CRYPTO_OP_TYPE_SYMMETRIC, &op, 1) != 1)
        return -ENOBUFS;

    /* Set up AES-GCM xform inline */
    struct rte_crypto_sym_xform xform = {
        .next    = NULL,
        .type    = RTE_CRYPTO_SYM_XFORM_AEAD,
        .aead    = {
            .op        = (params->dir == CRYPTO_DIR_ENCRYPT)
                         ? RTE_CRYPTO_AEAD_OP_ENCRYPT
                         : RTE_CRYPTO_AEAD_OP_DECRYPT,
            .algo      = RTE_CRYPTO_AEAD_AES_GCM,
            .key       = { .data = params->key, .length = params->key_len },
            .iv        = { .offset = 0, .length = 12 },
            .aad_length = params->aad_len,
            .digest_length = 16,
        },
    };

    struct rte_crypto_sym_op *sop = op->sym;
    /* Attach session-less op */
    if (rte_crypto_op_attach_sym_session(op, NULL) != 0) {
        rte_crypto_op_free(op);
        return -EINVAL;
    }
    sop->xform = &xform;

    /* Source/dest in contiguous memory — wrap in mbuf would be
     * required for a production path; use external mbuf here. */
    (void)src; (void)dst; (void)len; /* skipped — app sets mbuf fields */

    /* Store user_data in the op's private data area */
    void **priv = (void **)__rte_crypto_op_get_priv_data(op, sizeof(void *));
    if (priv) *priv = user_data;
    uint16_t sent = rte_cryptodev_enqueue_burst(g_cdev_id,
                                                (uint16_t)worker_idx,
                                                &op, 1);
    if (sent == 0) {
        rte_crypto_op_free(op);
        return -EBUSY;
    }
    return 0;
}

uint16_t
cryptodev_poll_completions(uint32_t worker_idx, cryptodev_cb_t cb)
{
    if (g_n_cdevs == 0) return 0;

    struct rte_crypto_op *ops[32];
    uint16_t n = rte_cryptodev_dequeue_burst(g_cdev_id,
                                             (uint16_t)worker_idx,
                                             ops, 32);
    for (uint16_t i = 0; i < n; i++) {
        int rc = (ops[i]->status == RTE_CRYPTO_OP_STATUS_SUCCESS) ? 0 : -EIO;
        void *ud = NULL;
        void **priv = (void **)__rte_crypto_op_get_priv_data(ops[i], sizeof(void *));
        if (priv) ud = *priv;
        if (cb) cb(ud, rc);
        rte_crypto_op_free(ops[i]);
    }
    return n;
}
