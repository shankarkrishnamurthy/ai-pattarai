/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: LPM routing table (§2.4).
 */
#include "lpm.h"
#include "../common/util.h"

#include <string.h>
#include <netinet/in.h>

#include <rte_lpm.h>
#include <rte_rwlock.h>
#include <rte_log.h>

/* ── State ───────────────────────────────────────────────────────────────── */
static struct rte_lpm *g_lpm;

/* next_hop table: lpm stores a uint32_t nexthop index;
 * we store the full route info in a side table. */
static lpm_route_t     g_routes[LPM_MAX_ROUTES];
static uint32_t        g_n_routes;
static rte_rwlock_t    g_lock;

/* ── Init ─────────────────────────────────────────────────────────────────── */
int lpm_init(void)
{
    rte_rwlock_init(&g_lock);
    memset(g_routes, 0, sizeof(g_routes));
    g_n_routes = 0;

    struct rte_lpm_config cfg = {
        .max_rules     = LPM_MAX_ROUTES,
        .number_tbl8s  = 256,
        .flags         = 0,
    };
    g_lpm = rte_lpm_create("tgen_lpm", SOCKET_ID_ANY, &cfg);
    if (!g_lpm) {
        RTE_LOG(ERR, NET, "LPM: rte_lpm_create failed\n");
        return -1;
    }
    RTE_LOG(INFO, NET, "LPM: created (max %u routes)\n", LPM_MAX_ROUTES);
    return 0;
}

/* ── Add route ───────────────────────────────────────────────────────────── */
int lpm_add(const lpm_route_t *route)
{
    if (!g_lpm || !route) return -1;
    rte_rwlock_write_lock(&g_lock);
    if (g_n_routes >= LPM_MAX_ROUTES) {
        RTE_LOG(ERR, NET, "LPM: route table full\n");
        rte_rwlock_write_unlock(&g_lock);
        return -1;
    }

    uint32_t idx = g_n_routes;
    g_routes[idx] = *route;

    /* rte_lpm uses host byte order address, so convert */
    uint32_t prefix_host = ntohl(route->prefix);
    int rc = rte_lpm_add(g_lpm, prefix_host, route->prefix_len, idx);
    if (rc < 0) {
        RTE_LOG(ERR, NET, "LPM: rte_lpm_add failed: %d\n", rc);
        rte_rwlock_write_unlock(&g_lock);
        return -1;
    }
    g_n_routes++;
    rte_rwlock_write_unlock(&g_lock);
    return 0;
}

/* ── Delete route ─────────────────────────────────────────────────────────── */
int lpm_del(uint32_t prefix_net, uint8_t prefix_len)
{
    if (!g_lpm) return -1;
    rte_rwlock_write_lock(&g_lock);
    uint32_t prefix_host = ntohl(prefix_net);
    int rc = rte_lpm_delete(g_lpm, prefix_host, prefix_len);
    rte_rwlock_write_unlock(&g_lock);
    return rc;
}

/* ── Lookup ───────────────────────────────────────────────────────────────── */
int lpm_lookup(uint32_t dst_ip_net,
               uint32_t *next_hop_ip_out,
               uint16_t *egress_port_out)
{
    if (!g_lpm) return -1;
    rte_rwlock_read_lock(&g_lock);
    uint32_t dst_host = ntohl(dst_ip_net);
    uint32_t idx      = 0;
    int rc = rte_lpm_lookup(g_lpm, dst_host, &idx);
    if (rc == 0 && idx < g_n_routes) {
        if (next_hop_ip_out)  *next_hop_ip_out  = g_routes[idx].next_hop_ip;
        if (egress_port_out)  *egress_port_out  = g_routes[idx].egress_port;
    }
    rte_rwlock_read_unlock(&g_lock);
    return rc;
}

/* ── Destroy ──────────────────────────────────────────────────────────────── */
void lpm_destroy(void)
{
    if (g_lpm) {
        rte_lpm_free(g_lpm);
        g_lpm = NULL;
    }
}
