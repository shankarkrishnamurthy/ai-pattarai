/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: core-assignment engine — maps lcores to roles per §1.3.
 */
#include "core_assign.h"

#include <stdio.h>
#include <string.h>

#include <rte_lcore.h>
#include <rte_ethdev.h>
#include <rte_log.h>

#include "../common/types.h"

/* ── Global state ────────────────────────────────────────────────────────── */
core_map_t g_core_map;

/* ── Auto-scaling tier table (§1.3) ─────────────────────────────────────── */
typedef struct {
    uint32_t lcore_lo;
    uint32_t lcore_hi;
    uint32_t num_mgmt;
    /* Role assignments for each mgmt slot (up to 4) */
    lcore_role_t mgmt_roles[TGEN_MAX_MGMT_CORES];
} tier_entry_t;

static const tier_entry_t g_tiers[] = {
    {   2,   4, 1, { LCORE_ROLE_PRIMARY_MGMT, 0, 0, 0 } },
    {   5,  16, 1, { LCORE_ROLE_PRIMARY_MGMT, 0, 0, 0 } },
    {  17,  32, 2, { LCORE_ROLE_PRIMARY_MGMT, LCORE_ROLE_TELEMETRY, 0, 0 } },
    {  33,  64, 2, { LCORE_ROLE_PRIMARY_MGMT, LCORE_ROLE_TELEMETRY, 0, 0 } },
    {  65, 128, 3, { LCORE_ROLE_PRIMARY_MGMT, LCORE_ROLE_TELEMETRY, LCORE_ROLE_CLI_API, 0 } },
    { 129, UINT32_MAX, 4,
                   { LCORE_ROLE_PRIMARY_MGMT, LCORE_ROLE_TELEMETRY, LCORE_ROLE_CLI_API, LCORE_ROLE_WATCHDOG } },
};

static const tier_entry_t *select_tier(uint32_t n_lcores)
{
    for (size_t i = 0; i < TGEN_ARRAY_SIZE(g_tiers); i++) {
        if (n_lcores >= g_tiers[i].lcore_lo &&
            n_lcores <= g_tiers[i].lcore_hi)
            return &g_tiers[i];
    }
    return &g_tiers[TGEN_ARRAY_SIZE(g_tiers) - 1]; /* fallback: largest tier */
}

/* ── Memoise socket id for every lcore ────────────────────────────────────── */
static void populate_socket_map(void)
{
    uint32_t lcore_id;
    RTE_LCORE_FOREACH(lcore_id) {
        g_core_map.socket_of_lcore[lcore_id] =
            rte_lcore_to_socket_id(lcore_id);
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */
int tgen_core_assign_init(uint32_t num_worker_hint,
                          uint32_t num_mgmt_hint,
                          bool     manual_mode,
                          uint32_t num_ports)
{
    memset(&g_core_map, 0, sizeof(g_core_map));

    uint32_t n_lcores = rte_lcore_count();
    if (n_lcores < 2) {
        RTE_LOG(ERR, TGEN,
            "core_assign: need at least 2 lcores, have %u\n", n_lcores);
        return -1;
    }

    populate_socket_map();

    const tier_entry_t *tier = select_tier(n_lcores);
    uint32_t n_mgmt = manual_mode ? num_mgmt_hint : tier->num_mgmt;
    if (n_mgmt > TGEN_MAX_MGMT_CORES) n_mgmt = TGEN_MAX_MGMT_CORES;
    if (n_mgmt < 1) n_mgmt = 1;

    uint32_t n_workers = manual_mode ? num_worker_hint : (n_lcores - n_mgmt);
    if (n_workers == 0) {
        RTE_LOG(ERR, TGEN,
            "core_assign: no worker lcores available (n_lcores=%u n_mgmt=%u)\n",
            n_lcores, n_mgmt);
        return -1;
    }

    /* Assign lcores: management first (preferring socket 0), then workers */
    uint32_t lcore_id;
    uint32_t mgmt_assigned = 0;
    uint32_t worker_assigned = 0;

    /* Pass 1: assign management cores, prefer socket 0 */
    RTE_LCORE_FOREACH(lcore_id) {
        if (mgmt_assigned >= n_mgmt) break;
        if (g_core_map.socket_of_lcore[lcore_id] != 0) continue;
        g_core_map.mgmt_lcores[mgmt_assigned] = lcore_id;
        g_core_map.role[lcore_id] = tier->mgmt_roles[mgmt_assigned];
        mgmt_assigned++;
    }
    /* Pass 1b: fill remaining mgmt from any socket */
    if (mgmt_assigned < n_mgmt) {
        RTE_LCORE_FOREACH(lcore_id) {
            if (mgmt_assigned >= n_mgmt) break;
            if (g_core_map.role[lcore_id] != LCORE_ROLE_IDLE &&
                g_core_map.role[lcore_id] != 0)
                continue; /* already assigned */
            g_core_map.mgmt_lcores[mgmt_assigned] = lcore_id;
            g_core_map.role[lcore_id] = tier->mgmt_roles[mgmt_assigned];
            mgmt_assigned++;
        }
    }

    /* Pass 2: assign worker cores */
    RTE_LCORE_FOREACH(lcore_id) {
        if (worker_assigned >= n_workers) break;
        if (g_core_map.role[lcore_id] != LCORE_ROLE_IDLE &&
            g_core_map.role[lcore_id] != 0) continue;
        g_core_map.worker_lcores[worker_assigned] = lcore_id;
        g_core_map.role[lcore_id] = LCORE_ROLE_WORKER;
        worker_assigned++;
    }

    g_core_map.num_mgmt    = mgmt_assigned;
    g_core_map.num_workers = worker_assigned;

    /* Distribute worker lcores to ports, by NUMA socket */
    if (num_ports > 0) {
        /* Query each port's socket */
        for (uint32_t p = 0; p < num_ports && p < TGEN_MAX_PORTS; p++) {
            g_core_map.port_socket[p] =
                rte_eth_dev_socket_id((uint16_t)p);
        }

        /* Round-robin assign workers to ports on same socket */
        for (uint32_t w = 0; w < worker_assigned; w++) {
            uint32_t wlcore  = g_core_map.worker_lcores[w];
            uint32_t wsocket = g_core_map.socket_of_lcore[wlcore];
            for (uint32_t p = 0; p < num_ports && p < TGEN_MAX_PORTS; p++) {
                if (g_core_map.port_socket[p] == wsocket ||
                    wsocket == (uint32_t)SOCKET_ID_ANY) {
                    uint32_t idx = g_core_map.port_num_workers[p]++;
                    if (idx < TGEN_MAX_WORKERS)
                        g_core_map.port_workers[p][idx] = wlcore;
                }
            }
        }
    }

    tgen_core_assign_dump();
    return 0;
}

void tgen_core_assign_dump(void)
{
    RTE_LOG(INFO, TGEN,
        "Core assignment: %u worker(s), %u management core(s)\n",
        g_core_map.num_workers, g_core_map.num_mgmt);

    for (uint32_t i = 0; i < g_core_map.num_workers; i++) {
        uint32_t lc = g_core_map.worker_lcores[i];
        RTE_LOG(INFO, TGEN,
            "  Worker[%u] lcore=%u socket=%u\n",
            i, lc, g_core_map.socket_of_lcore[lc]);
    }
    for (uint32_t i = 0; i < g_core_map.num_mgmt; i++) {
        uint32_t lc = g_core_map.mgmt_lcores[i];
        const char *rname = "unknown";
        switch (g_core_map.role[lc]) {
        case LCORE_ROLE_PRIMARY_MGMT: rname = "primary-mgmt"; break;
        case LCORE_ROLE_TELEMETRY:    rname = "telemetry"; break;
        case LCORE_ROLE_CLI_API:      rname = "cli-api"; break;
        case LCORE_ROLE_WATCHDOG:     rname = "watchdog"; break;
        default: break;
        }
        RTE_LOG(INFO, TGEN,
            "  Mgmt[%u] lcore=%u socket=%u role=%s\n",
            i, lc, g_core_map.socket_of_lcore[lc], rname);
    }
}
