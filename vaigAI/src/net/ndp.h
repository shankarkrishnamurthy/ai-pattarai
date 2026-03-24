/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Neighbor Discovery Protocol (RFC 4861) — IPv6 equivalent of ARP.
 */
#ifndef TGEN_NDP_H
#define TGEN_NDP_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_rwlock.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NDP_CACHE_TTL_S         300
#define NDP_RATE_LIMIT          1000

/* ── NDP neighbor cache entry ────────────────────────────────────────────── */
typedef enum {
    NDP_STATE_FREE = 0,
    NDP_STATE_PENDING,
    NDP_STATE_REACHABLE,
    NDP_STATE_STALE,
    NDP_STATE_FAILED,
} ndp_state_t;

typedef struct {
    uint8_t             ip6[16];      /* network byte order */
    struct rte_ether_addr mac;
    ndp_state_t         state;
    uint64_t            expire_tsc;
    uint8_t             fail_count;
} ndp_entry_t;

/* ── Per-port NDP state ──────────────────────────────────────────────────── */
typedef struct {
    ndp_entry_t         entries[TGEN_NDP_CACHE_SZ];
    uint32_t            count;
    rte_rwlock_t        lock;
    uint8_t             local_ip6[16];    /* local IPv6 address */
    uint8_t             local_ip6_ll[16]; /* link-local address (fe80::) */
    struct rte_ether_addr local_mac;
    uint16_t            port_id;
    bool                has_ip6;          /* true if local_ip6 is configured */
} ndp_state_t_port;

/** Global NDP state per port. */
extern ndp_state_t_port g_ndp[TGEN_MAX_PORTS];

/** Initialise NDP state for all ports. */
int ndp_init(void);

/** Worker: classify incoming NDP/ICMPv6 frames and forward to mgmt ring. */
void ndp_input(uint32_t worker_idx, struct rte_mbuf *m);

/** Management: periodic tick — drain NDP ring, process NS/NA. */
void ndp_mgmt_tick(void);

/** Management: look up MAC for a given IPv6 address.
 *  Returns true if resolved; fills *mac_out. */
bool ndp_lookup(uint16_t port_id, const uint8_t *ip6,
                struct rte_ether_addr *mac_out);

/** Management: send a Neighbor Solicitation for ip6 out of port_id. */
int ndp_solicit(uint16_t port_id, const uint8_t *ip6);

/** Compute solicited-node multicast address for a unicast IPv6 address.
 *  Result: ff02::1:ffXX:XXXX where XX:XXXX are the low 24 bits of ip6. */
void ndp_solicited_node_mcast(const uint8_t *ip6, uint8_t *out);

/** Compute multicast MAC for an IPv6 multicast address.
 *  Result: 33:33:XX:XX:XX:XX where XX:XX:XX:XX are the low 32 bits. */
void ndp_mcast_mac(const uint8_t *mcast_ip6, struct rte_ether_addr *mac_out);

/** Process incoming Neighbor Solicitation (called from ICMPv6 mgmt). */
void ndp_process_ns(uint16_t port_id, const uint8_t *icmpv6_body,
                     uint16_t body_len, const uint8_t *sender_ip6);

/** Process incoming Neighbor Advertisement (called from ICMPv6 mgmt). */
void ndp_process_na(uint16_t port_id, const uint8_t *icmpv6_body,
                     uint16_t body_len);

/** Destroy NDP state. */
void ndp_destroy(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_NDP_H */
