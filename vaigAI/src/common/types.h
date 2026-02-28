/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: shared type definitions, constants, and utility macros.
 */
#ifndef TGEN_TYPES_H
#define TGEN_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

#include <rte_log.h>

/* ── Log types (USER1-USER8 map to our subsystems) ───────────────────────── */
/* NOTE: In DPDK 24.11, RTE_LOGTYPE_PORT was removed from rte_log.h.
 *       In older DPDK (23.07) it is defined as 13.
 *       We define it conditionally to USER2 so code using RTE_LOG(x, PORT, ...)
 *       works on both versions. */
#ifndef RTE_LOGTYPE_PORT
#define RTE_LOGTYPE_PORT   RTE_LOGTYPE_USER2
#endif
#define RTE_LOGTYPE_TGEN   RTE_LOGTYPE_USER1
#define RTE_LOGTYPE_NET    RTE_LOGTYPE_USER3
#define RTE_LOGTYPE_TCP    RTE_LOGTYPE_USER4
#define RTE_LOGTYPE_TLS    RTE_LOGTYPE_USER5
#define RTE_LOGTYPE_HTTP   RTE_LOGTYPE_USER6
#define RTE_LOGTYPE_MGMT   RTE_LOGTYPE_USER7
#define RTE_LOGTYPE_TELEM  RTE_LOGTYPE_USER8

/* ── Constants ────────────────────────────────────────────────────────────── */
#define TGEN_MAX_PORTS          16
#define TGEN_MAX_LCORES         128
#define TGEN_MAX_WORKERS        124   /* TGEN_MAX_LCORES − 4 mgmt */
#define TGEN_MAX_MGMT_CORES     4
#define TGEN_MAX_QUEUES         64
#define TGEN_MAX_CONNECTIONS    1000000
#define TGEN_MAX_ROUTES         1024
#define TGEN_ARP_CACHE_SZ       1024
#define TGEN_DNS_CACHE_SZ       4096
#define TGEN_EPHEMERAL_LO       10000
#define TGEN_EPHEMERAL_HI       59999
#define TGEN_LOG_RING_SZ        65536
#define TGEN_IPC_RING_SZ        256   /* min; final = max(64, next_pow2(depth*2)) */
#define TGEN_MAX_TX_BURST       32
#define TGEN_MAX_RX_BURST       32
#define TGEN_DEFAULT_RX_DESC    2048
#define TGEN_DEFAULT_TX_DESC    2048
#define TGEN_MBUF_DATA_SZ       (2048 + 128)
#define TGEN_MAX_CHAIN_DEPTH    4
#define TGEN_TIMEWAIT_DEFAULT_MS 4000
#define TGEN_TIMEWAIT_MIN_MS    500
#define TGEN_ARP_HOLD_SZ        8
#define TGEN_OOO_QUEUE_SZ       8
#define TGEN_TEMPLATE_MAX_SZ    (64 * 1024)
#define TGEN_IFNAMESIZ          16
#define CACHE_LINE_SIZE         64

/* ── IPv4 helpers ─────────────────────────────────────────────────────────── */
#define TGEN_IPV4(a,b,c,d) \
    ((uint32_t)(a)<<24 | (uint32_t)(b)<<16 | (uint32_t)(c)<<8 | (uint32_t)(d))

/* ── Role flags ───────────────────────────────────────────────────────────── */
typedef enum {
    LCORE_ROLE_WORKER    = 0,
    LCORE_ROLE_PRIMARY_MGMT,
    LCORE_ROLE_TELEMETRY,
    LCORE_ROLE_CLI_API,
    LCORE_ROLE_WATCHDOG,
    LCORE_ROLE_IDLE,
} lcore_role_t;

/* ── Driver kind ──────────────────────────────────────────────────────────── */
typedef enum {
    DRIVER_PHYSICAL = 0,
    DRIVER_AF_PACKET,
    DRIVER_AF_XDP,
    DRIVER_TAP,
    DRIVER_VIRTIO,
    DRIVER_VHOST,
    DRIVER_NULL,
    DRIVER_RING,
    DRIVER_BONDING,
    DRIVER_UNKNOWN,
} driver_kind_t;

/* ── Load-shape mode ──────────────────────────────────────────────────────── */
typedef enum {
    LOAD_UNLIMITED = 0,
    LOAD_CONSTANT,
} load_mode_t;

/* ── Target metric for load shaping ─────────────────────────────────────── */
typedef enum {
    METRIC_CPS = 0,
    METRIC_RPS,
    METRIC_TPS,
    METRIC_MBPS,
} load_metric_t;

/* ── Generic result ───────────────────────────────────────────────────────── */
typedef enum {
    TGEN_OK  =  0,
    TGEN_ERR = -1,
} tgen_rc_t;

/* ── Macros ───────────────────────────────────────────────────────────────── */
#define TGEN_UNUSED(x)  ((void)(x))
#define TGEN_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define TGEN_ALIGN_POW2(x, align) \
    (((x) + ((align) - 1)) & ~((typeof(x))((align) - 1)))
#define TGEN_IS_POW2(x)  (((x) != 0) && (((x) & ((x) - 1)) == 0))
#define TGEN_NEXT_POW2(x) (__extension__({ \
    typeof(x) _v = (x) - 1; \
    _v |= _v >> 1; _v |= _v >> 2; _v |= _v >> 4; \
    _v |= _v >> 8; _v |= _v >> 16; _v |= _v >> 32; \
    _v + 1; \
}))

#define TGEN_MIN(a,b) ((a) < (b) ? (a) : (b))
#define TGEN_MAX(a,b) ((a) > (b) ? (a) : (b))
#define TGEN_CLAMP(v,lo,hi) (TGEN_MAX((lo), TGEN_MIN((v), (hi))))

#endif /* TGEN_TYPES_H */
