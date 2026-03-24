/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: NDP implementation (RFC 4861).
 */
#include "ndp.h"
#include "icmpv6.h"
#include "../core/mempool.h"
#include "../port/port_init.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"

#include <string.h>

#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_log.h>
#include <rte_byteorder.h>
#include <rte_ether.h>

/* Per-port ring from worker -> management for NDP frames */
static struct rte_ring *g_ndp_rings[TGEN_MAX_PORTS];

ndp_state_t_port g_ndp[TGEN_MAX_PORTS];

/* ── Helper: generate link-local address from MAC (EUI-64) ───────────────── */
static void make_link_local(const struct rte_ether_addr *mac, uint8_t *ll)
{
    memset(ll, 0, 16);
    ll[0] = 0xfe;
    ll[1] = 0x80;
    /* EUI-64: insert ff:fe in middle, flip U/L bit */
    ll[8]  = mac->addr_bytes[0] ^ 0x02;
    ll[9]  = mac->addr_bytes[1];
    ll[10] = mac->addr_bytes[2];
    ll[11] = 0xff;
    ll[12] = 0xfe;
    ll[13] = mac->addr_bytes[3];
    ll[14] = mac->addr_bytes[4];
    ll[15] = mac->addr_bytes[5];
}

/* ── Initialise ──────────────────────────────────────────────────────────── */
int ndp_init(void)
{
    memset(g_ndp, 0, sizeof(g_ndp));
    uint32_t n_ports = rte_eth_dev_count_avail();

    for (uint32_t p = 0; p < n_ports && p < TGEN_MAX_PORTS; p++) {
        ndp_state_t_port *s = &g_ndp[p];
        s->port_id = (uint16_t)p;
        rte_rwlock_init(&s->lock);
        rte_eth_macaddr_get((uint16_t)p, &s->local_mac);

        make_link_local(&s->local_mac, s->local_ip6_ll);

        char rname[32];
        snprintf(rname, sizeof(rname), "ndp_ring_%u", p);
        g_ndp_rings[p] = rte_ring_create(rname, 512,
                             (int)rte_eth_dev_socket_id((uint16_t)p),
                             RING_F_SC_DEQ);
        if (!g_ndp_rings[p]) {
            RTE_LOG(ERR, NET, "NDP: ring create failed for port %u\n", p);
            return -1;
        }
    }
    return 0;
}

/* ── Helper: compute solicited-node multicast address ────────────────────── */
void ndp_solicited_node_mcast(const uint8_t *ip6, uint8_t *out)
{
    /* ff02::1:ffXX:XXXX — low 24 bits of the unicast address */
    memset(out, 0, 16);
    out[0] = 0xff;
    out[1] = 0x02;
    out[11] = 0x01;
    out[12] = 0xff;
    out[13] = ip6[13];
    out[14] = ip6[14];
    out[15] = ip6[15];
}

/* ── Helper: compute multicast MAC ───────────────────────────────────────── */
void ndp_mcast_mac(const uint8_t *mcast_ip6, struct rte_ether_addr *mac_out)
{
    mac_out->addr_bytes[0] = 0x33;
    mac_out->addr_bytes[1] = 0x33;
    mac_out->addr_bytes[2] = mcast_ip6[12];
    mac_out->addr_bytes[3] = mcast_ip6[13];
    mac_out->addr_bytes[4] = mcast_ip6[14];
    mac_out->addr_bytes[5] = mcast_ip6[15];
}

/* ── Worker: forward to mgmt ring ────────────────────────────────────────── */
void ndp_input(uint32_t worker_idx, struct rte_mbuf *m)
{
    (void)worker_idx;
    uint16_t port_id = m->port;
    if (port_id >= TGEN_MAX_PORTS || !g_ndp_rings[port_id]) {
        rte_pktmbuf_free(m);
        return;
    }
    if (rte_ring_enqueue(g_ndp_rings[port_id], m) != 0)
        rte_pktmbuf_free(m);
}

/* ── NDP lookup (read-only, worker-safe) ─────────────────────────────────── */
bool ndp_lookup(uint16_t port_id, const uint8_t *ip6,
                struct rte_ether_addr *mac_out)
{
    if (port_id >= TGEN_MAX_PORTS) return false;
    ndp_state_t_port *s = &g_ndp[port_id];

    rte_rwlock_read_lock(&s->lock);
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->entries[i].state == NDP_STATE_REACHABLE &&
            memcmp(s->entries[i].ip6, ip6, 16) == 0) {
            rte_ether_addr_copy(&s->entries[i].mac, mac_out);
            rte_rwlock_read_unlock(&s->lock);
            return true;
        }
    }
    rte_rwlock_read_unlock(&s->lock);
    return false;
}

/* ── NDP cache update (internal) ─────────────────────────────────────────── */
static void ndp_cache_update(uint16_t port_id, const uint8_t *ip6,
                              const struct rte_ether_addr *mac)
{
    ndp_state_t_port *s = &g_ndp[port_id];
    rte_rwlock_write_lock(&s->lock);

    /* Find existing entry */
    for (uint32_t i = 0; i < s->count; i++) {
        if (memcmp(s->entries[i].ip6, ip6, 16) == 0) {
            rte_ether_addr_copy(mac, &s->entries[i].mac);
            s->entries[i].state = NDP_STATE_REACHABLE;
            s->entries[i].expire_tsc = rte_rdtsc() +
                                       g_tsc_hz * NDP_CACHE_TTL_S;
            s->entries[i].fail_count = 0;
            rte_rwlock_write_unlock(&s->lock);
            return;
        }
    }

    /* Add new entry */
    if (s->count < TGEN_NDP_CACHE_SZ) {
        ndp_entry_t *e = &s->entries[s->count++];
        memcpy(e->ip6, ip6, 16);
        rte_ether_addr_copy(mac, &e->mac);
        e->state = NDP_STATE_REACHABLE;
        e->expire_tsc = rte_rdtsc() + g_tsc_hz * NDP_CACHE_TTL_S;
        e->fail_count = 0;
    }
    rte_rwlock_write_unlock(&s->lock);
}

/* ── Build and send Neighbor Solicitation ─────────────────────────────────── */
int ndp_solicit(uint16_t port_id, const uint8_t *target_ip6)
{
    ndp_state_t_port *s = &g_ndp[port_id];

    extern struct rte_mempool *g_worker_mempools[];
    struct rte_mempool *mp = g_worker_mempools[0];
    if (!mp) return -1;

    struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
    if (!m) return -1;

    /* NS packet: Eth + IPv6 + ICMPv6 NS (24 bytes) + source LLADDR option (8 bytes) */
    size_t icmpv6_len = 24 + 8; /* NS body + SLLAO */
    size_t total = sizeof(struct rte_ether_hdr) + 40 + icmpv6_len;

    char *buf = rte_pktmbuf_append(m, (uint16_t)total);
    if (!buf) { rte_pktmbuf_free(m); return -1; }

    /* Determine source address: prefer configured global, fall back to link-local */
    const uint8_t *src_ip6 = s->has_ip6 ? s->local_ip6 : s->local_ip6_ll;

    /* Compute solicited-node multicast address for target */
    uint8_t sol_node[16];
    ndp_solicited_node_mcast(target_ip6, sol_node);

    /* Ethernet header */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
    rte_ether_addr_copy(&s->local_mac, &eth->src_addr);
    ndp_mcast_mac(sol_node, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

    /* IPv6 header */
    struct rte_ipv6_hdr *ip6 =
        (struct rte_ipv6_hdr *)(buf + sizeof(struct rte_ether_hdr));
    ip6->vtc_flow = rte_cpu_to_be_32(0x60000000);
    ip6->payload_len = rte_cpu_to_be_16((uint16_t)icmpv6_len);
    ip6->proto = IPPROTO_ICMPV6;
    ip6->hop_limits = 255;
    memcpy(&ip6->src_addr, src_ip6, 16);
    memcpy(&ip6->dst_addr, sol_node, 16);

    /* ICMPv6 Neighbor Solicitation */
    uint8_t *icmp = (uint8_t *)ip6 + 40;
    icmp[0] = ICMPV6_NEIGHBOR_SOL; /* type */
    icmp[1] = 0;                   /* code */
    icmp[2] = 0; icmp[3] = 0;     /* checksum (filled below) */
    icmp[4] = 0; icmp[5] = 0; icmp[6] = 0; icmp[7] = 0; /* reserved */
    memcpy(icmp + 8, target_ip6, 16); /* target address */

    /* Source Link-Layer Address option */
    icmp[24] = NDP_OPT_SRC_LLADDR;
    icmp[25] = 1; /* length in units of 8 bytes */
    memcpy(icmp + 26, s->local_mac.addr_bytes, 6);

    /* Compute checksum */
    icmp[2] = 0; icmp[3] = 0;
    uint16_t cksum = icmpv6_checksum(src_ip6, sol_node, icmp, (uint16_t)icmpv6_len);
    icmp[2] = (uint8_t)(cksum & 0xff);
    icmp[3] = (uint8_t)(cksum >> 8);

    /* Pre-insert PENDING entry */
    rte_rwlock_write_lock(&s->lock);
    bool found = false;
    for (uint32_t i = 0; i < s->count; i++) {
        if (memcmp(s->entries[i].ip6, target_ip6, 16) == 0) {
            found = true;
            if (s->entries[i].state != NDP_STATE_REACHABLE) {
                s->entries[i].state = NDP_STATE_PENDING;
                s->entries[i].fail_count = 0;
            }
            break;
        }
    }
    if (!found && s->count < TGEN_NDP_CACHE_SZ) {
        ndp_entry_t *e = &s->entries[s->count++];
        memcpy(e->ip6, target_ip6, 16);
        memset(&e->mac, 0, sizeof(e->mac));
        e->state = NDP_STATE_PENDING;
        e->fail_count = 0;
    }
    rte_rwlock_write_unlock(&s->lock);

    uint16_t txq = g_port_caps[port_id].mgmt_tx_q;
    uint16_t nb = rte_eth_tx_burst(port_id, txq, &m, 1);
    if (!nb) { rte_pktmbuf_free(m); return -1; }
    return 0;
}

/* ── Process Neighbor Advertisement ──────────────────────────────────────── */
void ndp_process_na(uint16_t port_id, const uint8_t *icmpv6_body,
                     uint16_t body_len)
{
    if (body_len < 24) return; /* minimum NA size: 8 (ICMPv6) + 16 (target) */

    /* Target address is at offset 8 in the ICMPv6 body */
    const uint8_t *target = icmpv6_body + 8;

    /* Look for Target Link-Layer Address option */
    uint16_t opt_off = 24;
    while (opt_off + 8 <= body_len) {
        uint8_t opt_type = icmpv6_body[opt_off];
        uint8_t opt_len  = icmpv6_body[opt_off + 1]; /* in 8-byte units */
        if (opt_len == 0) break;
        if (opt_type == NDP_OPT_TGT_LLADDR && opt_len >= 1) {
            struct rte_ether_addr mac;
            memcpy(mac.addr_bytes, &icmpv6_body[opt_off + 2], 6);
            ndp_cache_update(port_id, target, &mac);
            return;
        }
        opt_off += (uint16_t)(opt_len * 8);
    }
}

/* ── Process Neighbor Solicitation ───────────────────────────────────────── */
void ndp_process_ns(uint16_t port_id, const uint8_t *icmpv6_body,
                     uint16_t body_len,
                     const uint8_t *sender_ip6)
{
    ndp_state_t_port *s = &g_ndp[port_id];
    if (body_len < 24) return;

    const uint8_t *target = icmpv6_body + 8;

    /* Check if this NS is for our address */
    bool for_us = false;
    if (s->has_ip6 && memcmp(target, s->local_ip6, 16) == 0)
        for_us = true;
    if (memcmp(target, s->local_ip6_ll, 16) == 0)
        for_us = true;
    if (!for_us) return;

    /* Learn sender's MAC from Source LLADDR option */
    uint16_t opt_off = 24;
    while (opt_off + 8 <= body_len) {
        uint8_t opt_type = icmpv6_body[opt_off];
        uint8_t opt_len  = icmpv6_body[opt_off + 1];
        if (opt_len == 0) break;
        if (opt_type == NDP_OPT_SRC_LLADDR && opt_len >= 1) {
            struct rte_ether_addr sender_mac;
            memcpy(sender_mac.addr_bytes, &icmpv6_body[opt_off + 2], 6);
            ndp_cache_update(port_id, sender_ip6, &sender_mac);
            break;
        }
        opt_off += (uint16_t)(opt_len * 8);
    }

    /* Send Neighbor Advertisement reply */
    extern struct rte_mempool *g_worker_mempools[];
    struct rte_mempool *mp = g_worker_mempools[0];
    if (!mp) return;

    struct rte_mbuf *reply = rte_pktmbuf_alloc(mp);
    if (!reply) return;

    size_t icmpv6_len = 24 + 8; /* NA body + TLLAO */
    size_t total = sizeof(struct rte_ether_hdr) + 40 + icmpv6_len;
    char *buf = rte_pktmbuf_append(reply, (uint16_t)total);
    if (!buf) { rte_pktmbuf_free(reply); return; }

    /* Ethernet */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
    rte_ether_addr_copy(&s->local_mac, &eth->src_addr);
    /* Unicast reply to sender */
    struct rte_ether_addr dst_mac;
    if (ndp_lookup(port_id, sender_ip6, &dst_mac))
        rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
    else
        memset(eth->dst_addr.addr_bytes, 0xff, 6); /* fallback broadcast */
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

    /* IPv6 */
    struct rte_ipv6_hdr *ip6 =
        (struct rte_ipv6_hdr *)(buf + sizeof(struct rte_ether_hdr));
    ip6->vtc_flow = rte_cpu_to_be_32(0x60000000);
    ip6->payload_len = rte_cpu_to_be_16((uint16_t)icmpv6_len);
    ip6->proto = IPPROTO_ICMPV6;
    ip6->hop_limits = 255;
    memcpy(&ip6->src_addr, target, 16); /* respond from the solicited address */
    memcpy(&ip6->dst_addr, sender_ip6, 16);

    /* ICMPv6 Neighbor Advertisement */
    uint8_t *icmp = (uint8_t *)ip6 + 40;
    icmp[0] = ICMPV6_NEIGHBOR_ADV;
    icmp[1] = 0;
    icmp[2] = 0; icmp[3] = 0; /* checksum */
    /* Flags: Solicited=1, Override=1 */
    icmp[4] = 0x60;
    icmp[5] = 0; icmp[6] = 0; icmp[7] = 0;
    memcpy(icmp + 8, target, 16);

    /* Target Link-Layer Address option */
    icmp[24] = NDP_OPT_TGT_LLADDR;
    icmp[25] = 1;
    memcpy(icmp + 26, s->local_mac.addr_bytes, 6);

    /* Checksum */
    icmp[2] = 0; icmp[3] = 0;
    uint16_t cksum = icmpv6_checksum(target, sender_ip6,
                                      icmp, (uint16_t)icmpv6_len);
    icmp[2] = (uint8_t)(cksum & 0xff);
    icmp[3] = (uint8_t)(cksum >> 8);

    uint16_t txq = g_port_caps[port_id].mgmt_tx_q;
    rte_eth_tx_burst(port_id, txq, &reply, 1);
}

/* ── Management tick ─────────────────────────────────────────────────────── */
void ndp_mgmt_tick(void)
{
    uint32_t n_ports = rte_eth_dev_count_avail();
    for (uint32_t p = 0; p < n_ports && p < TGEN_MAX_PORTS; p++) {
        if (!g_ndp_rings[p]) continue;

        /* Drain NDP ring */
        void *ptrs[32];
        unsigned n = rte_ring_dequeue_burst(g_ndp_rings[p], ptrs, 32, NULL);
        for (unsigned i = 0; i < n; i++) {
            struct rte_mbuf *m = (struct rte_mbuf *)ptrs[i];
            icmpv6_mgmt_process((uint16_t)p, m);
        }
    }
}

/* ── Destroy ─────────────────────────────────────────────────────────────── */
void ndp_destroy(void)
{
    for (uint32_t p = 0; p < TGEN_MAX_PORTS; p++) {
        if (g_ndp_rings[p]) {
            rte_ring_free(g_ndp_rings[p]);
            g_ndp_rings[p] = NULL;
        }
    }
}
