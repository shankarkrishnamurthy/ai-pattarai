/* SPDX-License-Identifier: BSD-3-Clause */
#include "log.h"
#include <rte_log.h>

void
tgen_log_set_level(uint32_t level)
{
    static const uint32_t types[] = {
        TGEN_LOG_MAIN, TGEN_LOG_PORT, TGEN_LOG_CC,  TGEN_LOG_PP,
        TGEN_LOG_SYN,  TGEN_LOG_HTTP, TGEN_LOG_TLS, TGEN_LOG_MGMT,
    };
    for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++)
        rte_log_set_level(types[i], level);
}
