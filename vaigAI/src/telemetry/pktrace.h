/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: DPDK-native packet capture  (rte_pcapng + rx/tx callbacks).
 *
 * Two capture modes:
 *   Streaming:  pktrace_start() with a file path — packets are continuously
 *               flushed to disk via pktrace_flush().  No packet count limit.
 *   Buffered:   pktrace_start() without a path — packets accumulate in the
 *               ring buffer (4096 slots), then pktrace_save() writes them out.
 *
 * Usage:
 *   pktrace_init()    — once at startup, allocates ring + capture mempool
 *   pktrace_start()   — installs RX+TX callbacks on port/queue
 *   pktrace_flush()   — drain ring to file (streaming mode); call from mgmt tick
 *   pktrace_stop()    — removes callbacks, auto-flushes + closes file
 *   pktrace_save()    — writes buffered mbufs to a .pcapng file (non-streaming)
 *   pktrace_destroy() — cleanup at shutdown
 */
#ifndef TGEN_PKTRACE_H
#define TGEN_PKTRACE_H

#include <stdint.h>
#include <stdbool.h>

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
 * @param path      Output file for streaming mode, or NULL for buffered mode.
 *                  In streaming mode pktrace_flush() must be called
 *                  periodically from the management lcore.
 * @return 0 on success, negative errno on failure.
 */
int  pktrace_start(uint16_t port_id, uint16_t queue_id, uint32_t max_pkts,
                   const char *path);

/**
 * Flush captured packets from the ring to the open pcapng file.
 * Only meaningful in streaming mode.  Safe to call when idle.
 * Should be called periodically from the management lcore tick.
 */
void pktrace_flush(void);

/**
 * Stop an active capture.  Removes the rx/tx callbacks and logs capture
 * statistics.  In streaming mode, flushes remaining packets and closes
 * the output file automatically.
 */
void pktrace_stop(void);

/** Return number of packets captured since last pktrace_start(). */
uint32_t pktrace_count(void);

/** Return true if a capture is currently active. */
bool pktrace_is_active(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_PKTRACE_H */
