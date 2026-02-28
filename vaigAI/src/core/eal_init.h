/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: EAL initialisation & core-assignment engine interfaces.
 */
#ifndef TGEN_EAL_INIT_H
#define TGEN_EAL_INIT_H

#include <stdint.h>
#include <stdbool.h>

#include "core_assign.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Startup arguments (parsed before rte_eal_init) ──────────────────────── */
typedef struct {
    uint32_t    num_worker_cores;   /* 0 = derive from auto policy */
    uint32_t    num_mgmt_cores;     /* 0 = derive from auto policy */
    const char *core_assignment_policy; /* "auto" | "manual" */
    const char *lcores_map;         /* manual mode: EAL --lcores string */
    uint32_t    main_lcore;         /* EAL main lcore (default 0) */
    uint32_t    num_rx_desc;        /* descriptors per RX queue */
    uint32_t    num_tx_desc;        /* descriptors per TX queue */
    uint32_t    pipeline_depth;     /* for mempool sizing */
    uint32_t    max_chain_depth;    /* mbuf chain depth (default 4) */
    /* Optional vdev strings supplied via --vdev on the command line.
     * These are passed directly to rte_eal_init() as extra EAL args. */
    const char *extra_eal_args[16];
    uint32_t    num_extra_eal_args;
} tgen_eal_args_t;

/** Parse argv, populate tgen_eal_args_t, then call rte_eal_init().
 *  Returns the number of arguments consumed, or -1 on error. */
int tgen_eal_init(int argc, char **argv, tgen_eal_args_t *out_args);

/** Tear down EAL. */
void tgen_eal_cleanup(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_EAL_INIT_H */
