/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Runtime configuration state.
 */
#include "config_mgr.h"
#include <string.h>

/* Global configuration — populated from binary args in main.c */
tgen_config_t g_config = {
    .tls_enabled    = false,
    .rest_port      = 0,
    .max_concurrent = 5000,
};
