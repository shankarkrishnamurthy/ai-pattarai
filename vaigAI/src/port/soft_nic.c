/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: soft/virtual NIC detection and per-driver post-init.
 */
#include "soft_nic.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
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
    { "net_memif",     DRIVER_MEMIF     },
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

/* ── GRO disable helper ───────────────────────────────────────────────────── */

/* Disable Generic Receive Offload on a Linux network interface.
 *
 * AF_PACKET receives packets AFTER kernel GRO coalesces them.  GRO can merge
 * multiple TCP segments into a single "super-packet" (up to ~64 KB) that
 * exceeds the AF_PACKET ring frame size (2048 bytes).  Such packets are
 * truncated by the ring and then dropped by vaigai's IP validation check
 * (ip->total_length > m->data_len).  Disabling GRO ensures each segment
 * arrives at most MTU-sized, which fits cleanly into the ring frames.
 */
static void disable_gro_on_iface(uint16_t port_id, const char *ifname)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        RTE_LOG(WARNING, PORT,
            "Port %u: cannot open socket to disable GRO on %s: %s\n",
            port_id, ifname, strerror(errno));
        return;
    }

    struct ifreq ifr;
    struct ethtool_value ev;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    ev.cmd  = ETHTOOL_SGRO;
    ev.data = 0;               /* 0 = disable */
    ifr.ifr_data = (void *)&ev;

    if (ioctl(sock, SIOCETHTOOL, &ifr) < 0) {
        RTE_LOG(WARNING, PORT,
            "Port %u: could not disable GRO on %s: %s\n",
            port_id, ifname, strerror(errno));
    } else {
        RTE_LOG(INFO, PORT,
            "Port %u: GRO disabled on %s (prevents oversized GRO "
            "packets from being dropped by AF_PACKET ring)\n",
            port_id, ifname);
    }

    close(sock);
}

static void post_init_af_packet(uint16_t port_id, port_caps_t *caps)
{
    (void)caps;

    struct rte_eth_dev_info info;
    if (rte_eth_dev_info_get(port_id, &info) == 0 && info.if_index > 0) {
        char ifname[IF_NAMESIZE];
        if (if_indextoname((unsigned)info.if_index, ifname))
            disable_gro_on_iface(port_id, ifname);
    }
}

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

static void post_init_memif(uint16_t port_id, port_caps_t *caps)
{
    /* net_memif supports multi-queue but the remote peer (testpmd or another
     * DPDK app) typically starts with 1 RX queue (queue 0).  When vaigai
     * requests n_workers+1 TX queues the PMD grants 2, so mgmt_tx_q is set
     * to 1 — but the peer only drains queue 0, silently dropping all mgmt
     * traffic (ARP requests, ICMP replies).  Force mgmt TX onto queue 0 so
     * ARP resolution works.  Worker and mgmt share queue 0 (two producers),
     * which is safe for the short-burst sizes used in testing. */
    caps->mgmt_tx_q = 0;
    RTE_LOG(INFO, PORT,
        "Port %u (net_memif): single-queue peer; mgmt_tx_q overridden to 0\n",
        port_id);
}

static void post_init_ring(uint16_t port_id, port_caps_t *caps)
{
    /* net_ring TX[i] and RX[i] share the same SPSC ring — loopback works only
     * when mgmt TX uses queue 0 (the same ring the worker drains on RX[0]).
     * Without this override vaigai requests a dedicated mgmt TX queue (index 1)
     * which is a separate ring that the worker never reads, breaking ARP/ping. */
    caps->mgmt_tx_q = 0;
    RTE_LOG(INFO, PORT,
        "Port %u (net_ring): SPSC loopback; mgmt_tx_q overridden to 0\n", port_id);
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
    case DRIVER_AF_PACKET: post_init_af_packet(port_id, caps); break;
    case DRIVER_AF_XDP:   post_init_af_xdp(port_id, caps);  break;
    case DRIVER_TAP:      post_init_tap(port_id, caps);      break;
    case DRIVER_NULL:     post_init_null(port_id, caps);     break;
    case DRIVER_RING:     post_init_ring(port_id, caps);     break;
    case DRIVER_MEMIF:    post_init_memif(port_id, caps);   break;
    case DRIVER_VHOST:    post_init_vhost(port_id, caps);    break;
    default:              break;
    }
}
