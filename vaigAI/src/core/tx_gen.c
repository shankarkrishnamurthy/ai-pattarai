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
#include <rte_ip.h>
#include <rte_icmp.h>
#include <rte_log.h>

#include "../common/types.h"
#include "../telemetry/metrics.h"

#define TX_GEN_MAX_BURST   32
#define ICMP_HDR_LEN        8

/* ══════════════════════════════════════════════════════════════════════════
 *  Protocol-specific builders
 *  Each returns a single rte_mbuf ready for TX, or NULL on alloc failure.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── ICMP Echo Request ────────────────────────────────────────────────────── */
static struct rte_mbuf *
build_icmp_echo(tx_gen_state_t *state, struct rte_mempool *mp)
{
    uint16_t payload_len = state->cfg.pkt_size;
    size_t total = sizeof(struct rte_ether_hdr)
                 + sizeof(struct rte_ipv4_hdr)
                 + ICMP_HDR_LEN + payload_len;

    struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
    if (unlikely(!m)) return NULL;

    char *buf = rte_pktmbuf_append(m, (uint16_t)total);
    if (unlikely(!buf)) { rte_pktmbuf_free(m); return NULL; }

    /* Ethernet */
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)buf;
    rte_ether_addr_copy(&state->cfg.src_mac, &eth->src_addr);
    rte_ether_addr_copy(&state->cfg.dst_mac, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* IPv4 */
    struct rte_ipv4_hdr *ip =
        (struct rte_ipv4_hdr *)(buf + sizeof(struct rte_ether_hdr));
    ip->version_ihl     = RTE_IPV4_VHL_DEF;
    ip->type_of_service = 0;
    ip->total_length    = rte_cpu_to_be_16(
        (uint16_t)(sizeof(*ip) + ICMP_HDR_LEN + payload_len));
    ip->packet_id       = rte_cpu_to_be_16(state->seq);
    ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
    ip->time_to_live    = 64;
    ip->next_proto_id   = IPPROTO_ICMP;
    ip->hdr_checksum    = 0;
    ip->src_addr        = state->cfg.src_ip;
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

/* ── Builder dispatch ─────────────────────────────────────────────────────── */
static inline struct rte_mbuf *
build_packet(tx_gen_state_t *state, struct rte_mempool *mp)
{
    switch (state->cfg.proto) {
    case TX_GEN_PROTO_ICMP:
        return build_icmp_echo(state, mp);
    /* Future protocols go here: */
    case TX_GEN_PROTO_UDP:
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
    state->tokens          = TX_GEN_MAX_BURST; /* initial allowance */
    state->pkts_sent       = 0;
    state->pkts_dropped    = 0;
    state->seq             = 0;

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
        __atomic_store_n(&state->active, false, __ATOMIC_RELEASE);
        return 0;
    }

    /* ── Token-bucket rate control ──────────────────────────────────── */
    uint32_t to_send = TX_GEN_MAX_BURST;
    if (state->cfg.rate_pps > 0) {
        uint64_t elapsed  = now - state->last_refill_tsc;
        uint64_t new_tok  = elapsed * state->cfg.rate_pps / rte_get_tsc_hz();
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

    /* ── Build packet burst ─────────────────────────────────────────── */
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
    worker_metrics_add_tx(worker_idx, sent, 0);
    if (state->cfg.proto == TX_GEN_PROTO_ICMP) {
        for (uint16_t i = 0; i < sent; i++)
            worker_metrics_add_icmp_echo_tx(worker_idx);
    }

    return sent;
}
