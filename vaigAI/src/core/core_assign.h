/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: core assignment engine — maps lcores to roles.
 */
#ifndef TGEN_CORE_ASSIGN_H
#define TGEN_CORE_ASSIGN_H

#include <stdint.h>
#include <stdbool.h>

#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Core map ─────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t      worker_lcores[TGEN_MAX_WORKERS];
    uint32_t      mgmt_lcores[TGEN_MAX_MGMT_CORES];
    lcore_role_t  role[TGEN_MAX_LCORES];    /* indexed by lcore_id */
    uint32_t      num_workers;
    uint32_t      num_mgmt;
    uint32_t      socket_of_lcore[TGEN_MAX_LCORES];
    /* NUMA: which socket does each physical port live on? */
    uint32_t      port_socket[TGEN_MAX_PORTS];
    /* Which worker lcores service each port? */
    uint32_t      port_workers[TGEN_MAX_PORTS][TGEN_MAX_WORKERS];
    uint32_t      port_num_workers[TGEN_MAX_PORTS];
} core_map_t;

/** The global core map — populated by tgen_core_assign_init(). */
extern core_map_t g_core_map;

/** Build the core map, honouring the policy supplied in eal_args.
 *  Must be called after rte_eal_init().
 *  Returns 0 on success, -1 on error. */
int tgen_core_assign_init(uint32_t num_worker_hint,
                          uint32_t num_mgmt_hint,
                          bool     manual_mode,
                          uint32_t num_ports);

/** Dump the core map to the log at INFO level. */
void tgen_core_assign_dump(void);

/** Return true if lcore_id is a worker. */
static inline bool tgen_is_worker(uint32_t lcore_id)
{
    if (lcore_id >= TGEN_MAX_LCORES) return false;
    return g_core_map.role[lcore_id] == LCORE_ROLE_WORKER;
}

/** Return true if lcore_id is a management core. */
static inline bool tgen_is_mgmt(uint32_t lcore_id)
{
    if (lcore_id >= TGEN_MAX_LCORES) return false;
    lcore_role_t r = g_core_map.role[lcore_id];
    return r == LCORE_ROLE_PRIMARY_MGMT ||
           r == LCORE_ROLE_TELEMETRY    ||
           r == LCORE_ROLE_CLI_API      ||
           r == LCORE_ROLE_WATCHDOG;
}

#ifdef __cplusplus
}
#endif
#endif /* TGEN_CORE_ASSIGN_H */
