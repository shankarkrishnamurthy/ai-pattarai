/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Ethernet L2 framing helpers (§2.1).
 */
#ifndef TGEN_ETHERNET_H
#define TGEN_ETHERNET_H

#include <stdint.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Return pointer to the Ethernet header, or NULL if mbuf too short. */
static inline struct rte_ether_hdr *eth_hdr(struct rte_mbuf *m)
{
    if (m->data_len < sizeof(struct rte_ether_hdr)) return NULL;
    return rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
}

/** Prepend an Ethernet header to an mbuf.
 *  @param vlan_id  0 = no VLAN tag; non-zero = insert 802.1Q tag. */
int eth_push_hdr(struct rte_mbuf *m,
                  const struct rte_ether_addr *src,
                  const struct rte_ether_addr *dst,
                  uint16_t ether_type,
                  uint16_t vlan_id);

/** Strip the Ethernet (+ optional VLAN) header and return the inner
 *  ether_type.  Returns 0 on failure. */
uint16_t eth_pop_hdr(struct rte_mbuf *m);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_ETHERNET_H */
