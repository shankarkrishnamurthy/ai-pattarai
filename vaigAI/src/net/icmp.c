/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: ICMP implementation (§2.4, RFC 792).
 */
#include "icmp.h"
#include "arp.h"
#include "../core/mempool.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"

#include <string.h>
#include <errno.h>
#include <netinet/in.h>

#include <rte_icmp.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_cycles.h>

#define ICMP_HDR_LEN  8   /* standard ICMP header size */

static struct rte_ring *g_icmp_rings[TGEN_MAX_PORTS];

/* Token bucket for rate limiting unreachable/time-exceeded */
static uint64_t g_icmp_tokens[TGEN_MAX_PORTS];
static uint64_t g_icmp_last_ts[TGEN_MAX_PORTS];

int icmp_init(void)
{
    memset(g_icmp_rings, 0, sizeof(g_icmp_rings));
    uint32_t n_ports = rte_eth_dev_count_avail();
    for (uint32_t p = 0; p < n_ports && p < TGEN_MAX_PORTS; p++) {
        char name[32];
        snprintf(name, sizeof(name), "icmp_ring_%u", p);
        g_icmp_rings[p] = rte_ring_create(name, 512,
                              (int)rte_eth_dev_socket_id((uint16_t)p),
                              RING_F_SC_DEQ);
        if (!g_icmp_rings[p]) return -1;
        g_icmp_tokens[p] = ICMP_RATE_LIMIT;
        g_icmp_last_ts[p] = rte_rdtsc();
    }
    return 0;
}

void icmp_destroy(void)
{
    for (uint32_t p = 0; p < TGEN_MAX_PORTS; p++) {
        if (g_icmp_rings[p]) {
            rte_ring_free(g_icmp_rings[p]);
            g_icmp_rings[p] = NULL;
        }
    }
}

/* Worker: forward to mgmt ring */
void icmp_input(uint32_t worker_idx, struct rte_mbuf *m)
{
    (void)worker_idx;
    uint16_t port_id = m->port;
    if (port_id >= TGEN_MAX_PORTS || !g_icmp_rings[port_id]) {
        rte_pktmbuf_free(m);
        return;
    }
    if (rte_ring_enqueue(g_icmp_rings[port_id], m) != 0)
        rte_pktmbuf_free(m);
}

/* Build an ICMP Echo Reply */
static struct rte_mbuf *build_echo_reply(uint16_t port_id,
                                          const struct rte_ipv4_hdr *orig_ip,
                                          const struct rte_icmp_hdr *req)
{
    struct rte_mempool *mp = g_worker_mempools[0];
    if (!mp) return NULL;

    struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
    if (!m) return NULL;

    /* Copy original ICMP payload (identifier + seq + data) */
    uint16_t icmp_data_len = (uint16_t)(rte_be_to_cpu_16(orig_ip->total_length)
                             - (orig_ip->version_ihl & 0x0F) * 4
                             - ICMP_HDR_LEN);

    size_t total = sizeof(struct rte_ether_hdr) +
                   sizeof(struct rte_ipv4_hdr) +
                   ICMP_HDR_LEN + icmp_data_len;

    char *buf = rte_pktmbuf_append(m, (uint16_t)total);
    if (!buf) { rte_pktmbuf_free(m); return NULL; }

    /* Ethernet */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
    rte_ether_addr_copy(&g_arp[port_id].local_mac, &eth->src_addr);
    /* dst comes from ARP cache; skip for now (set to zero) */
    memset(eth->dst_addr.addr_bytes, 0, 6);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* IPv4 */
    struct rte_ipv4_hdr *ip =
        (struct rte_ipv4_hdr *)(buf + sizeof(struct rte_ether_hdr));
    ip->version_ihl   = RTE_IPV4_VHL_DEF;
    ip->type_of_service = 0;
    ip->total_length  = rte_cpu_to_be_16(
        (uint16_t)(sizeof(*ip) + ICMP_HDR_LEN + icmp_data_len));
    ip->packet_id     = 0;
    ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    ip->time_to_live  = 64;
    ip->next_proto_id = IPPROTO_ICMP;
    ip->hdr_checksum  = 0;
    ip->src_addr      = g_arp[port_id].local_ip;
    ip->dst_addr      = orig_ip->src_addr;
    ip->hdr_checksum  = rte_ipv4_cksum(ip);

    /* ICMP echo reply */
    struct rte_icmp_hdr *icmp =
        (struct rte_icmp_hdr *)((uint8_t *)ip + sizeof(*ip));
    icmp->icmp_type   = RTE_ICMP_TYPE_ECHO_REPLY;
    icmp->icmp_code   = 0;
    icmp->icmp_cksum  = 0;
    /* Copy identifier + sequence + payload verbatim */
    memcpy((uint8_t *)icmp + 4,
           (const uint8_t *)req + 4,
           icmp_data_len);
    uint16_t ck_reply = rte_raw_cksum(icmp, ICMP_HDR_LEN + icmp_data_len);
    icmp->icmp_cksum = (ck_reply == 0xFFFF) ? ck_reply : (uint16_t)~ck_reply;

    return m;
}

/* Management: process one ICMP frame */
void icmp_mgmt_process(uint16_t port_id, struct rte_mbuf *m)
{
    /* The mbuf data pointer is now at start of IP header */
    const struct rte_ipv4_hdr *ip = rte_pktmbuf_mtod(m, struct rte_ipv4_hdr *);
    size_t ip_hlen = (ip->version_ihl & 0x0F) * 4;
    const struct rte_icmp_hdr *icmp =
        (const struct rte_icmp_hdr *)((const uint8_t *)ip + ip_hlen);

    /* Validate ICMP checksum */
    uint16_t total = rte_be_to_cpu_16(ip->total_length);
    uint16_t icmp_len = (uint16_t)(total - (uint16_t)ip_hlen);
    if (rte_raw_cksum(icmp, icmp_len) != 0xFFFF) {
        worker_metrics_add_icmp_bad_cksum(0);
        rte_pktmbuf_free(m);
        return;
    }

    if (icmp->icmp_type == RTE_ICMP_TYPE_ECHO_REQUEST) {
        struct rte_mbuf *reply = build_echo_reply(port_id, ip, icmp);
        if (reply) {
            rte_eth_tx_burst(port_id, 0, &reply, 1);
            worker_metrics_add_icmp_echo_tx(0);
        }
    }
    rte_pktmbuf_free(m);
}

/* Management: drain ICMP ring */
struct rte_mbuf *icmp_mgmt_drain(uint16_t port_id)
{
    if (port_id >= TGEN_MAX_PORTS || !g_icmp_rings[port_id]) return NULL;
    void *m;
    if (rte_ring_dequeue(g_icmp_rings[port_id], &m) != 0) return NULL;
    return (struct rte_mbuf *)m;
}

int icmp_send_unreachable(uint16_t port_id, uint8_t code,
                           struct rte_mbuf *orig_m)
{
    (void)port_id; (void)code; (void)orig_m;
    /* TODO: token-bucket check; build Unreachable; send */
    rte_pktmbuf_free(orig_m);
    return 0;
}

int icmp_send_time_exceeded(uint16_t port_id, struct rte_mbuf *orig_m)
{
    (void)port_id; (void)orig_m;
    /* TODO: build Time Exceeded; send */
    rte_pktmbuf_free(orig_m);
    return 0;
}

int icmp_ping_start(uint16_t port_id, uint32_t dst_ip_net,
                    uint32_t count, uint32_t size, uint32_t interval_ms)
{
    if (count == 0) count = 5;
    if (size  == 0) size  = 56;
    if (interval_ms == 0) interval_ms = 1000;

    struct rte_ether_addr dst_mac;

    /* ── Step 1: ARP resolve destination ─────────────────────────── */
    if (!arp_lookup(port_id, dst_ip_net, &dst_mac)) {
        arp_request(port_id, dst_ip_net);
        uint64_t deadline = rte_rdtsc() + 3ULL * rte_get_tsc_hz();
        while (rte_rdtsc() < deadline) {
            arp_mgmt_tick();          /* drains ARP ring from workers */
            if (arp_lookup(port_id, dst_ip_net, &dst_mac)) break;
            rte_delay_ms(10);
        }
    }
    if (!arp_lookup(port_id, dst_ip_net, &dst_mac)) {
        uint32_t h = rte_be_to_cpu_32(dst_ip_net);
        RTE_LOG(ERR, USER1, "ping: ARP timeout for %u.%u.%u.%u\n",
                (h >> 24) & 0xFF, (h >> 16) & 0xFF,
                (h >>  8) & 0xFF,  h        & 0xFF);
        return -EHOSTUNREACH;
    }

    struct rte_mempool *mp = g_worker_mempools[0];
    if (!mp) return -ENOMEM;

    uint32_t sent = 0, rcvd = 0;
    uint16_t seq  = 0;
    uint16_t ident = (uint16_t)(rte_rdtsc() & 0xFFFF);

    /* ── Step 2: send / receive loop ─────────────────────────────── */
    for (uint32_t i = 0; i < count; i++, seq++) {

        /* Build ICMP echo request */
        uint16_t payload_len = (uint16_t)size;
        size_t total = sizeof(struct rte_ether_hdr)
                     + sizeof(struct rte_ipv4_hdr)
                     + ICMP_HDR_LEN + payload_len;

        struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
        if (!m) { RTE_LOG(ERR, USER1, "ping: mbuf alloc failed\n"); break; }

        char *buf = rte_pktmbuf_append(m, (uint16_t)total);
        if (!buf) { rte_pktmbuf_free(m); break; }

        /* Ethernet */
        struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
        rte_ether_addr_copy(&g_arp[port_id].local_mac, &eth->src_addr);
        rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

        /* IPv4 */
        struct rte_ipv4_hdr *ip =
            (struct rte_ipv4_hdr *)(buf + sizeof(struct rte_ether_hdr));
        ip->version_ihl     = RTE_IPV4_VHL_DEF;
        ip->type_of_service = 0;
        ip->total_length    = rte_cpu_to_be_16(
            (uint16_t)(sizeof(*ip) + ICMP_HDR_LEN + payload_len));
        ip->packet_id       = rte_cpu_to_be_16(seq);
        ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
        ip->time_to_live    = 64;
        ip->next_proto_id   = IPPROTO_ICMP;
        ip->hdr_checksum    = 0;
        ip->src_addr        = g_arp[port_id].local_ip;
        ip->dst_addr        = dst_ip_net;
        ip->hdr_checksum    = rte_ipv4_cksum(ip);

        /* ICMP echo request */
        struct rte_icmp_hdr *icmp =
            (struct rte_icmp_hdr *)((uint8_t *)ip + sizeof(*ip));
        icmp->icmp_type  = RTE_ICMP_TYPE_ECHO_REQUEST;
        icmp->icmp_code  = 0;
        icmp->icmp_cksum = 0;
        /* identifier (hi 16 bits after type/code/cksum) */
        *((uint16_t *)((uint8_t *)icmp + 4)) = rte_cpu_to_be_16(ident);
        /* sequence */
        *((uint16_t *)((uint8_t *)icmp + 6)) = rte_cpu_to_be_16(seq);
        /* payload */
        memset((uint8_t *)icmp + ICMP_HDR_LEN, 0xAB, payload_len);
        uint16_t ck_req = rte_raw_cksum(icmp, ICMP_HDR_LEN + payload_len);
        icmp->icmp_cksum = (ck_req == 0xFFFF) ? ck_req : (uint16_t)~ck_req;

        uint64_t t0 = rte_rdtsc();
        uint16_t nb = rte_eth_tx_burst(port_id, 0, &m, 1);
        if (nb == 0) { rte_pktmbuf_free(m); continue; }
        sent++;

        /* Wait up to interval_ms for a reply (drain icmp ring) */
        uint64_t wait_end = t0 + (uint64_t)interval_ms * rte_get_tsc_hz() / 1000;
        bool got_reply = false;
        while (rte_rdtsc() < wait_end) {
            struct rte_mbuf *r = icmp_mgmt_drain(port_id);
            if (r) {
                uint64_t t1 = rte_rdtsc();
                double rtt_ms = (double)(t1 - t0) * 1000.0 / (double)rte_get_tsc_hz();
                uint32_t _dip = rte_be_to_cpu_32(dst_ip_net);
                printf("Reply from %u.%u.%u.%u: icmp_seq=%u time=%.3f ms\n",
                       (_dip >> 24) & 0xFF, (_dip >> 16) & 0xFF,
                       (_dip >>  8) & 0xFF,  _dip        & 0xFF,
                       seq, rtt_ms);
                rte_pktmbuf_free(r);
                rcvd++;
                got_reply = true;
                break;
            }
            /* Also keep draining ARP ring to avoid stalls */
            arp_mgmt_tick();
            rte_delay_ms(1);
        }
        if (!got_reply)
            printf("Request timeout for icmp_seq=%u\n", seq);

        /* Pace subsequent packets */
        if (i + 1 < count) {
            uint64_t next = t0 + (uint64_t)interval_ms * rte_get_tsc_hz() / 1000;
            while (rte_rdtsc() < next) rte_pause();
        }
    }

    char dst_str[16];
    uint32_t _dip2 = rte_be_to_cpu_32(dst_ip_net);
    snprintf(dst_str, sizeof(dst_str), "%u.%u.%u.%u",
             (_dip2 >> 24) & 0xFF, (_dip2 >> 16) & 0xFF,
             (_dip2 >>  8) & 0xFF,  _dip2        & 0xFF);
    printf("\n--- %s ping statistics ---\n"
           "%u packets transmitted, %u received, %u%% packet loss\n",
           dst_str, sent, rcvd,
           sent ? (sent - rcvd) * 100 / sent : 0);
    return (int)rcvd;
}
