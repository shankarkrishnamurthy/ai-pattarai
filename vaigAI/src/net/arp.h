/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: ARP module (§2.2).
 */
#ifndef TGEN_ARP_H
#define TGEN_ARP_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_rwlock.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARP_CACHE_TTL_S         300
#define ARP_PROBE_BEFORE_EXPIRY 30
#define ARP_MAX_PROBE_FAILURES  2
#define ARP_GRATUITOUS_INTERVAL 60
#define ARP_RATE_LIMIT          1000  /* requests/s/port */
#define ARP_HOLD_QUEUE_SZ       TGEN_ARP_HOLD_SZ

/* ── ARP cache entry ──────────────────────────────────────────────────────── */
typedef enum {
    ARP_STATE_FREE = 0,
    ARP_STATE_PENDING,
    ARP_STATE_RESOLVED,
    ARP_STATE_STALE,
    ARP_STATE_FAILED,
} arp_state_t;

typedef struct {
    uint32_t            ip;           /* network byte order */
    struct rte_ether_addr mac;
    arp_state_t         state;
    uint64_t            expire_tsc;
    uint8_t             fail_count;
    /* hold queue for outgoing packets waiting for this entry */
    struct rte_mbuf    *hold[ARP_HOLD_QUEUE_SZ];
    uint32_t            hold_count;
} arp_entry_t;

/* ── Per-port ARP state ─────────────────────────────────────────────────────── */
typedef struct {
    struct rte_hash    *table;        /* keyed on IPv4 address */
    arp_entry_t         entries[TGEN_ARP_CACHE_SZ];
    rte_rwlock_t        lock;
    uint32_t            local_ip;     /* local IPv4 in network byte order */
    struct rte_ether_addr local_mac;
    uint16_t            port_id;
    /* Token bucket for rate limiting */
    uint64_t            token_bucket;
    uint64_t            last_tb_tsc;
} arp_state_t_port;

/** Global ARP state per port. */
extern arp_state_t_port g_arp[TGEN_MAX_PORTS];

/** Initialise ARP state for all ports. */
int arp_init(void);

/** Worker: classify incoming ARP frames and forward to mgmt ring.
 *  Consumes the mbuf. */
void arp_input(uint32_t worker_idx, struct rte_mbuf *m);

/** Management: process one dequeued ARP mbuf (reply / gratuitous handling). */
void arp_mgmt_process(uint16_t port_id, struct rte_mbuf *m);

/** Management: periodic tick — probe stale entries, send gratuitous ARPs. */
void arp_mgmt_tick(void);

/** Management: look up MAC for a given IPv4 (caller holds no lock).
 *  Returns true if resolved; fills *mac_out. */
bool arp_lookup(uint16_t port_id, uint32_t ip_net,
                struct rte_ether_addr *mac_out);

/** Management: send an ARP request for ip_net out of port_id. */
int arp_request(uint16_t port_id, uint32_t ip_net);

/** Destroy ARP state. */
void arp_destroy(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_ARP_H */
