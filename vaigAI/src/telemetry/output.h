/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Structured NDJSON output for cross-run comparison (§6.5).
 *
 * When enabled via -O <file>, every lifecycle event is appended as a
 * single JSON line.  All writes happen on the management lcore only.
 */
#ifndef TGEN_OUTPUT_H
#define TGEN_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open the structured output file.  No-op if path is NULL or empty.
 * @return 0 on success, -1 on error.
 */
int  output_init(const char *path);

/** Close the output file (if open). */
void output_fini(void);

/** @return true if output logging is active. */
bool output_enabled(void);

/* ── Event emitters (all are no-ops when output is disabled) ────────── */

/** Emit "meta" event: build version, host info, command line. */
void output_meta(int argc, char **argv);

/** Emit "config" event: workers, ports, IPs, server mode, TLS, etc. */
void output_config(void);

/** Emit "port" event: per-port NIC details. */
void output_port(uint16_t port_id);

/** Emit "cmd" event: record a CLI command. */
void output_cmd(const char *input);

/** Emit "start" event: test parameters for a client stream. */
void output_start(uint32_t stream_idx, const char *proto,
                  const char *dst_ip, uint16_t dst_port,
                  uint32_t duration, uint64_t rate, uint16_t size,
                  bool tls, uint32_t streams, bool reuse);

/** Emit "serve" event: server listener configuration. */
void output_serve(const char *listeners, const char *ciphers);

/** Emit "progress" event: periodic stats snapshot. */
void output_progress(uint32_t stream_idx, uint64_t elapsed_s,
                     const metrics_snapshot_t *snap);

/** Emit "result" event: final metrics for a completed stream. */
void output_result(uint32_t stream_idx, const char *proto,
                   double actual_s, const metrics_snapshot_t *snap);

/** Emit "error" event: structured error/warning. */
void output_error(const char *severity, const char *module,
                  const char *message);

/** Emit "end" event: process shutdown. */
void output_end(int exit_code, uint64_t uptime_s);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_OUTPUT_H */
