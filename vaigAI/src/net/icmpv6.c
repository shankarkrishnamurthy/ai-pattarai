/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: ICMPv6 implementation (RFC 4443, RFC 4861).
 */
#include "icmpv6.h"
#include "ndp.h"
#include "ipv6.h"
#include "../core/mempool.h"
#include "../port/port_init.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"
#include "../telemetry/pktrace.h"

#include <string.h>
#include <errno.h>
#include <netinet/in.h>

#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_cycles.h>
#include <rte_ether.h>

static struct rte_ring *g_icmpv6_rings[TGEN_MAX_PORTS];

int icmpv6_init(void)
{
    memset(g_icmpv6_rings, 0, sizeof(g_icmpv6_rings));
    uint32_t n_ports = rte_eth_dev_count_avail();
    for (uint32_t p = 0; p < n_ports && p < TGEN_MAX_PORTS; p++) {
        char name[32];
        snprintf(name, sizeof(name), "icmpv6_ring_%u", p);
        g_icmpv6_rings[p] = rte_ring_create(name, 512,
                                 (int)rte_eth_dev_socket_id((uint16_t)p),
                                 RING_F_SC_DEQ);
        if (!g_icmpv6_rings[p]) return -1;
    }
    return 0;
}

void icmpv6_destroy(void)
{
    for (uint32_t p = 0; p < TGEN_MAX_PORTS; p++) {
        if (g_icmpv6_rings[p]) {
            rte_ring_free(g_icmpv6_rings[p]);
            g_icmpv6_rings[p] = NULL;
        }
    }
}

/* ── ICMPv6 checksum (includes IPv6 pseudo-header) ───────────────────────── */
uint16_t icmpv6_checksum(const uint8_t *src6, const uint8_t *dst6,
                          const void *icmpv6_data, uint16_t icmpv6_len)
{
    /* IPv6 pseudo-header: src(16) + dst(16) + upper-layer length(4) + zeros(3) + next header(1) */
    struct {
        uint8_t  src[16];
        uint8_t  dst[16];
        uint32_t len;
        uint8_t  zeros[3];
        uint8_t  next_hdr;
    } __attribute__((packed)) pseudo;

    memcpy(pseudo.src, src6, 16);
    memcpy(pseudo.dst, dst6, 16);
    pseudo.len = rte_cpu_to_be_32((uint32_t)icmpv6_len);
    pseudo.zeros[0] = 0;
    pseudo.zeros[1] = 0;
    pseudo.zeros[2] = 0;
    pseudo.next_hdr = IPPROTO_ICMPV6;

    uint32_t cksum = rte_raw_cksum(&pseudo, sizeof(pseudo));
    cksum += rte_raw_cksum(icmpv6_data, icmpv6_len);
    cksum = ((cksum >> 16) & 0xFFFF) + (cksum & 0xFFFF);
    cksum += (cksum >> 16);
    return (uint16_t)(~cksum & 0xFFFF);
}

/* ── Worker: forward to mgmt ring ────────────────────────────────────────── */
void icmpv6_input(uint32_t worker_idx, struct rte_mbuf *m)
{
    (void)worker_idx;
    uint16_t port_id = m->port;
    if (port_id >= TGEN_MAX_PORTS || !g_icmpv6_rings[port_id]) {
        rte_pktmbuf_free(m);
        return;
    }
    /* Prepend 32 bytes of metadata (src6 + dst6) before the ICMPv6 payload.
     * The management thread strips them when processing. */
    uint8_t *meta = (uint8_t *)rte_pktmbuf_prepend(m, 32);
    if (!meta) {
        rte_pktmbuf_free(m);
        return;
    }
    memcpy(meta, t_saved_src6, 16);
    memcpy(meta + 16, t_saved_dst6, 16);

    if (rte_ring_enqueue(g_icmpv6_rings[port_id], m) != 0)
        rte_pktmbuf_free(m);
}

/* ── Build ICMPv6 Echo Reply ─────────────────────────────────────────────── */
static struct rte_mbuf *build_echo6_reply(uint16_t port_id,
                                            const uint8_t *requester_ip6,
                                            const uint8_t *local_ip6,
                                            const uint8_t *icmpv6_body,
                                            uint16_t icmpv6_len)
{
    struct rte_mempool *mp = g_worker_mempools[0];
    if (!mp) return NULL;

    struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
    if (!m) return NULL;

    size_t total = sizeof(struct rte_ether_hdr) + 40 + icmpv6_len;
    char *buf = rte_pktmbuf_append(m, (uint16_t)total);
    if (!buf) { rte_pktmbuf_free(m); return NULL; }

    ndp_state_t_port *s = &g_ndp[port_id];

    /* Ethernet */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
    rte_ether_addr_copy(&s->local_mac, &eth->src_addr);
    struct rte_ether_addr resolved_mac;
    if (ndp_lookup(port_id, requester_ip6, &resolved_mac))
        rte_ether_addr_copy(&resolved_mac, &eth->dst_addr);
    else
        memset(eth->dst_addr.addr_bytes, 0xFF, 6);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

    /* IPv6 */
    struct rte_ipv6_hdr *ip6 =
        (struct rte_ipv6_hdr *)(buf + sizeof(struct rte_ether_hdr));
    ip6->vtc_flow = rte_cpu_to_be_32(0x60000000);
    ip6->payload_len = rte_cpu_to_be_16(icmpv6_len);
    ip6->proto = IPPROTO_ICMPV6;
    ip6->hop_limits = 64;
    memcpy(&ip6->src_addr, local_ip6, 16);
    memcpy(&ip6->dst_addr, requester_ip6, 16);

    /* ICMPv6 Echo Reply — copy entire body and change type */
    uint8_t *icmp = (uint8_t *)ip6 + 40;
    memcpy(icmp, icmpv6_body, icmpv6_len);
    icmp[0] = ICMPV6_ECHO_REPLY;
    icmp[2] = 0; icmp[3] = 0; /* clear checksum */
    uint16_t cksum = icmpv6_checksum(local_ip6, requester_ip6,
                                      icmp, icmpv6_len);
    icmp[2] = (uint8_t)(cksum & 0xff);
    icmp[3] = (uint8_t)(cksum >> 8);

    return m;
}

/* ── Management: process one ICMPv6 frame ────────────────────────────────── */
void icmpv6_mgmt_process(uint16_t port_id, struct rte_mbuf *m)
{
    /* The mbuf has 32 bytes of prepended metadata (src6 + dst6)
     * followed by the ICMPv6 payload. */
    if (m->data_len < 32 + ICMPV6_HDR_LEN) {
        rte_pktmbuf_free(m);
        return;
    }

    const uint8_t *meta = rte_pktmbuf_mtod(m, const uint8_t *);
    uint8_t src6[16], dst6[16];
    memcpy(src6, meta, 16);
    memcpy(dst6, meta + 16, 16);

    /* Advance past metadata to ICMPv6 body */
    rte_pktmbuf_adj(m, 32);

    const uint8_t *icmpv6_body = rte_pktmbuf_mtod(m, const uint8_t *);
    uint16_t icmpv6_len = (uint16_t)m->data_len;
    uint8_t type = icmpv6_body[0];

    switch (type) {
    case ICMPV6_ECHO_REQUEST: {
        /* Determine our local IPv6 address for this port */
        ndp_state_t_port *s = &g_ndp[port_id];
        const uint8_t *local = s->has_ip6 ? s->local_ip6 : s->local_ip6_ll;

        struct rte_mbuf *reply = build_echo6_reply(port_id, src6, local,
                                                    icmpv6_body, icmpv6_len);
        if (reply) {
            uint16_t txq = g_port_caps[port_id].mgmt_tx_q;
            rte_eth_tx_burst(port_id, txq, &reply, 1);
            worker_metrics_add_icmp_echo_tx(0);
        }
        rte_pktmbuf_free(m);
        break;
    }
    case ICMPV6_NEIGHBOR_SOL:
        ndp_process_ns(port_id, icmpv6_body, icmpv6_len, src6);
        rte_pktmbuf_free(m);
        break;
    case ICMPV6_NEIGHBOR_ADV:
        ndp_process_na(port_id, icmpv6_body, icmpv6_len);
        rte_pktmbuf_free(m);
        break;
    case ICMPV6_ECHO_REPLY:
        /* Re-prepend metadata and re-enqueue for icmpv6_mgmt_drain */
        {
            uint8_t *pre = (uint8_t *)rte_pktmbuf_prepend(m, 32);
            if (pre) {
                memcpy(pre, src6, 16);
                memcpy(pre + 16, dst6, 16);
                if (rte_ring_enqueue(g_icmpv6_rings[port_id], m) == 0)
                    break;
            }
            rte_pktmbuf_free(m);
        }
        break;
    default:
        rte_pktmbuf_free(m);
        break;
    }
}

/* ── Management: periodic tick ───────────────────────────────────────────── */
void icmpv6_mgmt_tick(void)
{
    uint32_t n_ports = rte_eth_dev_count_avail();
    for (uint32_t p = 0; p < n_ports && p < TGEN_MAX_PORTS; p++) {
        if (!g_icmpv6_rings[p]) continue;

        void *ptrs[32];
        unsigned n = rte_ring_dequeue_burst(g_icmpv6_rings[p], ptrs, 32, NULL);
        for (unsigned i = 0; i < n; i++) {
            struct rte_mbuf *m = (struct rte_mbuf *)ptrs[i];
            if (m->data_len < 32 + ICMPV6_HDR_LEN) {
                rte_pktmbuf_free(m);
                continue;
            }
            /* Peek at type (after 32-byte metadata prefix) */
            const uint8_t *data = rte_pktmbuf_mtod(m, const uint8_t *);
            uint8_t type = data[32]; /* first byte after metadata */
            if (type == ICMPV6_ECHO_REQUEST || type == ICMPV6_NEIGHBOR_SOL ||
                type == ICMPV6_NEIGHBOR_ADV) {
                icmpv6_mgmt_process((uint16_t)p, m);
            } else {
                /* Echo reply or other — re-enqueue for drain */
                if (rte_ring_enqueue(g_icmpv6_rings[p], m) != 0)
                    rte_pktmbuf_free(m);
            }
        }
    }
}

/* ── Management: drain ICMPv6 ring (for ping6 client) ────────────────────── */
struct rte_mbuf *icmpv6_mgmt_drain(uint16_t port_id)
{
    if (port_id >= TGEN_MAX_PORTS || !g_icmpv6_rings[port_id]) return NULL;
    void *m;
    if (rte_ring_dequeue(g_icmpv6_rings[port_id], &m) != 0) return NULL;
    return (struct rte_mbuf *)m;
}

/* ── Management: ping6 ───────────────────────────────────────────────────── */
int icmpv6_ping_start(uint16_t port_id, const uint8_t *dst_ip6,
                       uint32_t count, uint32_t size, uint32_t interval_ms,
                       icmpv6_mgmt_poll_fn poll_fn)
{
    if (count == 0) count = 5;
    if (size == 0) size = 56;
    if (interval_ms == 0) interval_ms = 1000;

    ndp_state_t_port *s = &g_ndp[port_id];
    struct rte_ether_addr dst_mac;

    /* ── Step 1: NDP resolve destination ─────────────────────────── */
    if (!ndp_lookup(port_id, dst_ip6, &dst_mac)) {
        ndp_solicit(port_id, dst_ip6);
        uint64_t deadline = rte_rdtsc() + 3ULL * rte_get_tsc_hz();
        while (rte_rdtsc() < deadline) {
            icmpv6_mgmt_tick();
            pktrace_flush();
            if (ndp_lookup(port_id, dst_ip6, &dst_mac)) break;
            rte_delay_ms(10);
        }
    }
    if (!ndp_lookup(port_id, dst_ip6, &dst_mac)) {
        char buf[46];
        tgen_ipv6_str(dst_ip6, buf, sizeof(buf));
        RTE_LOG(ERR, USER1, "ping6: NDP timeout for %s\n", buf);
        printf("ping6: NDP resolution failed for %s\n", buf);
        return -EHOSTUNREACH;
    }

    struct rte_mempool *mp = g_worker_mempools[0];
    if (!mp) return -ENOMEM;

    const uint8_t *src_ip6 = s->has_ip6 ? s->local_ip6 : s->local_ip6_ll;

    uint32_t sent = 0, rcvd = 0;
    uint16_t seq = 0;
    uint16_t ident = (uint16_t)(rte_rdtsc() & 0xFFFF);

    /* ── Step 2: send / receive loop ─────────────────────────────── */
    for (uint32_t i = 0; i < count; i++, seq++) {

        uint16_t payload_len = (uint16_t)size;
        uint16_t icmpv6_len = ICMPV6_HDR_LEN + payload_len;
        size_t total = sizeof(struct rte_ether_hdr) + 40 + icmpv6_len;

        struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
        if (!m) { RTE_LOG(ERR, USER1, "ping6: mbuf alloc failed\n"); break; }

        char *buf = rte_pktmbuf_append(m, (uint16_t)total);
        if (!buf) { rte_pktmbuf_free(m); break; }

        /* Ethernet */
        struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
        rte_ether_addr_copy(&s->local_mac, &eth->src_addr);
        rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

        /* IPv6 */
        struct rte_ipv6_hdr *ip6 =
            (struct rte_ipv6_hdr *)(buf + sizeof(struct rte_ether_hdr));
        ip6->vtc_flow = rte_cpu_to_be_32(0x60000000);
        ip6->payload_len = rte_cpu_to_be_16(icmpv6_len);
        ip6->proto = IPPROTO_ICMPV6;
        ip6->hop_limits = 64;
        memcpy(&ip6->src_addr, src_ip6, 16);
        memcpy(&ip6->dst_addr, dst_ip6, 16);

        /* ICMPv6 Echo Request */
        uint8_t *icmp = (uint8_t *)ip6 + 40;
        icmp[0] = ICMPV6_ECHO_REQUEST;
        icmp[1] = 0;
        icmp[2] = 0; icmp[3] = 0; /* checksum */
        /* identifier */
        icmp[4] = (uint8_t)(ident >> 8);
        icmp[5] = (uint8_t)(ident & 0xff);
        /* sequence */
        icmp[6] = (uint8_t)(seq >> 8);
        icmp[7] = (uint8_t)(seq & 0xff);
        /* payload */
        memset(icmp + ICMPV6_HDR_LEN, 0xAB, payload_len);
        /* checksum */
        uint16_t cksum = icmpv6_checksum(src_ip6, dst_ip6,
                                          icmp, icmpv6_len);
        icmp[2] = (uint8_t)(cksum & 0xff);
        icmp[3] = (uint8_t)(cksum >> 8);

        uint64_t t0 = rte_rdtsc();
        uint16_t txq = g_port_caps[port_id].mgmt_tx_q;
        uint16_t nb = rte_eth_tx_burst(port_id, txq, &m, 1);
        if (nb == 0) { rte_pktmbuf_free(m); continue; }
        sent++;

        /* Wait for reply */
        uint64_t wait_end = t0 + (uint64_t)interval_ms * rte_get_tsc_hz() / 1000;
        bool got_reply = false;
        while (rte_rdtsc() < wait_end) {
            struct rte_mbuf *r = icmpv6_mgmt_drain(port_id);
            if (r) {
                uint64_t t1 = rte_rdtsc();
                double rtt_ms = (double)(t1 - t0) * 1000.0 / (double)rte_get_tsc_hz();
                char dst_str[46];
                tgen_ipv6_str(dst_ip6, dst_str, sizeof(dst_str));
                printf("Reply from %s: icmp_seq=%u time=%.3f ms\n",
                       dst_str, seq, rtt_ms);
                rte_pktmbuf_free(r);
                rcvd++;
                got_reply = true;
                break;
            }
            icmpv6_mgmt_tick();
            if (poll_fn) poll_fn();
            rte_delay_ms(1);
        }
        if (!got_reply)
            printf("Request timeout for icmp_seq=%u\n", seq);

        /* Pace subsequent packets */
        if (i + 1 < count) {
            uint64_t next = t0 + (uint64_t)interval_ms * rte_get_tsc_hz() / 1000;
            while (rte_rdtsc() < next) {
                pktrace_flush();
                rte_pause();
            }
        }
    }

    char dst_str[46];
    tgen_ipv6_str(dst_ip6, dst_str, sizeof(dst_str));
    printf("\n--- %s ping6 statistics ---\n"
           "%u packets transmitted, %u received, %u%% packet loss\n",
           dst_str, sent, rcvd,
           sent ? (sent - rcvd) * 100 / sent : 0);
    return (int)rcvd;
}
