/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP option parsing and generation (§2.5).
 */
#ifndef TGEN_TCP_OPTIONS_H
#define TGEN_TCP_OPTIONS_H

#include <stdint.h>
#include <stdbool.h>
#include <rte_tcp.h>
#include "tcp_tcb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TCP option kinds */
#define TCPOPT_EOL           0
#define TCPOPT_NOP           1
#define TCPOPT_MSS           2
#define TCPOPT_WINDOW_SCALE  3
#define TCPOPT_SACK_PERM     4
#define TCPOPT_SACK          5
#define TCPOPT_TIMESTAMP     8

/* Parsed TCP options */
typedef struct {
    uint16_t    mss;
    uint8_t     wscale;
    bool        has_mss;
    bool        has_wscale;
    bool        has_sack_perm;
    bool        has_timestamps;
    uint32_t    ts_val;
    uint32_t    ts_ecr;
    sack_block_t sack[4];
    uint8_t      sack_count;
} tcp_parsed_opts_t;

/** Parse TCP options from a segment; returns 0 on success. */
int tcp_options_parse(const struct rte_tcp_hdr *tcp,
                       tcp_parsed_opts_t *out);

/** Write options into a SYN segment; returns options byte length. */
int tcp_options_write_syn(uint8_t *buf, size_t bufsz,
                           uint16_t mss, uint8_t wscale,
                           bool sack_perm, bool timestamps,
                           uint32_t ts_val);

/** Write options into a data/ACK segment; returns options byte length. */
int tcp_options_write_data(uint8_t *buf, size_t bufsz,
                            bool timestamps, uint32_t ts_val, uint32_t ts_ecr,
                            const sack_block_t *sack, uint8_t sack_count);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TCP_OPTIONS_H */
