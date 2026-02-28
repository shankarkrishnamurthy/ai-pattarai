/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: soft/virtual NIC detection and post-init hooks (§1.5).
 */
#ifndef TGEN_SOFT_NIC_H
#define TGEN_SOFT_NIC_H

#include <stdint.h>
#include "../common/types.h"
#include "port_init.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Detect the driver kind from rte_eth_dev_info::driver_name. */
driver_kind_t soft_nic_detect(const char *driver_name);

/** Run per-driver post-init logic (e.g. load XDP pass programme for AF_XDP). */
void soft_nic_post_init(uint16_t port_id, port_caps_t *caps);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_SOFT_NIC_H */
