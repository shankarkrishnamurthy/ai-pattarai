/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: DPDK-native packet capture  (rte_pcapng + rx/tx callbacks).
 *
 * Usage:
 *   pktrace_init()   — once at startup, allocates ring + capture mempool
 *   pktrace_start()  — installs RX+TX callbacks on port/queue
 *   pktrace_stop()   — removes callbacks
 *   pktrace_save()   — writes captured mbufs to a .pcapng file
 *   pktrace_destroy()— cleanup at shutdown
 */
#ifndef TGEN_PKTRACE_H
#define TGEN_PKTRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the pktrace subsystem.
 * Creates a capture ring (PKTRACE_RING_SZ entries) and a dedicated
 * mempool sized for pcapng-annotated mbufs.
 * @return 0 on success, negative errno on failure.
 */
int  pktrace_init(void);

/** Free all pktrace resources (call after pktrace_stop). */
void pktrace_destroy(void);

/**
 * Start capturing packets on the given port + queue.
 * Installs DPDK rx and tx eth callbacks; captured mbufs are cloned into
 * the internal ring using rte_pcapng_copy().
 *
 * @param port_id   DPDK port to capture on.
 * @param queue_id  Queue index to capture on.
 * @param max_pkts  Stop after capturing this many packets (0 = unlimited).
 * @return 0 on success, negative errno on failure.
 */
int  pktrace_start(uint16_t port_id, uint16_t queue_id, uint32_t max_pkts);

/**
 * Stop an active capture.  Removes the rx/tx callbacks and logs capture
 * statistics.  Captured mbufs remain in the ring until pktrace_save().
 */
void pktrace_stop(void);

/**
 * Write all currently captured mbufs to a pcapng file.
 * The ring is drained and all mbufs are freed after writing.
 * Can be called whether a capture is running or not.
 *
 * @param path  Output file path, e.g. "capture.pcapng".
 * @return number of packets written on success, negative errno on failure.
 */
int  pktrace_save(const char *path);

/** Return number of packets captured since last pktrace_start(). */
uint32_t pktrace_count(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_PKTRACE_H */
