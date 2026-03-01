/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP FSM implementation (§3.2, RFC 793 + RFC 7323 + RFC 5681).
 */
#include "tcp_fsm.h"
#include "tcp_tcb.h"
#include "tcp_options.h"
#include "tcp_port_pool.h"
#include "tcp_checksum.h"
#include "tcp_congestion.h"
#include "../net/ipv4.h"
#include "../net/arp.h"
#include "../net/ethernet.h"
#include "../port/port_init.h"
#include "../core/mempool.h"
#include "../core/core_assign.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"

#include <string.h>
#include <netinet/in.h>

#include <rte_tcp.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include <rte_byteorder.h>
#include <rte_log.h>

/* ── Sequence number arithmetic ──────────────────────────────────────────── */
#define SEQ_LT(a,b)   ((int32_t)((a)-(b)) <  0)
#define SEQ_LE(a,b)   ((int32_t)((a)-(b)) <= 0)
#define SEQ_GT(a,b)   ((int32_t)((a)-(b)) >  0)
#define SEQ_GE(a,b)   ((int32_t)((a)-(b)) >= 0)

/* ── Build and send a TCP segment ────────────────────────────────────────── */
static int tcp_send_segment(uint32_t worker_idx, tcb_t *tcb,
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
                       true, true, ts_val);
    } else {
        opts_len = tcp_options_write_data(opts, sizeof(opts),
                       tcb->ts_enabled, ts_val, tcb->ts_ecr,
                       tcb->sack_block_count ? tcb->sack_blocks : NULL,
                       tcb->sack_block_count);
    }
    if (opts_len < 0) opts_len = 0;

    size_t tcp_hdr_sz = sizeof(struct rte_tcp_hdr) + (size_t)opts_len;
    size_t seg_len    = tcp_hdr_sz + payload_len;

    char *buf = rte_pktmbuf_append(m, (uint16_t)(
        sizeof(struct rte_ether_hdr) +
        sizeof(struct rte_ipv4_hdr)  +
        seg_len));
    if (!buf) { rte_pktmbuf_free(m); return -1; }

    /* Ethernet header */
    uint16_t port_id = 0; /* TODO: route to correct port */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
    rte_ether_addr_copy(&g_port_caps[port_id].mac_addr, &eth->src_addr);
    /* Resolve destination MAC via ARP */
    struct rte_ether_addr dst_mac;
    if (!arp_lookup(port_id, tcb->dst_ip, &dst_mac))
        memset(dst_mac.addr_bytes, 0xFF, 6);
    rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* IPv4 header */
    struct rte_ipv4_hdr *ip =
        (struct rte_ipv4_hdr *)(buf + sizeof(struct rte_ether_hdr));
    ip->version_ihl   = RTE_IPV4_VHL_DEF;
    ip->type_of_service = 0;
    ip->total_length  = rte_cpu_to_be_16(
        (uint16_t)(sizeof(*ip) + seg_len));
    ip->packet_id     = 0;
    ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    ip->time_to_live  = 64;
    ip->next_proto_id = IPPROTO_TCP;
    ip->hdr_checksum  = 0;
    ip->src_addr      = tcb->src_ip;
    ip->dst_addr      = tcb->dst_ip;

    /* TCP header */
    struct rte_tcp_hdr *tcp_h =
        (struct rte_tcp_hdr *)((uint8_t *)ip + sizeof(*ip));
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

    /* Set L2/L3/L4 lengths and compute checksums (HW offload when available) */
    m->l2_len = sizeof(struct rte_ether_hdr);
    m->l3_len = sizeof(struct rte_ipv4_hdr);
    m->l4_len = (uint16_t)tcp_hdr_sz;
    tcp_checksum_set(m, ip, tcp_h,
                     g_port_caps[port_id].has_tcp_cksum_offload);

    m->port = port_id;

    /* Transmit */
    uint16_t tx_q = (uint16_t)worker_idx % g_port_caps[port_id].max_tx_queues;
    uint16_t sent = rte_eth_tx_burst(port_id, tx_q, &m, 1);

    if (sent == 0) { rte_pktmbuf_free(m); return -1; }
    worker_metrics_add_tx(worker_idx, 1, (uint32_t)seg_len);
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
static inline void arm_rto(tcb_t *tcb)
{
    tcb->rto_deadline_tsc = rte_rdtsc() +
                             (uint64_t)tcb->rto_us * g_tsc_hz / 1000000ULL;
}

/* ── FSM: input ──────────────────────────────────────────────────────────── */
void tcp_fsm_input(uint32_t worker_idx, struct rte_mbuf *m)
{
    if (m->data_len < sizeof(struct rte_tcp_hdr)) goto bad;

    const struct rte_tcp_hdr *tcp =
        rte_pktmbuf_mtod(m, const struct rte_tcp_hdr *);

    uint32_t src_ip   = m->hash.usr;      /* saved by ipv4_input (network order) */
    uint32_t dst_ip   = m->dynfield1[0];  /* saved by ipv4_input (network order) */
    uint16_t src_port = rte_be_to_cpu_16(tcp->src_port);
    uint16_t dst_port = rte_be_to_cpu_16(tcp->dst_port);

    tcb_store_t *store = &g_tcb_stores[worker_idx];

    /* Try to find existing TCB (look up as "our" connection — swap src/dst) */
    tcb_t *tcb = tcb_lookup(store, dst_ip, dst_port, src_ip, src_port);

    uint8_t flags = tcp->tcp_flags;

    /* Parse TCP options */
    tcp_parsed_opts_t opts;
    tcp_options_parse(tcp, &opts);

    uint32_t seq = rte_be_to_cpu_32(tcp->sent_seq);
    uint32_t ack = rte_be_to_cpu_32(tcp->recv_ack);

    if (!tcb) {
        /* SYN → passive open */
        if ((flags & RTE_TCP_SYN_FLAG) && !(flags & RTE_TCP_ACK_FLAG)) {
            tcb = tcb_alloc(store, dst_ip, dst_port, src_ip, src_port);
            if (!tcb) { worker_metrics_add_syn_queue_drops(worker_idx); goto bad; }
            tcb->state         = TCP_SYN_RECEIVED;
            tcb->rcv_nxt       = seq + 1;
            tcb->snd_nxt       = (uint32_t)rte_rdtsc(); /* ISN */
            tcb->snd_una       = tcb->snd_nxt;
            tcb->mss_remote    = opts.has_mss ? opts.mss : 536;
            tcb->mss_local     = 1460;
            tcb->wscale_remote = opts.has_wscale ? opts.wscale : 0;
            tcb->wscale_local  = 7;
            tcb->rcv_wnd       = 65535 << tcb->wscale_local;
            tcb->snd_wnd       = 65535;
            tcb->cwnd          = 10 * tcb->mss_local;
            tcb->ssthresh      = UINT32_MAX;
            tcb->sack_enabled  = opts.has_sack_perm;
            tcb->ts_enabled    = opts.has_timestamps;
            tcb->ts_ecr        = opts.ts_val;
            tcb->nagle_enabled = true;
            tcb->lcore_id      = (uint8_t)rte_lcore_id();
            tcb->rto_us        = TCP_INITIAL_RTO_US;

            /* Send SYN-ACK */
            tcp_send_segment(worker_idx, tcb,
                              RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG,
                              NULL, 0,
                              tcb->snd_nxt, tcb->rcv_nxt);
            tcb->snd_nxt++;
            arm_rto(tcb);
            worker_metrics_add_tcp_conn_open(worker_idx);
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
            tcb->snd_wnd       = rte_be_to_cpu_16(tcp->rx_win)
                                  << tcb->wscale_remote;
            tcb->state = TCP_ESTABLISHED;
            tcb->retransmit_count = 0;
            tcb->rto_us = TCP_INITIAL_RTO_US;
            tcb->rto_deadline_tsc = 0;  /* disarm SYN RTO */
            worker_metrics_add_tcp_conn_open(worker_idx);
            /* Send ACK */
            tcp_send_segment(worker_idx, tcb,
                              RTE_TCP_ACK_FLAG,
                              NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
        }
        break;

    case TCP_SYN_RECEIVED:
        if ((flags & RTE_TCP_ACK_FLAG) && seq == tcb->rcv_nxt) {
            tcb->snd_una = ack;
            tcb->state   = TCP_ESTABLISHED;
            tcb->snd_wnd = rte_be_to_cpu_16(tcp->rx_win) << tcb->wscale_remote;
            worker_metrics_add_tcp_conn_open(worker_idx);
        }
        break;

    case TCP_ESTABLISHED: {
        /* ACK processing */
        if (flags & RTE_TCP_ACK_FLAG) {
            if (SEQ_GT(ack, tcb->snd_una)) {
                uint32_t acked = ack - tcb->snd_una;
                tcb->snd_una   = ack;
                tcb->dup_ack_count = 0;
                congestion_on_ack(tcb, acked);
                /* RFC 6298: on new ACK */
                tcb->retransmit_count = 0;
                if (tcb->snd_una == tcb->snd_nxt) {
                    /* All data acknowledged — disarm RTO */
                    tcb->rto_deadline_tsc = 0;
                } else {
                    /* Still unacked data — restart RTO from now */
                    arm_rto(tcb);
                }
                /* RTT measurement from timestamps */
                if (opts.has_timestamps && tcb->ts_enabled) {
                    uint32_t ts_now = (uint32_t)(rte_rdtsc() / (g_tsc_hz/1000000));
                    uint32_t rtt_us = ts_now - opts.ts_ecr;
                    if (rtt_us < 60000000U)
                        update_rtt(tcb, rtt_us);
                }
            } else if (ack == tcb->snd_una) {
                tcb->dup_ack_count++;
                if (tcb->dup_ack_count == 3)
                    congestion_fast_retransmit(worker_idx, tcb);
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
                worker_metrics_add_tcp_payload_rx(worker_idx, data_len);
                /* TODO: pass data to L7 */
            }
        }

        /* FIN */
        if (flags & RTE_TCP_FIN_FLAG) {
            tcb->rcv_nxt++;
            tcb->state = TCP_CLOSE_WAIT;
            tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                              NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
        }
        break;
    }

    case TCP_FIN_WAIT_1: {
        if (flags & RTE_TCP_ACK_FLAG) tcb->state = TCP_FIN_WAIT_2;
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
            tcb->state = TCP_TIME_WAIT;
            tcb->timewait_deadline_tsc = rte_rdtsc() +
                (uint64_t)TGEN_TIMEWAIT_DEFAULT_MS * g_tsc_hz / 1000ULL;
            worker_metrics_add_tcp_conn_close(worker_idx);
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
            tcb->state = TCP_TIME_WAIT;
            tcb->timewait_deadline_tsc = rte_rdtsc() +
                (uint64_t)TGEN_TIMEWAIT_DEFAULT_MS * g_tsc_hz / 1000ULL;
            worker_metrics_add_tcp_conn_close(worker_idx);
        } else if (fw2_tlen > fw2_hlen) {
            /* ACK the data even if no FIN yet */
            tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                              NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
        }
        break;
    }

    case TCP_LAST_ACK:
        if (flags & RTE_TCP_ACK_FLAG) {
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

    /* RST processing — only for active connections (not TIME_WAIT or already freed) */
    if ((flags & RTE_TCP_RST_FLAG) && tcb->in_use &&
        tcb->state != TCP_TIME_WAIT) {
        tcb_free(&g_tcb_stores[worker_idx], tcb);
        worker_metrics_add_tcp_reset_rx(worker_idx);
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
    (void)port_id;

    /* Auto-allocate ephemeral port if caller passes 0 */
    if (src_port == 0) {
        if (tcp_port_alloc(worker_idx, src_ip, &src_port) < 0)
            return NULL;
    }

    tcb_store_t *store = &g_tcb_stores[worker_idx];
    tcb_t *tcb = tcb_alloc(store, src_ip, src_port, dst_ip, dst_port);
    if (!tcb) return NULL;

    tcb->state        = TCP_SYN_SENT;
    tcb->snd_nxt      = (uint32_t)rte_rdtsc();
    tcb->snd_una      = tcb->snd_nxt;
    tcb->rcv_wnd      = 65535 << 7;
    tcb->mss_local    = 1460;
    tcb->wscale_local = 7;
    tcb->cwnd         = 10 * tcb->mss_local;
    tcb->ssthresh     = UINT32_MAX;
    tcb->nagle_enabled = true;
    tcb->rto_us       = TCP_INITIAL_RTO_US;
    tcb->lcore_id     = (uint8_t)rte_lcore_id();
    tcb->active_open  = true;
    tcb->ts_enabled   = true;

    tcp_send_segment(worker_idx, tcb, RTE_TCP_SYN_FLAG,
                      NULL, 0, tcb->snd_nxt, 0);
    tcb->snd_nxt++;
    arm_rto(tcb);
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
    arm_rto(tcb);
    return 0;
}

/* ── Send RST ─────────────────────────────────────────────────────────────── */
void tcp_fsm_reset(uint32_t worker_idx, tcb_t *tcb)
{
    tcp_send_segment(worker_idx, tcb, RTE_TCP_RST_FLAG | RTE_TCP_ACK_FLAG,
                      NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
    tcb_free(&g_tcb_stores[worker_idx], tcb);
    worker_metrics_add_tcp_reset_sent(worker_idx);
}

/* ── RTO expired ──────────────────────────────────────────────────────────── */
void tcp_fsm_rto_expired(uint32_t worker_idx, tcb_t *tcb)
{
    tcb->retransmit_count++;
    if (tcb->retransmit_count > TCP_MAX_RETRANSMITS) {
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
    default:
        /* ESTABLISHED: would need TX buffer replay — not yet supported */
        break;
    }
    arm_rto(tcb);
    worker_metrics_add_tcp_retransmit(worker_idx);
}

/* ── Flush delayed ACKs ───────────────────────────────────────────────────── */
void tcp_fsm_flush_delayed_acks(uint32_t worker_idx)
{
    tcb_store_t *store = &g_tcb_stores[worker_idx];
    uint64_t now = rte_rdtsc();
    for (uint32_t i = 0; i < store->capacity; i++) {
        tcb_t *tcb = &store->tcbs[i];
        if (!tcb->in_use || !tcb->pending_ack) continue;
        if (now >= tcb->delayed_ack_tsc) {
            tcp_send_segment(worker_idx, tcb, RTE_TCP_ACK_FLAG,
                              NULL, 0, tcb->snd_nxt, tcb->rcv_nxt);
            tcb->pending_ack = false;
        }
    }
}

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
    if (tcb->state != TCP_ESTABLISHED) return -1;

    /* Flow control: limit by window minus in-flight data */
    uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;
    uint32_t wnd = TGEN_MIN(tcb->cwnd, tcb->snd_wnd);
    uint32_t avail = (wnd > in_flight) ? wnd - in_flight : 0;
    uint32_t send_len = TGEN_MIN(len, avail);
    if (send_len == 0) return 0;

    /* Cap payload by effective MSS (account for TCP options that reduce MTU space) */
    uint32_t opts_overhead = tcb->ts_enabled ? 12 : 0;
    uint32_t effective_mss = (tcb->mss_remote > opts_overhead)
                             ? tcb->mss_remote - opts_overhead : 1;
    send_len = TGEN_MIN(send_len, effective_mss);

    int rc = tcp_send_segment(worker_idx, tcb,
                      RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG,
                      data, send_len, tcb->snd_nxt, tcb->rcv_nxt);
    if (rc < 0) return 0;  /* TX failed — don't advance snd_nxt */
    tcb->snd_nxt += send_len;
    /* RFC 6298: only start RTO timer when not already running */
    if (tcb->rto_deadline_tsc == 0)
        arm_rto(tcb);
    worker_metrics_add_tcp_payload_tx(worker_idx, send_len);
    return (int)send_len;
}
