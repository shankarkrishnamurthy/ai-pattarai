/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: ARP implementation (§2.2, RFC 826).
 */
#include "arp.h"
#include "ethernet.h"
#include "../core/core_assign.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"

#include <string.h>
#include <stdio.h>

#include <rte_arp.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_ring.h>

/* per-port ring from worker → management for ARP frames */
static struct rte_ring *g_arp_rings[TGEN_MAX_PORTS];

arp_state_t_port g_arp[TGEN_MAX_PORTS];

/* ── Initialise ──────────────────────────────────────────────────────────── */
int arp_init(void)
{
    memset(g_arp, 0, sizeof(g_arp));
    uint32_t n_ports = rte_eth_dev_count_avail();

    for (uint32_t p = 0; p < n_ports && p < TGEN_MAX_PORTS; p++) {
        arp_state_t_port *a = &g_arp[p];
        a->port_id = (uint16_t)p;
        rte_rwlock_init(&a->lock);

        struct rte_hash_parameters hp = {
            .name       = NULL,
            .entries    = TGEN_ARP_CACHE_SZ,
            .key_len    = sizeof(uint32_t),
            .hash_func  = rte_jhash,
            .socket_id  = (int)rte_eth_dev_socket_id((uint16_t)p),
        };
        char hname[32];
        snprintf(hname, sizeof(hname), "arp_cache_%u", p);
        hp.name = hname;

        a->table = rte_hash_create(&hp);
        if (!a->table) {
            RTE_LOG(ERR, NET, "ARP: failed to create hash for port %u\n", p);
            return -1;
        }

        /* init MAC from port */
        rte_eth_macaddr_get((uint16_t)p, &a->local_mac);

        /* ring: workers forward ARP frames here */
        char rname[32];
        snprintf(rname, sizeof(rname), "arp_ring_%u", p);
        g_arp_rings[p] = rte_ring_create(rname, 512,
                             (int)rte_eth_dev_socket_id((uint16_t)p),
                             RING_F_SC_DEQ);
        if (!g_arp_rings[p]) {
            RTE_LOG(ERR, NET, "ARP: ring create failed for port %u\n", p);
            return -1;
        }
    }
    return 0;
}

/* ── Worker path: enqueue frames to mgmt ────────────────────────────────── */
void arp_input(uint32_t worker_idx, struct rte_mbuf *m)
{
    (void)worker_idx;
    /* Determine which port this came from */
    uint16_t port_id = m->port;
    if (port_id >= TGEN_MAX_PORTS || !g_arp_rings[port_id]) {
        rte_pktmbuf_free(m);
        return;
    }
    if (rte_ring_enqueue(g_arp_rings[port_id], m) != 0)
        rte_pktmbuf_free(m);
}

/* ── Build ARP request packet ────────────────────────────────────────────── */
static struct rte_mbuf *build_arp_request(uint16_t port_id,
                                            uint32_t target_ip,
                                            struct rte_mempool *mp)
{
    arp_state_t_port *a = &g_arp[port_id];
    struct rte_mbuf *m  = rte_pktmbuf_alloc(mp);
    if (!m) return NULL;

    size_t pkt_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    char *data = rte_pktmbuf_append(m, (uint16_t)pkt_len);
    if (!data) { rte_pktmbuf_free(m); return NULL; }

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    memset(eth->dst_addr.addr_bytes, 0xFF, 6); /* broadcast */
    rte_ether_addr_copy(&a->local_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    struct rte_arp_hdr *arp =
        (struct rte_arp_hdr *)(data + sizeof(struct rte_ether_hdr));
    arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp->arp_hlen     = 6;
    arp->arp_plen     = 4;
    arp->arp_opcode   = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);
    rte_ether_addr_copy(&a->local_mac, &arp->arp_data.arp_sha);
    arp->arp_data.arp_sip = a->local_ip;
    memset(arp->arp_data.arp_tha.addr_bytes, 0, 6);
    arp->arp_data.arp_tip = target_ip;
    return m;
}

/* ── Build ARP reply ─────────────────────────────────────────────────────── */
static struct rte_mbuf *build_arp_reply(uint16_t port_id,
                                         const struct rte_arp_hdr *req,
                                         struct rte_mempool *mp)
{
    arp_state_t_port *a = &g_arp[port_id];
    struct rte_mbuf *m  = rte_pktmbuf_alloc(mp);
    if (!m) return NULL;

    size_t pkt_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    char *data = rte_pktmbuf_append(m, (uint16_t)pkt_len);
    if (!data) { rte_pktmbuf_free(m); return NULL; }

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    rte_ether_addr_copy(&req->arp_data.arp_sha, &eth->dst_addr);
    rte_ether_addr_copy(&a->local_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    struct rte_arp_hdr *reply =
        (struct rte_arp_hdr *)(data + sizeof(struct rte_ether_hdr));
    reply->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    reply->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    reply->arp_hlen     = 6;
    reply->arp_plen     = 4;
    reply->arp_opcode   = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
    rte_ether_addr_copy(&a->local_mac, &reply->arp_data.arp_sha);
    reply->arp_data.arp_sip = a->local_ip;
    rte_ether_addr_copy(&req->arp_data.arp_sha, &reply->arp_data.arp_tha);
    reply->arp_data.arp_tip = req->arp_data.arp_sip;

    return m;
}

/* ── Mgmt: process one ARP frame ────────────────────────────────────────── */
void arp_mgmt_process(uint16_t port_id, struct rte_mbuf *m)
{
    arp_state_t_port *a = &g_arp[port_id];

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_arp_hdr   *arp =
        (struct rte_arp_hdr *)((uint8_t *)eth + sizeof(*eth));

    uint16_t op = rte_be_to_cpu_16(arp->arp_opcode);
    uint32_t sender_ip = arp->arp_data.arp_sip;

    if (op == RTE_ARP_OP_REQUEST &&
        arp->arp_data.arp_tip == a->local_ip) {
        /* Reply with our MAC */
        extern struct rte_mempool *g_worker_mempools[];
        struct rte_mempool *mp = g_worker_mempools[0];
        struct rte_mbuf *reply = build_arp_reply(port_id, arp, mp);
        if (reply) {
            rte_eth_tx_burst(port_id, 0, &reply, 1);
            worker_metrics_add_arp_reply_tx(0);
        }
    } else if (op == RTE_ARP_OP_REQUEST) {
        /* ARP request not for us — ignored */
    } else if (op == RTE_ARP_OP_REPLY) {
        /* Update cache — entry was pre-inserted as PENDING by arp_request() */
        rte_rwlock_write_lock(&a->lock);
        int rc = rte_hash_lookup(a->table, &sender_ip);
        if (rc >= 0) {
            arp_entry_t *e = &a->entries[rc];
            rte_ether_addr_copy(&arp->arp_data.arp_sha, &e->mac);
            e->state      = ARP_STATE_RESOLVED;
            e->expire_tsc = rte_rdtsc() +
                            g_tsc_hz * ARP_CACHE_TTL_S;
            e->fail_count = 0;
            /* Flush hold queue */
            for (uint32_t i = 0; i < e->hold_count; i++) {
                rte_eth_tx_burst(port_id, 0, &e->hold[i], 1);
            }
            e->hold_count = 0;
        }
        rte_rwlock_write_unlock(&a->lock);
    }
    rte_pktmbuf_free(m);
}

/* ── Mgmt: periodic tick ─────────────────────────────────────────────────── */
void arp_mgmt_tick(void)
{
    uint64_t now = rte_rdtsc();
    uint32_t n_ports = rte_eth_dev_count_avail();

    for (uint32_t p = 0; p < n_ports && p < TGEN_MAX_PORTS; p++) {
        /* Drain ARP ring from workers */
        struct rte_mbuf *m;
        while (rte_ring_dequeue(g_arp_rings[p], (void **)&m) == 0)
            arp_mgmt_process((uint16_t)p, m);

        /* Probe stale entries */
        arp_state_t_port *a = &g_arp[p];
        rte_rwlock_write_lock(&a->lock);
        for (uint32_t i = 0; i < TGEN_ARP_CACHE_SZ; i++) {
            arp_entry_t *e = &a->entries[i];
            if (e->state != ARP_STATE_RESOLVED) continue;
            uint64_t probe_tsc = e->expire_tsc -
                                  g_tsc_hz * ARP_PROBE_BEFORE_EXPIRY;
            if (now >= probe_tsc && now < e->expire_tsc)
                e->state = ARP_STATE_STALE; /* trigger re-probe next tick */
            else if (now >= e->expire_tsc) {
                e->fail_count++;
                if (e->fail_count >= ARP_MAX_PROBE_FAILURES)
                    e->state = ARP_STATE_FAILED;
            }
        }
        rte_rwlock_write_unlock(&a->lock);
    }
}

/* ── ARP lookup (worker-safe, read-only) ─────────────────────────────────── */
bool arp_lookup(uint16_t port_id, uint32_t ip_net,
                struct rte_ether_addr *mac_out)
{
    if (port_id >= TGEN_MAX_PORTS) return false;
    arp_state_t_port *a = &g_arp[port_id];
    rte_rwlock_read_lock(&a->lock);
    void *hd = NULL;
    int idx = rte_hash_lookup_data(a->table, &ip_net, &hd);
    bool found = false;
    if (idx >= 0 && a->entries[idx].state == ARP_STATE_RESOLVED) {
        rte_ether_addr_copy(&a->entries[idx].mac, mac_out);
        found = true;
    }
    rte_rwlock_read_unlock(&a->lock);
    return found;
}

int arp_request(uint16_t port_id, uint32_t ip_net)
{
    arp_state_t_port *a = &g_arp[port_id];

    /* Pre-insert a PENDING entry so the reply handler can store the MAC */
    rte_rwlock_write_lock(&a->lock);
    int idx = rte_hash_add_key(a->table, &ip_net);
    if (idx >= 0) {
        arp_entry_t *e = &a->entries[idx];
        if (e->state != ARP_STATE_RESOLVED) {
            e->state      = ARP_STATE_PENDING;
            e->hold_count = 0;
            e->fail_count = 0;
        }
    }
    rte_rwlock_write_unlock(&a->lock);

    /* Uses mempool[0] for mgmt-initiated ARP probes */
    extern struct rte_mempool *g_worker_mempools[];
    struct rte_mempool *mp = g_worker_mempools[0];
    struct rte_mbuf *m = build_arp_request(port_id, ip_net, mp);
    if (!m) return -1;
    int nb = rte_eth_tx_burst(port_id, 0, &m, 1);
    if (!nb) rte_pktmbuf_free(m);
    else     worker_metrics_add_arp_request_tx(0);
    return 0;
}

void arp_destroy(void)
{
    for (uint32_t p = 0; p < TGEN_MAX_PORTS; p++) {
        if (g_arp[p].table) {
            rte_hash_free(g_arp[p].table);
            g_arp[p].table = NULL;
        }
        if (g_arp_rings[p]) {
            rte_ring_free(g_arp_rings[p]);
            g_arp_rings[p] = NULL;
        }
    }
}
