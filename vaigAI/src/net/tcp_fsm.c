/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP FSM implementation (§3.2, RFC 793 + RFC 7323 + RFC 5681).
 */
#include "tcp_fsm.h"
#include "tcp_timer.h"
#include "tcp_tcb.h"
#include "tcp_snd_buf.h"
#include "tcp_options.h"
#include "tcp_port_pool.h"
#include "tcp_checksum.h"
#include "tcp_congestion.h"
#include "../net/ipv4.h"
#include "../net/ipv6.h"
#include "../net/arp.h"
#include "../net/ndp.h"
#include "../net/ethernet.h"
#include "../port/port_init.h"
#include "../core/mempool.h"
#include "../core/core_assign.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"
#include "../telemetry/histogram.h"
#include "../tls/tls_session.h"
#include "../tls/tls_engine.h"
#include "../core/tx_gen.h"
#include "../telemetry/log.h"
#include "../app/server.h"

#include <string.h>
#include <netinet/in.h>

#include <rte_tcp.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include <rte_byteorder.h>
#include <rte_log.h>

/* ── HTTP response body tracking helpers ─────────────────────────────────── */

/* Parse Content-Length from an HTTP response buffer.
 * Returns the value, or 0 if not found / not parseable.
 * buf must be at least buf_len bytes; does NOT require NUL termination. */
static inline uint32_t
http_parse_content_length(const uint8_t *buf, uint32_t buf_len)
{
    /* memmem is a glibc extension, available on Linux */
    const uint8_t *p = memmem(buf, buf_len, "Content-Length: ", 16);
    if (!p)
        p = memmem(buf, buf_len, "content-length: ", 16);
    if (!p)
        return 0;
    p += 16;
    uint32_t rem = (uint32_t)(buf_len - (uint32_t)(p - buf));
    uint32_t val = 0;
    for (uint32_t i = 0; i < rem && p[i] >= '0' && p[i] <= '9'; i++)
        val = val * 10 + (p[i] - '0');
    return val;
}

/* Find the first byte of the HTTP response body (after \r\n\r\n).
 * Returns the offset into buf, or buf_len if header terminator not found. */
static inline uint32_t
http_body_offset(const uint8_t *buf, uint32_t buf_len)
{
    const uint8_t *p = memmem(buf, buf_len, "\r\n\r\n", 4);
    if (!p)
        return buf_len;
    return (uint32_t)(p - buf) + 4;
}

/* After receiving the first HTTP response segment: set up body tracking and
 * decide what to do next.  Returns true if the connection was closed/reset
 * (caller must goto done). */
static inline bool
http_rsp_body_start(uint32_t worker_idx, tcb_t *tcb,
                    const uint8_t *buf, uint32_t buf_len)
{
    tcb->http_content_length = http_parse_content_length(buf, buf_len);
    if (tcb->http_content_length == 0) {
        /* No Content-Length (chunked, connection-close, etc.) —
         * fall back to passive close: server will send FIN when done. */
        tcb->app_state = 0;
        return false;
    }

    uint32_t body_off = http_body_offset(buf, buf_len);
    tcb->http_body_rx = (buf_len > body_off) ? (buf_len - body_off) : 0;

    if (tcb->http_body_rx >= tcb->http_content_length) {
        /* Full body already arrived in this segment — active close now. */
        tcb->app_state = 0;
        if (tcb->graceful_close) {
            tcp_fsm_close(worker_idx, tcb);
            return true;
        }
        tcp_fsm_reset(worker_idx, tcb);
        return true;
    }

    /* More body to come — wait in state 6. */
    tcb->app_state = 6;
    return false;
}

/* While in state 6 (accumulating body): add data_len bytes and close when
 * done.  Returns true if the connection was closed/reset. */
static inline bool
http_rsp_body_recv(uint32_t worker_idx, tcb_t *tcb, uint32_t data_len)
{
    tcb->http_body_rx += data_len;
    if (tcb->http_body_rx >= tcb->http_content_length) {
        tcb->app_state = 0;
        if (tcb->graceful_close) {
            tcp_fsm_close(worker_idx, tcb);
            return true;
        }
        tcp_fsm_reset(worker_idx, tcb);
        return true;
    }
    return false;
}
#define SEQ_LT(a,b)   ((int32_t)((a)-(b)) <  0)
#define SEQ_LE(a,b)   ((int32_t)((a)-(b)) <= 0)
#define SEQ_GT(a,b)   ((int32_t)((a)-(b)) >  0)
#define SEQ_GE(a,b)   ((int32_t)((a)-(b)) >= 0)

/* ── Check if current lcore is a worker (vs mgmt) ───────────────────────── */
static inline bool is_worker_lcore(void)
{
    return tgen_is_worker(rte_lcore_id());
}

/* ── Detach TLS session before freeing a TCB ─────────────────────────────── */
static inline void
tls_detach_if_needed(uint32_t worker_idx, tcb_t *tcb)
{
    if (tcb->app_state >= 2) {
        uint32_t ci = (uint32_t)(tcb - g_tcb_stores[worker_idx].tcbs);
        tls_session_detach(worker_idx, ci);
        tcb->app_state = 0;
    }
}

/* ── Secure ISN generation (RFC 6528) ────────────────────────────────────── */
static uint64_t g_isn_secret[2];  /* random secret, set once at startup */

__attribute__((constructor))
static void isn_secret_init(void)
{
    /* Use rte_rdtsc as entropy seed — combined with SipHash this is
     * sufficiently unpredictable for a traffic generator. */
    g_isn_secret[0] = rte_rdtsc() ^ 0xdeadbeefcafe1234ULL;
    g_isn_secret[1] = rte_rdtsc() ^ 0x1122334455667788ULL;
}

static uint32_t isn_generate(uint32_t sip, uint16_t sport,
                              uint32_t dip, uint16_t dport)
{
    /* SipHash-2-4-like mixing of 4-tuple + secret + clock */
    uint64_t v = g_isn_secret[0] ^ ((uint64_t)sip << 32 | dip);
    v ^= ((uint64_t)sport << 16 | dport) * 0x9E3779B97F4A7C15ULL;
    v ^= g_isn_secret[1];
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    /* Add monotonic clock component (RFC 6528 §3: M + F(...)) */
    uint32_t m = (uint32_t)(rte_rdtsc() >> 8); /* ~4µs granularity */
    return (uint32_t)(v >> 32) + m;
}

/* ── Per-worker IP ID counter ────────────────────────────────────────────── */
static uint32_t g_tcp_ip_id[TGEN_MAX_WORKERS];

/* ── Per-worker TCP TX batch buffer ──────────────────────────────────────── */
/* Accumulates TCP segments and flushes them in a single rte_eth_tx_burst()
 * call, amortizing the sendto() syscall overhead of AF_PACKET/TAP PMDs.
 * Only used on worker lcores; management lcore sends immediately. */
#define TCP_TX_BATCH_MAX 64
static struct {
    struct rte_mbuf *pkts[TCP_TX_BATCH_MAX];
    uint16_t count;
    uint16_t port_id;
    uint16_t tx_queue;
} g_tcp_tx_buf[TGEN_MAX_WORKERS];

static inline void
tcp_tx_enqueue(uint32_t worker_idx, uint16_t port_id, uint16_t tx_q,
               struct rte_mbuf *m)
{
    typeof(g_tcp_tx_buf[0]) *tb = &g_tcp_tx_buf[worker_idx];

    /* Flush if port/queue changed or buffer full */
    if (tb->count > 0 &&
        (tb->port_id != port_id || tb->tx_queue != tx_q ||
         tb->count >= TCP_TX_BATCH_MAX)) {
        tcp_tx_flush(worker_idx);
    }

    tb->port_id  = port_id;
    tb->tx_queue = tx_q;
    tb->pkts[tb->count++] = m;
}

void
tcp_tx_flush(uint32_t worker_idx)
{
    typeof(g_tcp_tx_buf[0]) *tb = &g_tcp_tx_buf[worker_idx];
    if (tb->count == 0) return;

    uint16_t sent = rte_eth_tx_burst(tb->port_id, tb->tx_queue,
                                      tb->pkts, tb->count);
    for (uint16_t i = sent; i < tb->count; i++)
        rte_pktmbuf_free(tb->pkts[i]);
    tb->count = 0;
}

/* ── Build and send a TCP segment ────────────────────────────────────────── */
int tcp_send_segment(uint32_t worker_idx, tcb_t *tcb,
                     uint8_t flags,
                     const uint8_t *payload, uint32_t payload_len,
                     uint32_t seq, uint32_t ack)
{
    struct rte_mempool *mp = g_worker_mempools[worker_idx];
    struct rte_mbuf    *m  = rte_pktmbuf_alloc(mp);
    if (!m) return -1;

    /* TCP options (SYN: up to 20 bytes; other: up to 12 bytes) */
    uint8_t opts[40];
    int   opts_len = 0;
    uint32_t ts_val = (uint32_t)(rte_rdtsc() / (g_tsc_hz / 1000000));

    if (flags & RTE_TCP_SYN_FLAG) {
        opts_len = tcp_options_write_syn(opts, sizeof(opts),
                       tcb->mss_local, tcb->wscale_local,
                       true, true, ts_val, tcb->ts_ecr);
    } else {
        opts_len = tcp_options_write_data(opts, sizeof(opts),
                       tcb->ts_enabled, ts_val, tcb->ts_ecr,
                       tcb->sack_block_count ? tcb->sack_blocks : NULL,
                       tcb->sack_block_count);
    }
    if (opts_len < 0) opts_len = 0;

    size_t tcp_hdr_sz = sizeof(struct rte_tcp_hdr) + (size_t)opts_len;
    size_t seg_len    = tcp_hdr_sz + payload_len;

    bool is_v6 = (tcb->ip_version == 6);
    size_t ip_hdr_sz = is_v6 ? IPV6_HDR_LEN : sizeof(struct rte_ipv4_hdr);

    char *buf = rte_pktmbuf_append(m, (uint16_t)(
        sizeof(struct rte_ether_hdr) + ip_hdr_sz + seg_len));
    if (!buf) { rte_pktmbuf_free(m); return -1; }

    /* Ethernet header */
    uint16_t port_id = tcb->port_id;
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
    rte_ether_addr_copy(&g_port_caps[port_id].mac_addr, &eth->src_addr);
    /* Use cached destination MAC if available, otherwise resolve via ARP/NDP */
    if (likely(tcb->dst_mac_valid)) {
        rte_ether_addr_copy(&tcb->dst_mac, &eth->dst_addr);
    } else {
        struct rte_ether_addr resolved_mac;
        bool resolved = false;
        if (is_v6) {
            resolved = ndp_lookup(port_id, tcb->dst_ip6, &resolved_mac);
        } else {
            resolved = arp_lookup(port_id, arp_nexthop(port_id, tcb->dst_ip), &resolved_mac);
        }
        if (resolved) {
            rte_ether_addr_copy(&resolved_mac, &tcb->dst_mac);
            tcb->dst_mac_valid = true;
        } else {
            memset(resolved_mac.addr_bytes, 0xFF, 6);
        }
        rte_ether_addr_copy(&resolved_mac, &eth->dst_addr);
    }
    eth->ether_type = rte_cpu_to_be_16(is_v6 ? RTE_ETHER_TYPE_IPV6 : RTE_ETHER_TYPE_IPV4);

    struct rte_tcp_hdr *tcp_h;

    if (is_v6) {
        /* IPv6 header */
        struct rte_ipv6_hdr *ip6 =
            (struct rte_ipv6_hdr *)(buf + sizeof(struct rte_ether_hdr));
        ip6->vtc_flow = rte_cpu_to_be_32(0x60000000);
        ip6->payload_len = rte_cpu_to_be_16((uint16_t)seg_len);
        ip6->proto = IPPROTO_TCP;
        ip6->hop_limits = 64;
        memcpy(&ip6->src_addr, tcb->src_ip6, 16);
        memcpy(&ip6->dst_addr, tcb->dst_ip6, 16);

        tcp_h = (struct rte_tcp_hdr *)((uint8_t *)ip6 + IPV6_HDR_LEN);
    } else {
        /* IPv4 header */
        struct rte_ipv4_hdr *ip =
            (struct rte_ipv4_hdr *)(buf + sizeof(struct rte_ether_hdr));
        ip->version_ihl   = RTE_IPV4_VHL_DEF;
        ip->type_of_service = 0;
        ip->total_length  = rte_cpu_to_be_16(
            (uint16_t)(sizeof(*ip) + seg_len));
        ip->packet_id     = rte_cpu_to_be_16(
            (uint16_t)(g_tcp_ip_id[worker_idx]++ & 0xFFFF));
        ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
        ip->time_to_live  = 64;
        ip->next_proto_id = IPPROTO_TCP;
        ip->hdr_checksum  = 0;
        ip->src_addr      = tcb->src_ip;
        ip->dst_addr      = tcb->dst_ip;

        tcp_h = (struct rte_tcp_hdr *)((uint8_t *)ip + sizeof(*ip));
    }

    /* TCP header */
    memset(tcp_h, 0, sizeof(*tcp_h));
    tcp_h->src_port = rte_cpu_to_be_16(tcb->src_port);
    tcp_h->dst_port = rte_cpu_to_be_16(tcb->dst_port);
    tcp_h->sent_seq = rte_cpu_to_be_32(seq);
    tcp_h->recv_ack = rte_cpu_to_be_32(ack);
    tcp_h->data_off = (uint8_t)(((tcp_hdr_sz / 4) & 0x0F) << 4);
    tcp_h->tcp_flags = flags;
    tcp_h->rx_win   = rte_cpu_to_be_16((uint16_t)(tcb->rcv_wnd >> tcb->wscale_local));
    tcp_h->cksum    = 0;

    /* Copy options */
    if (opts_len > 0)
        memcpy((uint8_t *)tcp_h + sizeof(*tcp_h), opts, (size_t)opts_len);

    /* Copy payload */
    if (payload && payload_len > 0)
        memcpy((uint8_t *)tcp_h + tcp_hdr_sz, payload, payload_len);

    /* Set L2/L3/L4 lengths and compute checksums */
    m->l2_len = sizeof(struct rte_ether_hdr);
    m->l3_len = (uint16_t)ip_hdr_sz;
    m->l4_len = (uint16_t)tcp_hdr_sz;
    if (is_v6) {
        tcp_checksum_set_v6(m, tcb->src_ip6, tcb->dst_ip6, tcp_h,
                            (uint16_t)seg_len,
                            g_port_caps[port_id].has_tcp_cksum_offload);
    } else {
        struct rte_ipv4_hdr *ip =
            (struct rte_ipv4_hdr *)(buf + sizeof(struct rte_ether_hdr));
        tcp_checksum_set(m, ip, tcp_h,
                         g_port_caps[port_id].has_tcp_cksum_offload);
    }

    m->port = port_id;

    /* Transmit — batch on worker lcores, send immediately on mgmt lcore */
    uint16_t tx_q;
    if (is_worker_lcore()) {
        tx_q = (uint16_t)worker_idx % g_port_caps[port_id].max_tx_queues;
        tcp_tx_enqueue(worker_idx, port_id, tx_q, m);
    } else {
        tx_q = g_port_caps[port_id].mgmt_tx_q;
        uint16_t sent = rte_eth_tx_burst(port_id, tx_q, &m, 1);
        if (sent == 0) { rte_pktmbuf_free(m); return -1; }
    }
    worker_metrics_add_tx(worker_idx, 1, (uint32_t)seg_len);
    /* Piggybacking an ACK clears any pending delayed-ACK. */
    if ((flags & RTE_TCP_ACK_FLAG) && !(flags & RTE_TCP_SYN_FLAG))
        tcb->pending_ack = false;
    return 0;
}

/* ── Update RTO (RFC 6298) ───────────────────────────────────────────────── */
static void update_rtt(tcb_t *tcb, uint32_t rtt_us)
{
    if (tcb->srtt_us == 0) {
        tcb->srtt_us  = rtt_us;
        tcb->rttvar_us = rtt_us / 2;
    } else {
        uint32_t delta = (rtt_us > tcb->srtt_us) ?
                         rtt_us - tcb->srtt_us : tcb->srtt_us - rtt_us;
        tcb->rttvar_us = (3 * tcb->rttvar_us + delta) / 4;
        tcb->srtt_us   = (7 * tcb->srtt_us + rtt_us) / 8;
    }
    uint32_t rto_us = tcb->srtt_us + 4 * tcb->rttvar_us;
    tcb->rto_us = TGEN_CLAMP(rto_us,
                              (uint32_t)200000,    /* 200 ms min */
                              (uint32_t)TCP_MAX_RTO_US);
}

/* ── Arm RTO timer ────────────────────────────────────────────────────────── */
static inline void arm_rto(uint32_t worker_idx, tcb_t *tcb)
{
    tcb->rto_deadline_tsc = rte_rdtsc() +
                             (uint64_t)tcb->rto_us * g_tsc_hz / 1000000ULL;
    tcp_timer_resched(worker_idx, tcb);
}

/* ── Send RST for a packet that has no matching TCB (RFC 793 §3.4) ──────── *
 * Currently disabled: for a traffic generator, the RST storm from replying
 * to every stale packet overwhelms the server and hurts HTTP completion.   */
__rte_unused
static void tcp_send_rst_no_tcb(uint32_t worker_idx, struct rte_mbuf *m,
                                 const struct rte_tcp_hdr *tcp_in,
                                 uint32_t local_ip, uint32_t remote_ip)
{
    uint16_t port_id = 0;
    struct rte_mempool *mp = g_worker_mempools[worker_idx];
    struct rte_mbuf *rst = rte_pktmbuf_alloc(mp);
    if (!rst) return;

    size_t tcp_hdr_sz = sizeof(struct rte_tcp_hdr);
    char *buf = rte_pktmbuf_append(rst, (uint16_t)(
        sizeof(struct rte_ether_hdr) +
        sizeof(struct rte_ipv4_hdr) +
        tcp_hdr_sz));
    if (!buf) { rte_pktmbuf_free(rst); return; }

    /* Ethernet */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
    rte_ether_addr_copy(&g_port_caps[port_id].mac_addr, &eth->src_addr);
    struct rte_ether_addr dst_mac;
    if (!arp_lookup(port_id, arp_nexthop(port_id, remote_ip), &dst_mac))
        memset(dst_mac.addr_bytes, 0xFF, 6);
    rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* IPv4 */
    struct rte_ipv4_hdr *ip =
        (struct rte_ipv4_hdr *)(buf + sizeof(struct rte_ether_hdr));
    ip->version_ihl    = RTE_IPV4_VHL_DEF;
    ip->type_of_service = 0;
    ip->total_length   = rte_cpu_to_be_16(
        (uint16_t)(sizeof(*ip) + tcp_hdr_sz));
    ip->packet_id      = 0;
    ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    ip->time_to_live   = 64;
    ip->next_proto_id  = IPPROTO_TCP;
    ip->hdr_checksum   = 0;
    ip->src_addr       = local_ip;
    ip->dst_addr       = remote_ip;

    /* TCP RST — RFC 793: if ACK bit on, SEQ = ACK of incoming; else ACK = SEQ+LEN */
    struct rte_tcp_hdr *tcp_h =
        (struct rte_tcp_hdr *)((uint8_t *)ip + sizeof(*ip));
    memset(tcp_h, 0, sizeof(*tcp_h));
    tcp_h->src_port  = tcp_in->dst_port;  /* already in NBO */
    tcp_h->dst_port  = tcp_in->src_port;
    tcp_h->data_off  = (uint8_t)((sizeof(*tcp_h) / 4) << 4);

    uint32_t in_seq = rte_be_to_cpu_32(tcp_in->sent_seq);
    uint8_t  in_flags = tcp_in->tcp_flags;
    uint8_t  in_doff  = (tcp_in->data_off >> 4) & 0x0F;
    uint32_t seg_len  = (uint32_t)m->data_len - (uint32_t)(in_doff * 4);
    if (in_flags & RTE_TCP_SYN_FLAG) seg_len++;
    if (in_flags & RTE_TCP_FIN_FLAG) seg_len++;

    if (in_flags & RTE_TCP_ACK_FLAG) {
        tcp_h->sent_seq  = tcp_in->recv_ack;
        tcp_h->recv_ack  = 0;
        tcp_h->tcp_flags = RTE_TCP_RST_FLAG;
    } else {
        tcp_h->sent_seq  = 0;
        tcp_h->recv_ack  = rte_cpu_to_be_32(in_seq + seg_len);
        tcp_h->tcp_flags = RTE_TCP_RST_FLAG | RTE_TCP_ACK_FLAG;
    }
    tcp_h->rx_win = 0;
    tcp_h->cksum  = 0;

    rst->l2_len = sizeof(struct rte_ether_hdr);
    rst->l3_len = sizeof(struct rte_ipv4_hdr);
    rst->l4_len = (uint16_t)tcp_hdr_sz;
    tcp_checksum_set(rst, ip, tcp_h,
                     g_port_caps[port_id].has_tcp_cksum_offload);
    rst->port = port_id;

    if (is_worker_lcore()) {
        uint16_t tx_q = (uint16_t)worker_idx % g_port_caps[port_id].max_tx_queues;
        tcp_tx_enqueue(worker_idx, port_id, tx_q, rst);
    } else {
        uint16_t tx_q = g_port_caps[port_id].mgmt_tx_q;
        uint16_t sent = rte_eth_tx_burst(port_id, tx_q, &rst, 1);
        if (sent == 0) { rte_pktmbuf_free(rst); return; }
    }
    worker_metrics_add_tcp_reset_sent(worker_idx);
}

/* ── HTTP keep-alive transaction helper ───────────────────────────────────
 * After an HTTP response is received on a keep-alive connection, decide
 * whether to send the next request (state 4), enter think-time (state 7),
 * or close the connection.  Returns the new app_state. */
static inline uint8_t
http_next_txn(uint32_t worker_idx, tcb_t *tcb)
{
    http_prebuilt_req_t *hpb = (http_prebuilt_req_t *)tcb->app_ctx;
    if (!hpb || !hpb->keep_alive) return 0; /* no keep-alive → close */

    tcb->http_txn_count++;

    /* Check transaction limit */
    if (hpb->txn_per_conn > 0 && tcb->http_txn_count >= hpb->txn_per_conn)
        return 0; /* limit reached → close */

    /* Think-time delay before next request */
    if (hpb->think_time_us > 0) {
        tcb->think_deadline_tsc = rte_rdtsc() +
            (uint64_t)hpb->think_time_us * g_tsc_hz / 1000000ULL;
        tcp_timer_resched(worker_idx, tcb);
        return 7; /* think-time wait */
    }

    return 4; /* send next request immediately */
}

/* ── Send next HTTP request on a keep-alive connection ────────────────────
 * Handles both plain HTTP and HTTPS (encrypts if TLS session exists).
 * Sets app_state = 5 (response pending) and records sent timestamp. */
static void
http_send_next_request(uint32_t worker_idx, tcb_t *tcb)
{
    http_prebuilt_req_t *hp = (http_prebuilt_req_t *)tcb->app_ctx;
    if (!hp || hp->hdr_len == 0) return;

    uint32_t ci = (uint32_t)(tcb - g_tcb_stores[worker_idx].tcbs);
    tls_session_t *ts = tls_session_get(worker_idx, ci);
    if (ts) {
        uint8_t ct_buf[4096];
        int ct_len = tls_encrypt(ts, hp->hdr, hp->hdr_len,
                                  ct_buf, sizeof(ct_buf));
        if (ct_len > 0)
            tcp_fsm_send(worker_idx, tcb, ct_buf, (uint32_t)ct_len);
        worker_metrics_add_tls_tx(worker_idx);
    } else {
        tcp_fsm_send(worker_idx, tcb, hp->hdr, hp->hdr_len);
    }
    worker_metrics_add_http_req(worker_idx);
    tcb->http_req_sent_tsc = rte_rdtsc();
    tcb->app_state = 5; /* HTTP response pending */
    tcp_timer_resched(worker_idx, tcb);
}

/* ── Effective MSS helper ─────────────────────────────────────────────────── */
static inline uint32_t
tcb_effective_mss(const tcb_t *tcb)
{
    uint32_t opts = tcb->ts_enabled ? 12 : 0;
    return (tcb->mss_remote > opts) ? tcb->mss_remote - opts : 1;
}

/* ── Send-buffer drain: transmit unsent data within the current window ─── */
static void
snd_buf_drain(uint32_t worker_idx, tcb_t *tcb)
{
    tcp_snd_buf_t *sb = tcb->snd_buf;
    if (!sb || sb->len == 0) return;

    uint32_t eff_mss = tcb_effective_mss(tcb);
    uint32_t offset  = tcb->snd_nxt - tcb->snd_una;

    while (offset < sb->len) {
        uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;
        uint32_t wnd   = TGEN_MIN(tcb->cwnd, tcb->snd_wnd);
        uint32_t avail = (wnd > in_flight) ? wnd - in_flight : 0;
        uint32_t unsent = sb->len - offset;
        uint32_t send_len = TGEN_MIN(unsent, avail);
        if (send_len == 0) break;
        send_len = TGEN_MIN(send_len, eff_mss);

        int rc = tcp_send_segment(worker_idx, tcb,
                     RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG,
                     sb->data + offset, send_len,
                     tcb->snd_nxt, tcb->rcv_nxt);
        if (rc < 0) break;
        tcb->snd_nxt += send_len;
        offset       += send_len;
        worker_metrics_add_tcp_payload_tx(worker_idx, send_len);
    }
}

/* ── FSM: input ──────────────────────────────────────────────────────────── */
void tcp_fsm_input(uint32_t worker_idx, struct rte_mbuf *m)
{
    if (m->data_len < sizeof(struct rte_tcp_hdr)) goto bad;

    const struct rte_tcp_hdr *tcp =
        rte_pktmbuf_mtod(m, const struct rte_tcp_hdr *);

    /* Detect IPv6 via version marker saved by ipv6_input() */
    bool is_input_v6 = (m->hash.usr == 6);
    uint32_t src_ip = 0, dst_ip = 0;
    if (!is_input_v6) {
        src_ip = m->hash.usr;      /* saved by ipv4_input (network order) */
        dst_ip = (uint32_t)m->dynfield1[0];  /* saved by ipv4_input (network order) */
    }
    /* For IPv6, t_saved_src6/t_saved_dst6 are set by ipv6_input() */
    uint16_t src_port = rte_be_to_cpu_16(tcp->src_port);
    uint16_t dst_port = rte_be_to_cpu_16(tcp->dst_port);

    /* TCP checksum verification.
     * Hardware offload: drop on explicit BAD; GOOD passes without SW verify.
     * Software PMDs (AF_PACKET, TAP): skip SW verification — the Linux kernel
     * uses TX checksum offload on veth/tap so received partial checksums look
     * invalid even for correct packets.  A traffic generator trusts the test
     * network; HW BAD-flag detection is sufficient for real NICs. */
    if (m->ol_flags & RTE_MBUF_F_RX_L4_CKSUM_BAD) {
        worker_metrics_add_tcp_bad_cksum(worker_idx);
        goto bad;
    }

    tcb_store_t *store = &g_tcb_stores[worker_idx];

    /* Try to find existing TCB (look up as "our" connection — swap src/dst) */
    tcb_t *tcb = tcb_lookup(store, is_input_v6 ? 0 : dst_ip, dst_port,
                             is_input_v6 ? 0 : src_ip, src_port);
    /* For IPv6, do a secondary match check on the IP addresses */
    if (is_input_v6 && tcb && tcb->ip_version == 6) {
        if (memcmp(tcb->src_ip6, t_saved_dst6, 16) != 0 ||
            memcmp(tcb->dst_ip6, t_saved_src6, 16) != 0)
            tcb = NULL; /* 4-tuple mismatch */
    }


    uint8_t flags = tcp->tcp_flags;

    /* Parse TCP options */
    tcp_parsed_opts_t opts;
    tcp_options_parse(tcp, &opts);

    uint32_t seq = rte_be_to_cpu_32(tcp->sent_seq);
    uint32_t ack = rte_be_to_cpu_32(tcp->recv_ack);

    if (!tcb) {
        /* SYN → passive open */
        if ((flags & RTE_TCP_SYN_FLAG) && !(flags & RTE_TCP_ACK_FLAG)) {
            /* In server mode, only accept SYNs on ports with listeners */
            srv_handler_t srv_h = SRV_HANDLER_NONE;
            if (g_srv_tables[worker_idx].serving) {
                srv_h = srv_lookup_port(worker_idx, dst_port);
                if (srv_h == SRV_HANDLER_NONE)
                    goto done; /* no listener on this port — drop */
            }
            tcb = tcb_alloc(store, dst_ip, dst_port, src_ip, src_port);
            if (!tcb) { worker_metrics_add_syn_queue_drops(worker_idx); goto bad; }
            tcb->state         = TCP_SYN_RECEIVED;
            tcb->rcv_nxt       = seq + 1;
            tcb->snd_nxt       = isn_generate(dst_ip, dst_port, src_ip, src_port);
            tcb->snd_una       = tcb->snd_nxt;
            tcb->mss_remote    = opts.has_mss ? opts.mss : 536;
            tcb->mss_local     = is_input_v6 ? 1440 : 1460; /* IPv6 header is 20 bytes larger */
            tcb->wscale_remote = opts.has_wscale ? opts.wscale : 0;
            tcb->wscale_local  = 7;
            tcb->rcv_wnd       = 65535 << tcb->wscale_local;
            tcb->snd_wnd       = 65535;
            /* Traffic generator: allow full-window initial burst.
             * No send buffer, so data beyond cwnd is lost. */
            tcb->cwnd          = 65535;
            tcb->ssthresh      = UINT32_MAX;
            tcb->sack_enabled  = opts.has_sack_perm;
            tcb->ts_enabled    = opts.has_timestamps;
            tcb->ts_ecr        = opts.ts_val;
            tcb->nagle_enabled = true;
            tcb->lcore_id      = (uint8_t)rte_lcore_id();
            tcb->port_id       = m->port;
            tcb->rto_us        = TCP_INITIAL_RTO_US;
            /* IPv6 address fields */
            if (is_input_v6) {
                tcb->ip_version = 6;
                memcpy(tcb->src_ip6, t_saved_dst6, 16);
                memcpy(tcb->dst_ip6, t_saved_src6, 16);
            } else {
                tcb->ip_version = 4;
            }
            /* Pre-resolve destination MAC for passive open */
            {
                struct rte_ether_addr pmac;
                bool resolved = false;
                if (is_input_v6) {
                    resolved = ndp_lookup(m->port, t_saved_src6, &pmac);
                } else {
                    resolved = arp_lookup(m->port, arp_nexthop(m->port, src_ip), &pmac);
                }
                if (resolved) {
                    rte_ether_addr_copy(&pmac, &tcb->dst_mac);
                    tcb->dst_mac_valid = true;
                }
            }

            /* Send SYN-ACK */
            tcp_send_segment(worker_idx, tcb,
                              RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG,
                              NULL, 0,
                              tcb->snd_nxt, tcb->rcv_nxt);
            tcb->snd_nxt++;
            arm_rto(worker_idx, tcb);
            worker_metrics_add_tcp_conn_open(worker_idx);
        } else if (!(flags & RTE_TCP_RST_FLAG)) {
            /* Stale packets from recently-closed connections: drop silently.
             * Sending RST-no-TCB per RFC 793 §3.4 causes a RST storm that
             * overwhelms the server — each stale data-ACK or FIN generates
             * an RST, which causes more server-side error handling, which
             * delays HTTP responses, creating more stale packets.
             * For a traffic generator, silent drop is the right trade-off. */
        }
        goto done;
    }

    /* ── Existing TCB ─────────────────────────────────────────────────────── */
    switch (tcb->state) {
    case TCP_SYN_SENT:
        if ((flags & RTE_TCP_SYN_FLAG) && (flags & RTE_TCP_ACK_FLAG)) {
            if (ack != tcb->snd_nxt) { tcp_fsm_reset(worker_idx, tcb); goto done; }
            tcb->rcv_nxt       = seq + 1;
            tcb->snd_una       = ack;
            tcb->mss_remote    = opts.has_mss ? opts.mss : 536;
            tcb->wscale_remote = opts.has_wscale ? opts.wscale : 0;
            tcb->sack_enabled  = opts.has_sack_perm;
            tcb->ts_enabled    = opts.has_timestamps;
            tcb->ts_ecr        = opts.ts_val;
            /* RFC 7323 §2.2: window in SYN-ACK is NOT scaled */
            tcb->snd_wnd       = rte_be_to_cpu_16(tcp->rx_win);
            /* Traffic generator: allow full-window initial burst */
            tcb->cwnd          = rte_be_to_cpu_16(tcp->rx_win);
            tcb->state = TCP_ESTABLISHED;
            tcb->retransmit_count = 0;
            tcb->rto_deadline_tsc = 0;  /* disarm SYN RTO */
            tcp_timer_resched(worker_idx, tcb);
            /* Measure RTT from SYN round-trip using timestamp echo.
             * Without this, rto_us stays at the aggressive TCP_INITIAL_RTO_US
             * (10ms), causing spurious retransmissions when the first data
             * segments are sent before the server's delayed ACK arrives. */
            if (opts.has_timestamps) {
                uint32_t ts_now = (uint32_t)(rte_rdtsc() /
                                             (g_tsc_hz / 1000000));
                uint32_t rtt_us = ts_now - opts.ts_ecr;
                if (rtt_us > 0 && rtt_us < 60000000U)
                    update_rtt(tcb, rtt_us);
                else
                    tcb->rto_us = TCP_INITIAL_RTO_US;
            } else {
                tcb->rto_us = TCP_INITIAL_RTO_US;
            }
            worker_metrics_add_tcp_conn_open(worker_idx);
            /* Send ACK */
            tcp_send_segment(worker_idx, tcb,
                              RTE_TCP_ACK_FLAG,
                              NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);

            /* ── SYN-only mode: 3WHS complete → close & recycle ──── */
            if (tcb->app_state == 0 && tcb->app_ctx == NULL) {
                if (tcb->graceful_close)
                    tcp_fsm_close(worker_idx, tcb);
                else
                    tcp_fsm_reset(worker_idx, tcb);
                goto done;
            }

            /* ── Throughput mode: bypass slow start ────────────────── */
            /* Traffic generators measure link/NIC throughput, not
             * congestion control convergence.  Set cwnd = MAX so
             * the receiver's advertised window is the only limit. */
            if (tcb->app_ctx == (void *)1)
                tcb->cwnd = UINT32_MAX;

            /* ── TLS handshake initiation ─────────────────────────── */
            if (tcb->app_state == 1) { /* TLS requested */
                uint32_t conn_idx = (uint32_t)(tcb - g_tcb_stores[worker_idx].tcbs);
                int trc = tls_session_attach(worker_idx, conn_idx, false, NULL);
                if (trc == 0) {
                    tcb->app_state = 2; /* TLS handshaking */
                    tcb->tls_hs_start_tsc = rte_rdtsc();
                    tls_session_t *sess = tls_session_get(worker_idx, conn_idx);
                    if (sess) {
                        uint8_t tls_out[4096];
                        size_t  tls_out_len = sizeof(tls_out);
                        int hr = tls_handshake(sess, NULL, 0, tls_out, &tls_out_len);
                        if (tls_out_len > 0)
                            tcp_fsm_send(worker_idx, tcb, tls_out, (uint32_t)tls_out_len);
                        if (hr == 1) {
                            tcb->app_state = 3; /* TLS established */
                            worker_metrics_add_tls_ok(worker_idx);
                            uint64_t lat_us = (rte_rdtsc() - tcb->tls_hs_start_tsc)
                                              * 1000000ULL / rte_get_tsc_hz();
                            hist_record(&g_latency_hist[worker_idx], lat_us);
                            /* If HTTPS: encrypt & send HTTP request now that TLS is up.
                             * Throughput mode sets app_ctx=(void*)1 as marker — skip HTTP send. */
                            if (tcb->app_ctx && (uintptr_t)tcb->app_ctx > 0x1000) {
                                http_prebuilt_req_t *hp_req =
                                    (http_prebuilt_req_t *)tcb->app_ctx;
                                if (hp_req->hdr_len > 0) {
                                    uint8_t ct_buf[4096];
                                    int ct_len = tls_encrypt(sess,
                                        hp_req->hdr, hp_req->hdr_len,
                                        ct_buf, sizeof(ct_buf));
                                    if (ct_len > 0)
                                        tcp_fsm_send(worker_idx, tcb,
                                                     ct_buf, (uint32_t)ct_len);
                                    worker_metrics_add_http_req(worker_idx);
                                    tcb->http_req_sent_tsc = rte_rdtsc();
                                    tcb->app_state = 5;
                                }
                            }
                        } else if (hr < 0) {
                            tcb->app_state = 0;
                            worker_metrics_add_tls_fail(worker_idx);
                        }
                    }
                } else {
                    tcb->app_state = 0;
                    worker_metrics_add_tls_fail(worker_idx);
                    TGEN_ERR(TGEN_LOG_TLS, "tls_session_attach failed: %d\n", trc);
                }
            }

            /* ── HTTP request initiation (plain HTTP only) ────────── */
            if (tcb->app_state == 4) {
                http_prebuilt_req_t *hp =
                    (http_prebuilt_req_t *)tcb->app_ctx;
                if (hp && hp->hdr_len > 0) {
                    tcp_fsm_send(worker_idx, tcb,
                                 hp->hdr, hp->hdr_len);
                    worker_metrics_add_http_req(worker_idx);
                    tcb->http_req_sent_tsc = rte_rdtsc();
                    tcb->app_state = 5; /* HTTP response pending */
                }
            }
        } else if ((flags & RTE_TCP_ACK_FLAG) && !(flags & RTE_TCP_RST_FLAG) &&
                   ack == tcb->snd_nxt) {
            /* RFC 5961 §4: challenge ACK — server already has a connection
             * for this 4-tuple (stale ESTABLISHED from a prior test run).
             * Per RFC 5961 §4.2: respond with RST (seq = SEG.ACK = snd_nxt)
             * to attempt clearing the stale connection, then fail fast.
             * The next --one attempt will use a fresh ephemeral port (cursor
             * already advanced), avoiding the 4-tuple collision. */
            TGEN_WARN(TGEN_LOG_SYN,
                "challenge ACK from %08x:%u — stale server conn, sending RST\n",
                rte_be_to_cpu_32(tcb->dst_ip), tcb->dst_port);
            tcp_fsm_reset(worker_idx, tcb);
            goto done;
        }
        break;

    case TCP_SYN_RECEIVED:
        if ((flags & RTE_TCP_ACK_FLAG) && seq == tcb->rcv_nxt) {
            tcb->snd_una = ack;
            tcb->state   = TCP_ESTABLISHED;
            tcb->snd_wnd = rte_be_to_cpu_16(tcp->rx_win) << tcb->wscale_remote;
            worker_metrics_add_tcp_conn_open(worker_idx);
            /* Server mode: notify handler that connection is established */
            if (g_srv_tables[worker_idx].serving) {
                srv_on_established(worker_idx, tcb, tcb->src_port);
            }
        }
        break;

    case TCP_ESTABLISHED: {
        /* Update timestamp echo (RFC 7323 §4.3.1) */
        if (opts.has_timestamps && tcb->ts_enabled)
            tcb->ts_ecr = opts.ts_val;

        /* ACK processing */
        if (flags & RTE_TCP_ACK_FLAG) {
            if (SEQ_GT(ack, tcb->snd_una)) {
                uint32_t acked = ack - tcb->snd_una;
                tcb->snd_una   = ack;
                /* After go-back-N, cumulative ACK may jump past snd_nxt */
                if (SEQ_GT(tcb->snd_una, tcb->snd_nxt))
                    tcb->snd_nxt = tcb->snd_una;
                tcb->dup_ack_count = 0;
                congestion_on_ack(tcb, acked);
                /* RFC 6298: on new ACK */
                tcb->retransmit_count = 0;
                /* Trim ACKed data from send buffer */
                if (tcb->snd_buf)
                    tcp_snd_buf_ack(tcb->snd_buf, acked);
                if (tcb->snd_una == tcb->snd_nxt) {
                    /* All data acknowledged — disarm RTO */
                    tcb->rto_deadline_tsc = 0;
                    tcp_timer_resched(worker_idx, tcb);
                } else {
                    /* Still unacked data — restart RTO from now */
                    arm_rto(worker_idx, tcb);
                }
                /* RTT measurement from timestamps */
                if (opts.has_timestamps && tcb->ts_enabled) {
                    uint32_t ts_now = (uint32_t)(rte_rdtsc() / (g_tsc_hz/1000000));
                    uint32_t rtt_us = ts_now - opts.ts_ecr;
                    if (rtt_us < 60000000U)
                        update_rtt(tcb, rtt_us);
                }
                /* Drain queued unsent data now that window opened */
                snd_buf_drain(worker_idx, tcb);
                /* Pump more chunked response data if streaming */
                if (tcb->app_state == 12)
                    srv_stream_pump(worker_idx, tcb);
            } else if (ack == tcb->snd_una) {
                tcb->dup_ack_count++;
                if (tcb->dup_ack_count == 3) {
                    congestion_fast_retransmit(worker_idx, tcb);
                    /* Retransmit oldest unACKed segment from send buffer
                     * (RFC 5681 §3.2: retransmit what appears to be lost) */
                    if (tcb->snd_buf && tcb->snd_buf->len > 0) {
                        uint32_t rtx_mss = tcb_effective_mss(tcb);
                        uint32_t rtx_len = TGEN_MIN(tcb->snd_buf->len, rtx_mss);
                        tcp_send_segment(worker_idx, tcb,
                                         RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG,
                                         tcb->snd_buf->data, rtx_len,
                                         tcb->snd_una, tcb->rcv_nxt);
                        worker_metrics_add_tcp_retransmit(worker_idx);
                    }
                }
            }
            /* Update snd_wnd */
            tcb->snd_wnd = rte_be_to_cpu_16(tcp->rx_win) << tcb->wscale_remote;
        }

        /* Data */
        uint8_t doff   = (tcp->data_off >> 4) & 0x0F;
        uint16_t tcp_len = (uint16_t)m->data_len;
        uint16_t hdr_len = (uint16_t)(doff * 4);
        if (tcp_len > hdr_len) {
            uint32_t data_len = tcp_len - hdr_len;
            if (seq == tcb->rcv_nxt) {
                tcb->rcv_nxt += data_len;
                /* Defer ACK */
                tcb->pending_ack     = true;
                tcb->delayed_ack_tsc = rte_rdtsc() +
                    TCP_DELAYED_ACK_US * g_tsc_hz / 1000000ULL;
                tcp_timer_dack_add(worker_idx, tcb);
                worker_metrics_add_tcp_payload_rx(worker_idx, data_len);

                /* ── Server mode: dispatch to handler ────────────── */
                if (tcb->app_state >= 10) {
                    const uint8_t *payload_data =
                        (const uint8_t *)tcp + hdr_len;
                    srv_on_data(worker_idx, tcb,
                                payload_data, data_len,
                                tcb->src_port);
                    /* Immediate ACK for server handlers */
                    tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                                     NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
                    tcb->pending_ack = false;
                } else
                /* ── L7: TLS handshake / decrypt ──────────────────── */
                if (tcb->app_state == 2 || tcb->app_state == 3 ||
                    tcb->app_state == 5 || tcb->app_state == 6) {
                    uint32_t ci = (uint32_t)(tcb - g_tcb_stores[worker_idx].tcbs);
                    tls_session_t *ts = tls_session_get(worker_idx, ci);
                    if (ts) {
                        const uint8_t *payload_data =
                            (const uint8_t *)tcp + hdr_len;
                        if (tcb->app_state == 2) {
                            /* TLS handshake in progress */
                            uint8_t tls_out[4096];
                            size_t  tls_out_len = sizeof(tls_out);
                            int hr = tls_handshake(ts, payload_data, data_len,
                                                   tls_out, &tls_out_len);
                            if (tls_out_len > 0) {
                                tcp_fsm_send(worker_idx, tcb,
                                             tls_out, (uint32_t)tls_out_len);
                                worker_metrics_add_tls_tx(worker_idx);
                            }
                            if (hr == 1) {
                                tcb->app_state = 3;
                                worker_metrics_add_tls_ok(worker_idx);
                                uint64_t lat_us = (rte_rdtsc() - tcb->tls_hs_start_tsc)
                                                  * 1000000ULL / rte_get_tsc_hz();
                                hist_record(&g_latency_hist[worker_idx], lat_us);
                                /* If HTTPS: encrypt & send HTTP request now that TLS is up.
                                 * Throughput mode sets app_ctx=(void*)1 as marker — skip HTTP send. */
                                if (tcb->app_ctx && (uintptr_t)tcb->app_ctx > 0x1000) {
                                    http_prebuilt_req_t *hp_req =
                                        (http_prebuilt_req_t *)tcb->app_ctx;
                                    if (hp_req->hdr_len > 0) {
                                        uint8_t ct_buf[4096];
                                        int ct_len = tls_encrypt(ts,
                                            hp_req->hdr, hp_req->hdr_len,
                                            ct_buf, sizeof(ct_buf));
                                        if (ct_len > 0)
                                            tcp_fsm_send(worker_idx, tcb,
                                                         ct_buf, (uint32_t)ct_len);
                                        worker_metrics_add_http_req(worker_idx);
                                        tcb->http_req_sent_tsc = rte_rdtsc();
                                        tcb->app_state = 5; /* HTTP response pending */
                                    }
                                } else if (!tcb->app_ctx) {
                                    /* TLS-only mode: send close_notify then FIN. */
                                    if (tcb->graceful_close) {
                                        uint8_t cl_buf[64];
                                        size_t cl_len = 0;
                                        tls_shutdown(ts, cl_buf,
                                                     sizeof(cl_buf),
                                                     &cl_len);
                                        if (cl_len > 0)
                                            tcp_fsm_send(worker_idx, tcb,
                                                         cl_buf,
                                                         (uint32_t)cl_len);
                                        tcp_fsm_close(worker_idx, tcb);
                                    } else {
                                        tcp_fsm_reset(worker_idx, tcb);
                                    }
                                    goto done;
                                }
                                /* else: throughput mode marker — keep connection open,
                                 * tx_gen_burst phase 1 will pump data */
                            } else if (hr < 0) {
                                tcb->app_state = 0;
                                worker_metrics_add_tls_fail(worker_idx);
                            }
                        } else {
                            /* TLS established — decrypt incoming data */
                            uint8_t plain[4096];
                            int n = tls_decrypt(ts, payload_data, data_len,
                                                plain, sizeof(plain));
                            if (n > 0) {
                                worker_metrics_add_tls_rx(worker_idx);
                                /* Parse decrypted HTTP response if waiting */
                                if (tcb->app_state == 5 && n >= 12 &&
                                    memcmp(plain, "HTTP/1.", 7) == 0) {
                                    uint16_t status =
                                        (uint16_t)atoi((const char *)plain + 9);
                                    worker_metrics_add_http_rsp(worker_idx, status);
                                    /* Record HTTPS request→response latency (CO-corrected) */
                                    if (tcb->http_req_sent_tsc) {
                                        uint64_t lat_us =
                                            (rte_rdtsc() - tcb->http_req_sent_tsc)
                                            * 1000000ULL / rte_get_tsc_hz();
                                        http_prebuilt_req_t *hlat =
                                            (http_prebuilt_req_t *)tcb->app_ctx;
                                        uint64_t ei = hlat ? hlat->expected_interval_us : 0;
                                        hist_record_corrected(&g_latency_hist[worker_idx],
                                                              lat_us, ei);
                                    }
                                    uint8_t nxt = http_next_txn(worker_idx, tcb);
                                    if (nxt == 4) {
                                        http_send_next_request(worker_idx, tcb);
                                    } else if (nxt == 7) {
                                        tcb->app_state = 7; /* think-time wait */
                                    } else if (!tcb->graceful_close) {
                                        tcp_fsm_reset(worker_idx, tcb);
                                        goto done;
                                    } else {
                                        /* graceful_close: parse Content-Length
                                         * and close after full body received. */
                                        if (http_rsp_body_start(worker_idx, tcb,
                                                plain, (uint32_t)n))
                                            goto done;
                                    }
                                } else if (tcb->app_state == 6) {
                                    /* Accumulating response body in TLS stream */
                                    if (http_rsp_body_recv(worker_idx, tcb,
                                                           (uint32_t)n))
                                        goto done;
                                }
                            }
                        }
                    }
                }

                /* ── L7: HTTP response parsing + body accumulation (plain HTTP only) ── */
                if (tcb->app_state == 5) {
                    uint32_t ci2 = (uint32_t)(tcb - g_tcb_stores[worker_idx].tcbs);
                    tls_session_t *ts2 = tls_session_get(worker_idx, ci2);
                    if (!ts2) {
                        /* Plain HTTP — parse from raw TCP payload */
                        const uint8_t *hp =
                            (const uint8_t *)tcp + hdr_len;
                        if (data_len >= 12 &&
                            memcmp(hp, "HTTP/1.", 7) == 0) {
                            uint16_t status =
                                (uint16_t)atoi((const char *)hp + 9);
                            worker_metrics_add_http_rsp(worker_idx,
                                                        status);
                            /* Record HTTP request→response latency (CO-corrected) */
                            if (tcb->http_req_sent_tsc) {
                                uint64_t lat_us =
                                    (rte_rdtsc() - tcb->http_req_sent_tsc)
                                    * 1000000ULL / rte_get_tsc_hz();
                                http_prebuilt_req_t *hlat2 =
                                    (http_prebuilt_req_t *)tcb->app_ctx;
                                uint64_t ei2 = hlat2 ? hlat2->expected_interval_us : 0;
                                hist_record_corrected(&g_latency_hist[worker_idx],
                                                      lat_us, ei2);
                            }
                        } else {
                            worker_metrics_add_http_parse_err(
                                worker_idx);
                        }
                        uint8_t nxt2 = http_next_txn(worker_idx, tcb);
                        if (nxt2 == 4) {
                            http_send_next_request(worker_idx, tcb);
                        } else if (nxt2 == 7) {
                            tcb->app_state = 7; /* think-time wait */
                        } else if (!tcb->graceful_close) {
                            tcp_fsm_reset(worker_idx, tcb);
                            goto done;
                        } else {
                            /* graceful_close: parse Content-Length and
                             * send FIN only after the full body arrives.
                             * Falls back to passive close if no
                             * Content-Length header (chunked, etc.). */
                            if (http_rsp_body_start(worker_idx, tcb,
                                    (const uint8_t *)tcp + hdr_len,
                                    data_len))
                                goto done;
                        }
                    }
                    /* TLS HTTP responses handled above in decrypt path */
                } else if (tcb->app_state == 6) {
                    /* HTTP body accumulation — only for plain HTTP.
                     * TLS body is accumulated in the decrypt path above.
                     * Use else-if so this block does NOT fire on the same
                     * segment that transitions state 5→6 (which would
                     * double-count the body bytes already tallied by
                     * http_rsp_body_start). */
                    uint32_t ci3 = (uint32_t)(tcb - g_tcb_stores[worker_idx].tcbs);
                    tls_session_t *ts3 = tls_session_get(worker_idx, ci3);
                    if (!ts3) {
                        if (http_rsp_body_recv(worker_idx, tcb, data_len))
                            goto done;
                    }
                }
            }
        }

        /* FIN — only process if still in ESTABLISHED state.
         * L7 handlers above (HTTP/TLS) may have already transitioned
         * via tcp_fsm_reset()/tcp_fsm_close(), freeing the TCB. */
        if ((flags & RTE_TCP_FIN_FLAG) && tcb->state == TCP_ESTABLISHED) {
            tcb->rcv_nxt++;
            tcb->state = TCP_CLOSE_WAIT;
            tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                              NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
            /* Throughput mode: keep connection open in CLOSE_WAIT to continue
             * sending data (RFC 793: CLOSE_WAIT allows sending).
             * app_ctx == (void*)1 is the throughput marker set by tx_gen. */
            if (tcb->app_ctx != (void *)1)
                tcp_fsm_close(worker_idx, tcb);
        }
        break;
    }

    case TCP_FIN_WAIT_1: {
        bool fin_acked = false;
        if (flags & RTE_TCP_ACK_FLAG) {
            if (SEQ_GT(ack, tcb->snd_una)) {
                tcb->snd_una = ack;
                fin_acked = (tcb->snd_una == tcb->snd_nxt);
            }
        }
        /* Accept incoming data (half-open: remote can still send) */
        uint8_t fw1_doff   = (tcp->data_off >> 4) & 0x0F;
        uint16_t fw1_tlen  = (uint16_t)m->data_len;
        uint16_t fw1_hlen  = (uint16_t)(fw1_doff * 4);
        if (fw1_tlen > fw1_hlen) {
            uint32_t dlen = fw1_tlen - fw1_hlen;
            if (seq == tcb->rcv_nxt) {
                tcb->rcv_nxt += dlen;
                worker_metrics_add_tcp_payload_rx(worker_idx, dlen);
            }
        }
        if (flags & RTE_TCP_FIN_FLAG) {
            tcb->rcv_nxt++;
            tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                              NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
            if (fin_acked) {
                /* Our FIN was ACKed + peer's FIN received → free immediately.
                 * Traffic generators don't need 2×MSL TIME_WAIT protection;
                 * rapid port recycling is more important than guarding
                 * against duplicate segments from old connections. */
                tls_detach_if_needed(worker_idx, tcb);
                tcp_port_free_immediate(worker_idx, tcb->src_ip, tcb->src_port);
                tcb_free(&g_tcb_stores[worker_idx], tcb);
                worker_metrics_add_tcp_conn_close(worker_idx);
            } else {
                /* Simultaneous close: peer FIN but our FIN not yet ACKed → CLOSING */
                tcb->state = TCP_CLOSING;
                arm_rto(worker_idx, tcb);
            }
        } else if (fin_acked) {
            /* Our FIN ACKed but no peer FIN yet → FIN_WAIT_2 */
            tcb->state = TCP_FIN_WAIT_2;
            /* Arm idle timeout for FIN_WAIT_2 (RFC 9293 §3.6) */
            tcb->timewait_deadline_tsc = rte_rdtsc() +
                60ULL * rte_get_tsc_hz();
            tcp_timer_resched(worker_idx, tcb);
            /* ACK any data that arrived in the same segment */
            if (fw1_tlen > fw1_hlen)
                tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                                  NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
        } else if (fw1_tlen > fw1_hlen) {
            /* ACK the data even if no FIN yet */
            tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                              NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
        }
        break;
    }

    case TCP_FIN_WAIT_2: {
        /* Accept incoming data (half-open: remote can still send) */
        uint8_t fw2_doff   = (tcp->data_off >> 4) & 0x0F;
        uint16_t fw2_tlen  = (uint16_t)m->data_len;
        uint16_t fw2_hlen  = (uint16_t)(fw2_doff * 4);
        if (fw2_tlen > fw2_hlen) {
            uint32_t dlen = fw2_tlen - fw2_hlen;
            if (seq == tcb->rcv_nxt) {
                tcb->rcv_nxt += dlen;
                worker_metrics_add_tcp_payload_rx(worker_idx, dlen);
            }
        }
        if (flags & RTE_TCP_FIN_FLAG) {
            tcb->rcv_nxt++;
            tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                              NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
            /* Skip TIME_WAIT — free immediately for fast port recycling */
            tls_detach_if_needed(worker_idx, tcb);
            tcp_port_free_immediate(worker_idx, tcb->src_ip, tcb->src_port);
            tcb_free(&g_tcb_stores[worker_idx], tcb);
            worker_metrics_add_tcp_conn_close(worker_idx);
        } else if (fw2_tlen > fw2_hlen) {
            /* ACK the data even if no FIN yet */
            tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                              NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
        }
        break;
    }

    case TCP_CLOSING:
        /* Simultaneous close: waiting for ACK of our FIN */
        if ((flags & RTE_TCP_ACK_FLAG) && SEQ_GE(ack, tcb->snd_nxt)) {
            /* Skip TIME_WAIT — free immediately */
            tls_detach_if_needed(worker_idx, tcb);
            tcp_port_free_immediate(worker_idx, tcb->src_ip, tcb->src_port);
            tcb_free(&g_tcb_stores[worker_idx], tcb);
            worker_metrics_add_tcp_conn_close(worker_idx);
        }
        break;

    case TCP_CLOSE_WAIT:
        /* We can still send data in CLOSE_WAIT. Process ACKs for our data. */
        if (flags & RTE_TCP_ACK_FLAG) {
            if (SEQ_GT(ack, tcb->snd_una)) {
                uint32_t acked = ack - tcb->snd_una;
                tcb->snd_una = ack;
                /* After go-back-N, cumulative ACK may jump past snd_nxt */
                if (SEQ_GT(tcb->snd_una, tcb->snd_nxt))
                    tcb->snd_nxt = tcb->snd_una;
                tcb->dup_ack_count = 0;
                congestion_on_ack(tcb, acked);
                if (tcb->snd_buf)
                    tcp_snd_buf_ack(tcb->snd_buf, acked);
                tcb->retransmit_count = 0;
                if (tcb->snd_una == tcb->snd_nxt) {
                    tcb->rto_deadline_tsc = 0;
                    tcp_timer_resched(worker_idx, tcb);
                } else {
                    arm_rto(worker_idx, tcb);
                }
                snd_buf_drain(worker_idx, tcb);
            }
            tcb->snd_wnd = rte_be_to_cpu_16(tcp->rx_win)
                           << tcb->wscale_remote;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & RTE_TCP_ACK_FLAG) {
            tls_detach_if_needed(worker_idx, tcb);
            tcp_port_free(worker_idx, tcb->src_ip, tcb->src_port);
            tcb_free(&g_tcb_stores[worker_idx], tcb);
            worker_metrics_add_tcp_conn_close(worker_idx);
        }
        break;

    case TCP_TIME_WAIT:
        /* Ignore new segments; timer wheel handles expiry */
        break;

    default:
        break;
    }

    /* RST processing (RFC 9293 §3.5.2) — validate sequence is in window */
    if ((flags & RTE_TCP_RST_FLAG) && tcb->in_use &&
        tcb->state != TCP_TIME_WAIT) {
        bool rst_valid = false;
        if (tcb->state == TCP_SYN_SENT) {
            /* RFC 9293 §3.5.2: In SYN-SENT, RST is valid if ACK is acceptable */
            rst_valid = (flags & RTE_TCP_ACK_FLAG) && (ack == tcb->snd_nxt);
        } else {
            /* All other states: RST valid if SEQ is in receive window */
            rst_valid = SEQ_GE(seq, tcb->rcv_nxt) &&
                        SEQ_LT(seq, tcb->rcv_nxt + tcb->rcv_wnd);
        }
        if (rst_valid) {
            tls_detach_if_needed(worker_idx, tcb);
            tcp_port_free_immediate(worker_idx, tcb->src_ip, tcb->src_port);
            tcb_free(&g_tcb_stores[worker_idx], tcb);
            worker_metrics_add_tcp_reset_rx(worker_idx);
        }
        /* Invalid RST silently dropped (RFC 9293 §3.5.2) */
    }

done:
    rte_pktmbuf_free(m);
    return;
bad:
    rte_pktmbuf_free(m);
}

/* ── Active open ─────────────────────────────────────────────────────────── */
tcb_t *tcp_fsm_connect(uint32_t worker_idx,
                         uint32_t src_ip, uint16_t src_port,
                         uint32_t dst_ip, uint16_t dst_port,
                         uint16_t port_id)
{

    /* Auto-allocate ephemeral port if caller passes 0 */
    if (src_port == 0) {
        if (tcp_port_alloc(worker_idx, src_ip, &src_port) < 0)
            return NULL;
    }

    tcb_store_t *store = &g_tcb_stores[worker_idx];
    tcb_t *tcb = tcb_alloc(store, src_ip, src_port, dst_ip, dst_port);
    if (!tcb) return NULL;

    tcb->state        = TCP_SYN_SENT;
    tcb->ip_version   = 4; /* default; caller sets to 6 + addresses for IPv6 */
    tcb->snd_nxt      = isn_generate(src_ip, src_port, dst_ip, dst_port);
    tcb->snd_una      = tcb->snd_nxt;
    tcb->rcv_wnd      = 65535 << 7;
    tcb->mss_local    = 1460;
    tcb->wscale_local = 7;
    /* RFC 5681 §3.1: IW before knowing peer MSS — use local MSS as estimate */
    tcb->cwnd         = TGEN_MIN(10u * tcb->mss_local,
                           TGEN_MAX(2u * tcb->mss_local, 4380u));
    tcb->ssthresh     = UINT32_MAX;
    tcb->nagle_enabled = true;
    tcb->rto_us       = TCP_INITIAL_RTO_US;
    tcb->lcore_id     = (uint8_t)rte_lcore_id();
    tcb->port_id      = port_id;
    tcb->active_open  = true;
    tcb->ts_enabled   = true;
    /* Pre-resolve destination MAC so all segments use cached value */
    {
        struct rte_ether_addr mac;
        if (arp_lookup(port_id, arp_nexthop(port_id, dst_ip), &mac)) {
            rte_ether_addr_copy(&mac, &tcb->dst_mac);
            tcb->dst_mac_valid = true;
        }
    }
    /* Store creation time for TLS handshake timeout.
     * Reuses timewait_deadline_tsc (only used in TIME_WAIT state). */
    tcb->timewait_deadline_tsc = rte_rdtsc();

    tcp_send_segment(worker_idx, tcb, RTE_TCP_SYN_FLAG,
                      NULL, 0, tcb->snd_nxt, 0);
    tcb->snd_nxt++;
    arm_rto(worker_idx, tcb);
    worker_metrics_add_tcp_syn_sent(worker_idx);
    return tcb;
}

/* ── Close ───────────────────────────────────────────────────────────────── */
int tcp_fsm_close(uint32_t worker_idx, tcb_t *tcb)
{
    if (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT)
        return -1;

    tcp_send_segment(worker_idx, tcb, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG,
                      NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
    tcb->snd_nxt++;

    tcb->state = (tcb->state == TCP_ESTABLISHED) ?
                  TCP_FIN_WAIT_1 : TCP_LAST_ACK;
    arm_rto(worker_idx, tcb);
    return 0;
}

/* ── Send RST ─────────────────────────────────────────────────────────────── */
void tcp_fsm_reset(uint32_t worker_idx, tcb_t *tcb)
{
    /* Count connection close if handshake was complete (matches conn_open) */
    if (tcb->state >= TCP_ESTABLISHED)
        worker_metrics_add_tcp_conn_close(worker_idx);

    tcp_send_segment(worker_idx, tcb, RTE_TCP_RST_FLAG | RTE_TCP_ACK_FLAG,
                      NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
    tls_detach_if_needed(worker_idx, tcb);
    /* RST teardown — no TIME_WAIT required (RFC 793 §3.4) */
    tcp_port_free_immediate(worker_idx, tcb->src_ip, tcb->src_port);
    tcb_free(&g_tcb_stores[worker_idx], tcb);
    worker_metrics_add_tcp_reset_sent(worker_idx);
}

/* ── RST all active connections and reset store ──────────────────────────── */
void tcp_fsm_reset_all(uint32_t worker_idx)
{
    tcb_store_t *store = &g_tcb_stores[worker_idx];
    for (uint32_t i = 0; i < store->capacity; i++) {
        tcb_t *tcb = &store->tcbs[i];
        if (!tcb->in_use) continue;
        tcp_send_segment(worker_idx, tcb, RTE_TCP_RST_FLAG | RTE_TCP_ACK_FLAG,
                          NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
        tls_detach_if_needed(worker_idx, tcb);
        if (tcb->snd_buf) {
            tcp_snd_buf_free(tcb->snd_buf);
            tcb->snd_buf = NULL;
        }
    }
    tcb_store_reset(store);
}

/* ── RTO expired ──────────────────────────────────────────────────────────── */
void tcp_fsm_rto_expired(uint32_t worker_idx, tcb_t *tcb)
{
    tcb->retransmit_count++;
    /* SYN_SENT: fail fast (3 retries) to free slots for new connections.
     * Other states: use the full TCP_MAX_RETRANSMITS. */
    uint32_t max_retries = (tcb->state == TCP_SYN_SENT) ? 3 : TCP_MAX_RETRANSMITS;
    if (tcb->retransmit_count > max_retries) {
        tcp_fsm_reset(worker_idx, tcb);
        return;
    }
    /* Backoff */
    tcb->rto_us = TGEN_MIN(tcb->rto_us * 2, (uint32_t)TCP_MAX_RTO_US);
    congestion_on_rto(tcb);
    /* Retransmit based on FSM state */
    switch (tcb->state) {
    case TCP_SYN_SENT:
        tcp_send_segment(worker_idx, tcb, RTE_TCP_SYN_FLAG,
                          NULL, 0, tcb->snd_una, 0);
        break;
    case TCP_SYN_RECEIVED:
        tcp_send_segment(worker_idx, tcb,
                          RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG,
                          NULL, 0, tcb->snd_una, tcb->rcv_nxt);
        break;
    case TCP_FIN_WAIT_1:
    case TCP_LAST_ACK:
        tcp_send_segment(worker_idx, tcb,
                          RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG,
                          NULL, 0, tcb->snd_una, tcb->rcv_nxt);
        break;
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        /* Go-back-N: reset snd_nxt to snd_una so snd_buf_drain() re-sends
         * from the first unACKed byte.  cwnd is already 1×MSS from
         * congestion_on_rto(), so drain sends exactly one segment. */
        if (tcb->snd_buf && tcb->snd_buf->len > 0) {
            tcb->snd_nxt = tcb->snd_una;
            snd_buf_drain(worker_idx, tcb);
        }
        break;
    default:
        break;
    }
    arm_rto(worker_idx, tcb);
    worker_metrics_add_tcp_retransmit(worker_idx);
}

/* tcp_fsm_flush_delayed_acks() removed — delayed ACK flushing is now
 * integrated into tcp_timer_tick() via the dack_head singly-linked list. */

/* ── Listen (passive open) ────────────────────────────────────────────────── */
int tcp_fsm_listen(uint32_t worker_idx, uint16_t local_port)
{
    (void)worker_idx; (void)local_port;
    /* Listener state is implicit: unknown TCB + SYN → accept */
    return 0;
}

/* ── Data send ────────────────────────────────────────────────────────────── */
int tcp_fsm_send(uint32_t worker_idx, tcb_t *tcb,
                  const uint8_t *data, uint32_t len)
{
    /* RFC 793: sending is valid in ESTABLISHED and CLOSE_WAIT
     * (peer closed their send direction, but we can still send). */
    if (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT)
        return -1;

    /* Throughput mode (app_ctx == 1): bypass send buffer for maximum PPS.
     * No retransmission needed — throughput tests measure raw NIC rate. */
    if (tcb->app_ctx == (void *)1) {
        uint32_t eff_mss = tcb_effective_mss(tcb);
        uint32_t total_sent = 0;
        while (total_sent < len) {
            uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;
            uint32_t wnd = TGEN_MIN(tcb->cwnd, tcb->snd_wnd);
            uint32_t avail = (wnd > in_flight) ? wnd - in_flight : 0;
            uint32_t send_len = TGEN_MIN(len - total_sent, avail);
            if (send_len == 0) break;
            send_len = TGEN_MIN(send_len, eff_mss);
            int rc = tcp_send_segment(worker_idx, tcb,
                              RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG,
                              data + total_sent, send_len,
                              tcb->snd_nxt, tcb->rcv_nxt);
            if (rc < 0) break;
            tcb->snd_nxt += send_len;
            total_sent += send_len;
            worker_metrics_add_tcp_payload_tx(worker_idx, send_len);
        }
        if (total_sent > 0 && tcb->rto_deadline_tsc == 0)
            arm_rto(worker_idx, tcb);
        return (int)total_sent;
    }

    /* Normal mode: queue all data in send buffer for retransmission. */
    if (!tcb->snd_buf) {
        uint32_t cap = TCP_SND_BUF_DEFAULT_CAP;
        if (len > cap) cap = len + 4096; /* ensure first send fits */
        tcb->snd_buf = tcp_snd_buf_alloc(cap);
        if (!tcb->snd_buf)
            return -1;
        tcb->snd_buf->base_seq = tcb->snd_una;
    }

    uint32_t queued = tcp_snd_buf_append(tcb->snd_buf, data, len);

    /* Drain: send unsent data from the buffer within the window */
    snd_buf_drain(worker_idx, tcb);

    /* RFC 6298: arm RTO if data is now in flight */
    if (tcb->snd_nxt != tcb->snd_una && tcb->rto_deadline_tsc == 0)
        arm_rto(worker_idx, tcb);

    return (int)queued;
}

/* ── Public wrapper for http_send_next_request (used by tcp_timer) ──────── */
void tcp_fsm_http_send_next(uint32_t worker_idx, tcb_t *tcb)
{
    http_send_next_request(worker_idx, tcb);
}
