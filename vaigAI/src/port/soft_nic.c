/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: soft/virtual NIC detection and per-driver post-init.
 */
#include "soft_nic.h"

#include <string.h>
#include <rte_log.h>
#include <rte_ethdev.h>

/* ── Driver name → kind mapping ──────────────────────────────────────────── */
typedef struct { const char *name; driver_kind_t kind; } drv_map_t;

static const drv_map_t g_drv_map[] = {
    /* Physical — verified NICs only */
    { "net_mlx5",      DRIVER_PHYSICAL  },   /* Mellanox ConnectX-5/6 (bifurcated) */
    { "net_i40e",      DRIVER_PHYSICAL  },   /* Intel X710 / XL710                 */
    { "net_ixgbe",     DRIVER_PHYSICAL  },   /* Intel 82599 / X520                 */
    /* Soft / virtual */
    { "net_af_packet", DRIVER_AF_PACKET },
    { "net_af_xdp",    DRIVER_AF_XDP    },
    { "net_tap",       DRIVER_TAP       },
    { "net_virtio",    DRIVER_VIRTIO    },
    { "net_vhost",     DRIVER_VHOST     },
    { "net_null",      DRIVER_NULL      },
    { "net_ring",      DRIVER_RING      },
    { "net_bonding",   DRIVER_BONDING   },
};

driver_kind_t soft_nic_detect(const char *driver_name)
{
    if (!driver_name) return DRIVER_UNKNOWN;
    for (size_t i = 0; i < sizeof(g_drv_map)/sizeof(g_drv_map[0]); i++) {
        if (strcmp(driver_name, g_drv_map[i].name) == 0)
            return g_drv_map[i].kind;
    }
    return DRIVER_UNKNOWN;
}

/* ── Per-driver post-init ─────────────────────────────────────────────────── */
static void post_init_af_xdp(uint16_t port_id, port_caps_t *caps)
{
    (void)caps;
    /* AF_XDP: attempt zero-copy first; DPDK handles the fallback via devargs
     * (XDP_ZEROCOPY → XDP_COPY on ENOTSUP).  Log the detected mode. */
    RTE_LOG(INFO, PORT,
        "Port %u (net_af_xdp): zero-copy attempted; fill/completion rings "
        "sized to descriptor count\n", port_id);
}

static void post_init_tap(uint16_t port_id, port_caps_t *caps)
{
    (void)caps;
    RTE_LOG(INFO, PORT,
        "Port %u (net_tap): no HW checksum / VLAN / RSS\n", port_id);
}

static void post_init_null(uint16_t port_id, port_caps_t *caps)
{
    (void)caps;
    RTE_LOG(INFO, PORT,
        "Port %u (net_null): TX silently dropped; TX counters still "
        "incremented for pipeline benchmarking\n", port_id);
}

static void post_init_ring(uint16_t port_id, port_caps_t *caps)
{
    (void)caps;
    RTE_LOG(INFO, PORT,
        "Port %u (net_ring): in-process SPSC loopback\n", port_id);
}

static void post_init_vhost(uint16_t port_id, port_caps_t *caps)
{
    (void)caps;
    RTE_LOG(INFO, PORT,
        "Port %u (net_vhost): management core monitors socket connection "
        "state; guest reconnect triggers port re-init\n", port_id);
}

void soft_nic_post_init(uint16_t port_id, port_caps_t *caps)
{
    switch (caps->driver) {
    case DRIVER_AF_XDP:   post_init_af_xdp(port_id, caps);  break;
    case DRIVER_TAP:      post_init_tap(port_id, caps);      break;
    case DRIVER_NULL:     post_init_null(port_id, caps);     break;
    case DRIVER_RING:     post_init_ring(port_id, caps);     break;
    case DRIVER_VHOST:    post_init_vhost(port_id, caps);    break;
    default:              break;
    }
}
