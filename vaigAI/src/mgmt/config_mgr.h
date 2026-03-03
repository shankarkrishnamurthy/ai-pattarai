/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Runtime configuration state.
 *
 * Minimal runtime state populated from binary command-line arguments.
 * Workers receive updates via the IPC ring (core/ipc.h).
 */
#ifndef VAIGAI_CONFIG_MGR_H
#define VAIGAI_CONFIG_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/types.h"
#include "../net/ipv4.h"
#include "../tls/cert_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Global configuration (populated from binary args at startup)         */
/* ------------------------------------------------------------------ */
typedef struct {
    /* TLS */
    cert_cfg_t  cert;
    bool        tls_enabled;

    /* Mgmt */
    uint16_t    rest_port;      /**< 0 = disabled */

    /* TCB sizing */
    uint32_t    max_concurrent; /**< max connections per worker */
} tgen_config_t;

extern tgen_config_t g_config;

#ifdef __cplusplus
}
#endif
#endif /* VAIGAI_CONFIG_MGR_H */
