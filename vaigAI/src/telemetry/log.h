/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Structured logging wrapper (§6.2).
 */
#ifndef TGEN_LOG_H
#define TGEN_LOG_H

#include <rte_log.h>

/* Log-type identifiers */
#define TGEN_LOG_MAIN   RTE_LOGTYPE_USER1
#define TGEN_LOG_PORT   RTE_LOGTYPE_USER2
#define TGEN_LOG_CC     RTE_LOGTYPE_USER3
#define TGEN_LOG_PP     RTE_LOGTYPE_USER4
#define TGEN_LOG_SYN    RTE_LOGTYPE_USER5
#define TGEN_LOG_HTTP   RTE_LOGTYPE_USER6
#define TGEN_LOG_TLS    RTE_LOGTYPE_USER7
#define TGEN_LOG_MGMT   RTE_LOGTYPE_USER8

/* Convenience wrappers — call rte_log() directly to avoid RTE_LOG's
 * token-pasting of the log-type macro, which breaks when the type
 * argument is already a numeric constant (e.g. RTE_LOGTYPE_USER7 = 30). */
#define TGEN_LOG(level, type, fmt, ...) \
    rte_log(RTE_LOG_ ## level, (type), "[%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__)

#define TGEN_ERR(type, fmt, ...)    TGEN_LOG(ERR,     type, fmt, ##__VA_ARGS__)
#define TGEN_WARN(type, fmt, ...)   TGEN_LOG(WARNING, type, fmt, ##__VA_ARGS__)
#define TGEN_INFO(type, fmt, ...)   TGEN_LOG(INFO,    type, fmt, ##__VA_ARGS__)
#define TGEN_DEBUG(type, fmt, ...)  TGEN_LOG(DEBUG,   type, fmt, ##__VA_ARGS__)

/**
 * Set the global DPDK log level for all tgen log types.
 * @param level  RTE_LOG_ERR / RTE_LOG_WARNING / RTE_LOG_INFO / RTE_LOG_DEBUG
 */
void tgen_log_set_level(uint32_t level);

#endif /* TGEN_LOG_H */
