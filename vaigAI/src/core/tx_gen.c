/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Timer-based TX packet generator (§1.9).
 *
 * The worker loop calls tx_gen_burst() on every iteration.  If the
 * generator is armed, it builds a burst of packets using the protocol-
 * specific builder, rate-limits with a token bucket, and transmits
 * directly via rte_eth_tx_burst().
 *
 * When the TSC deadline expires the generator self-disarms — no IPC
 * round-trip required for time-bounded runs.
 */
#include "tx_gen.h"

#include <string.h>
#include <netinet/in.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>
#include <rte_ip.h>
#include <rte_icmp.h>
#include <rte_udp.h>
#include <rte_log.h>

#include "../common/types.h"
#include "../telemetry/metrics.h"
#include "../net/tcp_fsm.h"
#include "../net/tcp_port_pool.h"
#include "../net/tcp_tcb.h"
#include "../app/http11.h"
#include "../tls/tls_session.h"
#include "../tls/tls_engine.h"

#define TX_GEN_MAX_BURST   32
#define ICMP_HDR_LEN        8

/* Limit concurrent in-flight TCP connections (SYN_SENT) to prevent
 * overwhelming the peer's SYN backlog.  Connections complete in
 * microseconds when the server can keep up, so a modest limit
 * doesn't reduce sustained CPS — it just prevents the initial burst
 * from causing mass SYN drops and 200 ms RTO retransmit storms. */
#define TCP_MAX_INFLIGHT   4096

static uint8_t g_tp_zero_buf[1400]; /* zero-filled plaintext for throughput
                                      * Keep small enough that TLS record
                                      * (plaintext + ~29B overhead) fits in
                                      * one MSS (1460). */

/* ── Pre-built HTTP request (one per worker, reused across connections) ──── */
http_prebuilt_req_t g_http_req[TGEN_MAX_WORKERS];

/* ── Per-flow custom HTTP headers (set by CLI, consumed by tx_gen_burst) ── */
char g_http_custom_hdrs[TGEN_MAX_CLIENT_FLOWS][512];

/* ══════════════════════════════════════════════════════════════════════════
 *  Protocol-specific builders
 *  Each returns a single rte_mbuf ready for TX, or NULL on alloc failure.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── ICMP Echo Request ────────────────────────────────────────────────────── */
static struct rte_mbuf *
build_icmp_echo(tx_gen_state_t *state, struct rte_mempool *mp)
{
    uint16_t payload_len = state->cfg.pkt_size;
    uint16_t vlan_id = state->cfg.vlan_id;
    size_t eth_sz = sizeof(struct rte_ether_hdr)
                  + (vlan_id ? sizeof(struct rte_vlan_hdr) : 0);
    size_t total = eth_sz
                 + sizeof(struct rte_ipv4_hdr)
                 + ICMP_HDR_LEN + payload_len;

    struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
    if (unlikely(!m)) return NULL;

    char *buf = rte_pktmbuf_append(m, (uint16_t)total);
    if (unlikely(!buf)) { rte_pktmbuf_free(m); return NULL; }

    /* Ethernet (with optional 802.1Q VLAN) */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
    rte_ether_addr_copy(&state->cfg.src_mac, &eth->src_addr);
    rte_ether_addr_copy(&state->cfg.dst_mac, &eth->dst_addr);
    if (vlan_id) {
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);
        struct rte_vlan_hdr *vh = (struct rte_vlan_hdr *)(
            buf + sizeof(struct rte_ether_hdr));
        vh->vlan_tci  = rte_cpu_to_be_16(vlan_id & 0x0FFF);
        vh->eth_proto = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    } else {
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    }

    /* IPv4 */
    struct rte_ipv4_hdr *ip =
        (struct rte_ipv4_hdr *)(buf + eth_sz);
    ip->version_ihl     = RTE_IPV4_VHL_DEF;
    ip->type_of_service = (uint8_t)(state->cfg.dscp << 2);
    ip->total_length    = rte_cpu_to_be_16(
        (uint16_t)(sizeof(*ip) + ICMP_HDR_LEN + payload_len));
    ip->packet_id       = rte_cpu_to_be_16(state->seq);
    ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    ip->time_to_live    = 64;
    ip->next_proto_id   = IPPROTO_ICMP;
    ip->hdr_checksum    = 0;
    /* IP range pool for ICMP */
    uint32_t icmp_src_ip = state->cfg.src_ip;
    if (state->cfg.src_ip_count > 1) {
        uint32_t off = state->ip_pool_idx % state->cfg.src_ip_count;
        state->ip_pool_idx++;
        icmp_src_ip = rte_cpu_to_be_32(
            rte_be_to_cpu_32(state->cfg.src_ip) + off);
    }
    ip->src_addr        = icmp_src_ip;
    ip->dst_addr        = state->cfg.dst_ip;
    ip->hdr_checksum    = rte_ipv4_cksum(ip);

    /* ICMP echo request */
    struct rte_icmp_hdr *icmp =
        (struct rte_icmp_hdr *)((uint8_t *)ip + sizeof(*ip));
    icmp->icmp_type  = RTE_ICMP_TYPE_ECHO_REQUEST;
    icmp->icmp_code  = 0;
    icmp->icmp_cksum = 0;
    /* identifier + sequence */
    *((uint16_t *)((uint8_t *)icmp + 4)) = rte_cpu_to_be_16(state->ident);
    *((uint16_t *)((uint8_t *)icmp + 6)) = rte_cpu_to_be_16(state->seq);
    /* fill payload */
    memset((uint8_t *)icmp + ICMP_HDR_LEN, 0xAB, payload_len);
    /* checksum */
    uint16_t ck = rte_raw_cksum(icmp, ICMP_HDR_LEN + payload_len);
    icmp->icmp_cksum = (ck == 0xFFFF) ? ck : (uint16_t)~ck;

    state->seq++;
    return m;
}

/* ── UDP Datagram ─────────────────────────────────────────────────────────── */
static struct rte_mbuf *
build_udp_datagram(tx_gen_state_t *state, struct rte_mempool *mp)
{
    uint16_t payload_len = state->cfg.pkt_size;
    uint16_t udp_total   = (uint16_t)(sizeof(struct rte_udp_hdr) + payload_len);
    uint16_t vlan_id = state->cfg.vlan_id;
    size_t eth_sz = sizeof(struct rte_ether_hdr)
                  + (vlan_id ? sizeof(struct rte_vlan_hdr) : 0);
    size_t   total       = eth_sz
                         + sizeof(struct rte_ipv4_hdr)
                         + udp_total;

    struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
    if (unlikely(!m)) return NULL;

    char *buf = rte_pktmbuf_append(m, (uint16_t)total);
    if (unlikely(!buf)) { rte_pktmbuf_free(m); return NULL; }

    /* Ethernet (with optional 802.1Q VLAN) */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
    rte_ether_addr_copy(&state->cfg.src_mac, &eth->src_addr);
    rte_ether_addr_copy(&state->cfg.dst_mac, &eth->dst_addr);
    if (vlan_id) {
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);
        struct rte_vlan_hdr *vh = (struct rte_vlan_hdr *)(
            buf + sizeof(struct rte_ether_hdr));
        vh->vlan_tci  = rte_cpu_to_be_16(vlan_id & 0x0FFF);
        vh->eth_proto = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    } else {
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    }

    /* IPv4 */
    struct rte_ipv4_hdr *ip =
        (struct rte_ipv4_hdr *)(buf + eth_sz);
    ip->version_ihl     = RTE_IPV4_VHL_DEF;
    ip->type_of_service = (uint8_t)(state->cfg.dscp << 2);
    ip->total_length    = rte_cpu_to_be_16(
        (uint16_t)(sizeof(*ip) + udp_total));
    ip->packet_id       = rte_cpu_to_be_16(state->seq);
    ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    ip->time_to_live    = 64;
    ip->next_proto_id   = IPPROTO_UDP;
    ip->hdr_checksum    = 0;
    /* IP range pool for UDP */
    uint32_t udp_src_ip = state->cfg.src_ip;
    if (state->cfg.src_ip_count > 1) {
        uint32_t off = state->ip_pool_idx % state->cfg.src_ip_count;
        state->ip_pool_idx++;
        udp_src_ip = rte_cpu_to_be_32(
            rte_be_to_cpu_32(state->cfg.src_ip) + off);
    }
    ip->src_addr        = udp_src_ip;
    ip->dst_addr        = state->cfg.dst_ip;
    ip->hdr_checksum    = rte_ipv4_cksum(ip);

    /* UDP */
    struct rte_udp_hdr *udp =
        (struct rte_udp_hdr *)((uint8_t *)ip + sizeof(*ip));
    udp->src_port    = rte_cpu_to_be_16(state->cfg.src_port);
    udp->dst_port    = rte_cpu_to_be_16(state->cfg.dst_port);
    udp->dgram_len   = rte_cpu_to_be_16(udp_total);
    udp->dgram_cksum = 0;

    /* Fill payload */
    memset((uint8_t *)udp + sizeof(*udp), 0xBE, payload_len);

    /* UDP checksum (optional per RFC 768 for IPv4, but good practice) */
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);

    state->seq++;
    return m;
}

/* ── Builder dispatch ─────────────────────────────────────────────────────── */
static inline struct rte_mbuf *
build_packet(tx_gen_state_t *state, struct rte_mempool *mp)
{
    switch (state->cfg.proto) {
    case TX_GEN_PROTO_ICMP:
        return build_icmp_echo(state, mp);
    case TX_GEN_PROTO_UDP:
        return build_udp_datagram(state, mp);
    /* Future protocols go here: */
    case TX_GEN_PROTO_TCP_SYN:
    case TX_GEN_PROTO_HTTP:
    default:
        return NULL;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════════ */

void
tx_gen_configure(tx_gen_state_t *state, const tx_gen_config_t *cfg,
                 uint16_t tx_queue)
{
    memset(state, 0, sizeof(*state));
    memcpy(&state->cfg, cfg, sizeof(*cfg));
    state->ident       = (uint16_t)(rte_rdtsc() & 0xFFFF);
    state->tx_queue_id = tx_queue;
}

void
tx_gen_start(tx_gen_state_t *state)
{
    uint64_t now = rte_rdtsc();
    state->start_tsc       = now;
    state->last_refill_tsc = now;
    state->pkts_sent       = 0;
    state->pkts_dropped    = 0;
    state->seq             = 0;

    /* Cap initial token allowance when max_initiations is set,
     * so a --one command doesn't burst 32 connections on first tick. */
    if (state->cfg.max_initiations > 0 &&
        state->cfg.max_initiations < TX_GEN_MAX_BURST)
        state->tokens = state->cfg.max_initiations;
    else
        state->tokens = TX_GEN_MAX_BURST;

    if (state->cfg.duration_s > 0)
        state->deadline_tsc = now +
            (uint64_t)state->cfg.duration_s * rte_get_tsc_hz();
    else
        state->deadline_tsc = 0; /* run until stopped */

    __atomic_store_n(&state->active, true, __ATOMIC_RELEASE);
}

void
tx_gen_stop(tx_gen_state_t *state)
{
    __atomic_store_n(&state->active, false, __ATOMIC_RELEASE);
}

uint32_t
tx_gen_burst(tx_gen_state_t *state, struct rte_mempool *mp,
             uint32_t worker_idx)
{
    if (!__atomic_load_n(&state->active, __ATOMIC_ACQUIRE))
        return 0;

    uint64_t now = rte_rdtsc();

    /* ── Deadline check ─────────────────────────────────────────────── */
    if (state->deadline_tsc > 0 && now >= state->deadline_tsc) {
        /* Close throughput connections before stopping */
        if (state->cfg.proto == TX_GEN_PROTO_THROUGHPUT) {
            for (uint32_t i = 0; i < state->tp_n_streams; i++) {
                tcb_t *t = (tcb_t *)state->tp_tcbs[i];
                if (t && (t->state == TCP_ESTABLISHED ||
                         t->state == TCP_CLOSE_WAIT))
                    tcp_fsm_close(worker_idx, t);
                state->tp_tcbs[i] = NULL;
            }
            state->tp_n_streams = 0;
            state->tp_phase = 0;
        }
        __atomic_store_n(&state->active, false, __ATOMIC_RELEASE);
        return 0;
    }

    /* ── Max-initiations check (--one flag) ─────────────────────────── */
    if (state->cfg.max_initiations > 0 &&
        state->pkts_sent >= state->cfg.max_initiations) {
        /* Already initiated enough connections/packets.
         * Stop generating new ones.  Existing TCP connections
         * continue processing via RX and timer ticks. */
        __atomic_store_n(&state->active, false, __ATOMIC_RELEASE);
        return 0;
    }

    /* ── Token-bucket rate control ──────────────────────────────────── */
    uint32_t to_send = TX_GEN_MAX_BURST;
    if (state->cfg.rate_pps > 0) {
        /* Compute effective rate (with optional ramp-up) */
        uint64_t eff_rate = state->cfg.rate_pps;
        if (state->cfg.ramp_s > 0) {
            uint64_t age = (now - state->start_tsc) / rte_get_tsc_hz();
            if (age < state->cfg.ramp_s)
                eff_rate = state->cfg.rate_pps * (age + 1) / state->cfg.ramp_s;
        }
        uint64_t elapsed  = now - state->last_refill_tsc;
        uint64_t new_tok  = elapsed * eff_rate / rte_get_tsc_hz();
        if (new_tok > 0) {
            state->tokens += new_tok;
            state->last_refill_tsc = now;
            if (state->tokens > TX_GEN_MAX_BURST)
                state->tokens = TX_GEN_MAX_BURST;
        }
        to_send = (uint32_t)state->tokens;
        if (to_send == 0)
            return 0;
    }

    /* ── TCP SYN TPS: use tcp_fsm_connect() instead of raw packets ── */
    if (state->cfg.proto == TX_GEN_PROTO_TCP_SYN ||
        state->cfg.proto == TX_GEN_PROTO_HTTP) {

        /* Pace connection opens: limit concurrent in-flight TCBs to
         * prevent a SYN storm that overwhelms the peer's backlog. */
        uint32_t in_flight = g_tcb_stores[worker_idx].count;
        if (in_flight >= TCP_MAX_INFLIGHT) {
            return 0;   /* wait for existing connections to complete */
        }
        to_send = TGEN_MIN(to_send, TCP_MAX_INFLIGHT - in_flight);

        /* Pre-build HTTP request headers on first burst */
        if (state->cfg.proto == TX_GEN_PROTO_HTTP &&
            g_http_req[worker_idx].hdr_len == 0) {
            bool ka = state->cfg.txn_per_conn > 1;
            http_conn_t tmp;
            http11_conn_init(&tmp);
            const char *extra = g_http_custom_hdrs[state->cfg.flow_idx];
            http_request_t req = {
                .method        = (http_method_t)state->cfg.http_method,
                .url           = state->cfg.http_url[0] ? state->cfg.http_url : "/",
                .host          = state->cfg.http_host[0] ? state->cfg.http_host : "localhost",
                .keep_alive    = ka,
                .extra_headers = (extra && extra[0]) ? extra : NULL,
            };
            int n = http11_tx_request(&tmp, &req);
            if (n > 0) {
                memcpy(g_http_req[worker_idx].hdr, tmp.tx_hdr, (size_t)n);
                g_http_req[worker_idx].hdr_len = (uint32_t)n;
                g_http_req[worker_idx].keep_alive = ka;
                g_http_req[worker_idx].txn_per_conn = state->cfg.txn_per_conn;
                g_http_req[worker_idx].think_time_us = state->cfg.think_time_us;
                g_http_req[worker_idx].expected_interval_us =
                    state->cfg.rate_pps > 0
                        ? 1000000ULL / state->cfg.rate_pps : 0;
            }
        }

        uint32_t initiated = 0;
        for (uint32_t i = 0; i < to_send; i++) {
            /* IP range pool: cycle through src IPs */
            uint32_t cur_src_ip = state->cfg.src_ip;
            if (state->cfg.src_ip_count > 1) {
                uint32_t offset = state->ip_pool_idx %
                                  state->cfg.src_ip_count;
                state->ip_pool_idx++;
                /* src_ip is network byte order — convert, add, convert back */
                cur_src_ip = rte_cpu_to_be_32(
                    rte_be_to_cpu_32(state->cfg.src_ip) + offset);
            }
            uint16_t src_port;
            if (tcp_port_alloc(worker_idx, cur_src_ip,
                               &src_port) < 0)
                break;   /* port pool exhausted */
            tcb_t *tcb = tcp_fsm_connect(worker_idx,
                             cur_src_ip, src_port,
                             state->cfg.dst_ip, state->cfg.dst_port,
                             state->cfg.port_id);
            if (!tcb) {
                tcp_port_free_immediate(worker_idx, cur_src_ip, src_port);
                break;   /* TCB store full */
            }
            tcb->dscp    = state->cfg.dscp;
            tcb->vlan_id = state->cfg.vlan_id;
            tcb->cc_algo = state->cfg.cc_algo;
            if (state->cfg.max_initiations > 0)
                tcb->graceful_close = true;
            /* Mark connection for HTTP request after ESTABLISHED */
            if (state->cfg.proto == TX_GEN_PROTO_HTTP) {
                tcb->app_ctx = &g_http_req[worker_idx];
                if (state->cfg.enable_tls) {
                    /* HTTPS: TLS handshake first, then HTTP after TLS done.
                     * app_state 1 → TLS handshake → app_state 3 (TLS ok) →
                     * then the ESTABLISHED data handler sends HTTP. */
                    tcb->app_state = 1; /* 1 = TLS requested */
                } else {
                    /* Plain HTTP: send request immediately after TCP ESTABLISHED */
                    tcb->app_state = 4; /* 4 = HTTP send request */
                }
            } else if (state->cfg.enable_tls) {
                /* Raw TLS (no HTTP): just do TLS handshake */
                tcb->app_state = 1; /* 1 = TLS requested */
            }
            initiated++;
        }
        state->pkts_sent += initiated;
        if (state->cfg.rate_pps > 0) {
            /* Always consume at least 1 token per attempt so that
             * failures (port exhaustion, TCB store full) don't spin
             * in a tight loop with tokens stuck at the initial value. */
            uint64_t consumed = initiated > 0 ? initiated : 1;
            if (consumed <= state->tokens)
                state->tokens -= consumed;
            else
                state->tokens = 0;
        }
        /* tcp_fsm_connect already updates tcp_syn_sent & tx metrics */
        return initiated;
    }

    /* ── Throughput mode: create connections once, pump data continuously ── */
    if (state->cfg.proto == TX_GEN_PROTO_THROUGHPUT) {
        bool tls = state->cfg.enable_tls;
        uint32_t streams = state->cfg.throughput_streams;
        if (streams == 0) streams = 1;
        if (streams > 16) streams = 16;

        /* Phase 0: Create connections (once) */
        if (state->tp_phase == 0) {
            state->tp_n_streams = 0;
            for (uint32_t i = 0; i < streams; i++) {
                uint16_t src_port;
                if (tcp_port_alloc(worker_idx, state->cfg.src_ip,
                                   &src_port) < 0)
                    break;
                tcb_t *tcb = tcp_fsm_connect(worker_idx,
                                 state->cfg.src_ip, src_port,
                                 state->cfg.dst_ip, state->cfg.dst_port,
                                 state->cfg.port_id);
                if (!tcb) {
                    tcp_port_free(worker_idx, state->cfg.src_ip, src_port);
                    break;
                }
                tcb->app_ctx = (void *)1; /* mark as throughput (not SYN-only) */
                tcb->dscp    = state->cfg.dscp;
                tcb->vlan_id = state->cfg.vlan_id;
                tcb->cc_algo = state->cfg.cc_algo;
                if (tls) {
                    tcb->app_state = 1; /* request TLS handshake */
                }
                state->tp_tcbs[i] = tcb;
                state->tp_n_streams++;
            }
            state->tp_phase = 1; /* move to pump phase */
            return state->tp_n_streams;
        }

        /* Phase 1: Pump data on established connections */
        uint32_t sent_total = 0;
        uint8_t ct_buf[2048];
        for (uint32_t i = 0; i < state->tp_n_streams; i++) {
            tcb_t *tcb = (tcb_t *)state->tp_tcbs[i];
            if (!tcb || (tcb->state != TCP_ESTABLISHED &&
                         tcb->state != TCP_CLOSE_WAIT))
                continue;
            if (tls && tcb->app_state == 3) {
                uint32_t ci = (uint32_t)(tcb - g_tcb_stores[worker_idx].tcbs);
                tls_session_t *sess = tls_session_get(worker_idx, ci);
                if (sess) {
                    int ct_len = tls_encrypt(sess,
                                    g_tp_zero_buf, sizeof(g_tp_zero_buf),
                                    ct_buf, sizeof(ct_buf));
                    if (ct_len > 0) {
                        int rc = tcp_fsm_send(worker_idx, tcb,
                                     ct_buf, (uint32_t)ct_len);
                        if (rc > 0) {
                            sent_total++;
                            worker_metrics_add_tls_tx(worker_idx);
                        }
                    }
                }
            } else if (!tls) {
                /* Send multiple segments to fill the TCP window */
                for (int seg = 0; seg < 32; seg++) {
                    int rc = tcp_fsm_send(worker_idx, tcb,
                                 g_tp_zero_buf, (uint32_t)sizeof(g_tp_zero_buf));
                    if (rc <= 0) break; /* window full or error */
                    sent_total++;
                }
            }
        }
        state->pkts_sent += sent_total;
        return sent_total;
    }

    /* ── Build packet burst (ICMP / UDP) ────────────────────────────── */
    struct rte_mbuf *pkts[TX_GEN_MAX_BURST];
    uint32_t built = 0;
    for (uint32_t i = 0; i < to_send; i++) {
        pkts[built] = build_packet(state, mp);
        if (pkts[built])
            built++;
        else
            break;   /* mempool exhaustion — stop building */
    }
    if (built == 0)
        return 0;

    /* ── Transmit ───────────────────────────────────────────────────── */
    uint16_t sent = rte_eth_tx_burst(state->cfg.port_id,
                                     state->tx_queue_id,
                                     pkts, (uint16_t)built);

    /* Free unsent */
    for (uint16_t i = sent; i < built; i++) {
        rte_pktmbuf_free(pkts[i]);
        state->pkts_dropped++;
    }

    state->pkts_sent += sent;
    if (state->cfg.rate_pps > 0 && sent <= state->tokens)
        state->tokens -= sent;

    /* ── Metrics ────────────────────────────────────────────────────── */
    uint64_t tx_total_bytes = 0;
    for (uint16_t i = 0; i < sent; i++)
        tx_total_bytes += pkts[i]->pkt_len;
    worker_metrics_add_tx(worker_idx, sent, tx_total_bytes);
    if (state->cfg.proto == TX_GEN_PROTO_ICMP) {
        for (uint16_t i = 0; i < sent; i++)
            worker_metrics_add_icmp_echo_tx(worker_idx);
    } else if (state->cfg.proto == TX_GEN_PROTO_UDP) {
        for (uint16_t i = 0; i < sent; i++)
            worker_metrics_add_udp_tx(worker_idx);
    }

    return sent;
}
