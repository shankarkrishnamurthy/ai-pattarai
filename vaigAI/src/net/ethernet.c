/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Ethernet L2 framing implementation.
 */
#include "ethernet.h"
#include <string.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_byteorder.h>

int eth_push_hdr(struct rte_mbuf *m,
                  const struct rte_ether_addr *src,
                  const struct rte_ether_addr *dst,
                  uint16_t ether_type,
                  uint16_t vlan_id)
{
    size_t hdr_len = sizeof(struct rte_ether_hdr);
    if (vlan_id)
        hdr_len += sizeof(struct rte_vlan_hdr);

    struct rte_ether_hdr *eth =
        (struct rte_ether_hdr *)rte_pktmbuf_prepend(m, (uint16_t)hdr_len);
    if (!eth) return -1;

    rte_ether_addr_copy(src, &eth->src_addr);
    rte_ether_addr_copy(dst, &eth->dst_addr);

    if (vlan_id) {
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);
        struct rte_vlan_hdr *v =
            (struct rte_vlan_hdr *)((uint8_t *)eth +
                                    sizeof(struct rte_ether_hdr));
        v->vlan_tci    = rte_cpu_to_be_16(vlan_id & 0x0FFF);
        v->eth_proto   = rte_cpu_to_be_16(ether_type);
    } else {
        eth->ether_type = rte_cpu_to_be_16(ether_type);
    }
    return 0;
}

uint16_t eth_pop_hdr(struct rte_mbuf *m)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    if (!eth) return 0;

    uint16_t etype = rte_be_to_cpu_16(eth->ether_type);
    size_t off = sizeof(struct rte_ether_hdr);

    if (etype == RTE_ETHER_TYPE_VLAN) {
        struct rte_vlan_hdr *v =
            (struct rte_vlan_hdr *)((uint8_t *)eth + off);
        etype = rte_be_to_cpu_16(v->eth_proto);
        off  += sizeof(struct rte_vlan_hdr);
    }

    if (rte_pktmbuf_adj(m, (uint16_t)off) == NULL) return 0;
    return etype;
}
