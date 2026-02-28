/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Configuration manager (§5.2).
 *
 * Single source of truth for all runtime parameters.
 * Workers receive updates via the IPC ring (core/ipc.h).
 * Supports JSON file load, live CLI overrides, and REST PATCH.
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
/* Flow descriptor                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t src_ip_lo;     /**< Source IP range start (host BO) */
    uint32_t src_ip_hi;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t vlan_id;       /**< 0 = no VLAN */
    uint8_t  dscp;
    uint8_t  ttl;
    bool     enable_tls;
    char     sni[256];
    char     http_url[1024];
    char     http_host[256];
    uint32_t http_body_len; /**< 0 = GET, >0 = POST with synthetic body */
    bool     icmp_ping;     /**< true = ICMP echo mode; dst_port not required */
} flow_cfg_t;

/* ------------------------------------------------------------------ */
/* Load shape                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    load_mode_t mode;
    uint64_t    target_cps;     /**< connections per second (FIXED mode) */
    uint64_t    target_rps;     /**< requests per second (FIXED mode)    */
    uint64_t    ramp_up_secs;   /**< RAMP mode: seconds to reach target  */
    uint64_t    ramp_down_secs;
    uint64_t    duration_secs;  /**< 0 = run forever                     */
    uint32_t    max_concurrent; /**< concurrent connections cap           */
} load_cfg_t;

/* ------------------------------------------------------------------ */
/* Global configuration                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Network */
    flow_cfg_t  flows[TGEN_MAX_PORTS];
    uint32_t    n_flows;

    /* Load */
    load_cfg_t  load;

    /* TLS */
    cert_cfg_t  cert;
    bool        tls_enabled;

    /* Mgmt */
    uint16_t    rest_port;   /**< 0 = disabled */
    char        cli_prompt[32];
} tgen_config_t;

extern tgen_config_t g_config;

/** Load configuration from a JSON file. */
int config_load_json(const char *path);

/** Save current configuration to a JSON file. */
int config_save_json(const char *path);

/** Apply a JSON patch string (e.g. from REST or CLI). */
int config_apply_patch(const char *json_patch);

/** Broadcast updated flow config to all workers via IPC. */
int config_push_to_workers(void);

/** Validate configuration for consistency. Returns 0 or error code. */
int config_validate(const tgen_config_t *cfg);

#ifdef __cplusplus
}
#endif
#endif /* VAIGAI_CONFIG_MGR_H */
