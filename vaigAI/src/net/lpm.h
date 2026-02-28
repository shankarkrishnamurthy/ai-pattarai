/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: LPM routing table wrapper (§2.4, rte_lpm).
 */
#ifndef TGEN_LPM_H
#define TGEN_LPM_H

#include <stdint.h>
#include <rte_rwlock.h>
#include "../common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LPM_MAX_ROUTES 1024

typedef struct {
    uint32_t prefix;        /* network byte order */
    uint8_t  prefix_len;
    uint32_t next_hop_ip;   /* network byte order */
    uint16_t egress_port;
} lpm_route_t;

/** Initialise LPM table (called once at startup). */
int lpm_init(void);

/** Add a static route.  Management core only.  Thread-safe via rwlock. */
int lpm_add(const lpm_route_t *route);

/** Remove a route by prefix & length. */
int lpm_del(uint32_t prefix_net, uint8_t prefix_len);

/** Look up a destination address.  Worker-safe (read lock). */
int lpm_lookup(uint32_t dst_ip_net,
               uint32_t *next_hop_ip_out,
               uint16_t *egress_port_out);

/** Destroy the LPM table. */
void lpm_destroy(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_LPM_H */
