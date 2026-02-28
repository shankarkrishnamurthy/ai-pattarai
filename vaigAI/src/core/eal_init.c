/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: EAL initialisation implementation.
 */
#include "eal_init.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_errno.h>

#include "../common/types.h"
#include "../common/util.h"

/* ── Defaults ─────────────────────────────────────────────────────────────── */
#define DEFAULT_RX_DESC        TGEN_DEFAULT_RX_DESC
#define DEFAULT_TX_DESC        TGEN_DEFAULT_TX_DESC
#define DEFAULT_PIPELINE_DEPTH 16
#define DEFAULT_CHAIN_DEPTH    TGEN_MAX_CHAIN_DEPTH

/* ── Long options ─────────────────────────────────────────────────────────── */
static const struct option g_long_opts[] = {
    { "num-worker-cores",       required_argument, NULL, 'W' },
    { "num-mgmt-cores",         required_argument, NULL, 'M' },
    { "core-assignment-policy", required_argument, NULL, 'P' },
    { "rx-descs",               required_argument, NULL, 'r' },
    { "tx-descs",               required_argument, NULL, 't' },
    { "pipeline-depth",         required_argument, NULL, 'd' },
    { "max-chain-depth",        required_argument, NULL, 'C' },
    { NULL, 0, NULL, 0 },
};

/* ── Parse tgen-specific options before EAL ──────────────────────────────── */
static int parse_tgen_args(int argc, char **argv, tgen_eal_args_t *a)
{
    int opt;
    int opt_idx = 0;

    /* Defaults */
    a->num_worker_cores       = 0;
    a->num_mgmt_cores         = 0;
    a->core_assignment_policy = "auto";
    a->lcores_map             = NULL;
    a->main_lcore             = 0;
    a->num_rx_desc            = DEFAULT_RX_DESC;
    a->num_tx_desc            = DEFAULT_TX_DESC;
    a->pipeline_depth         = DEFAULT_PIPELINE_DEPTH;
    a->max_chain_depth        = DEFAULT_CHAIN_DEPTH;
    a->num_extra_eal_args     = 0;

    /* We do not disturb optind for EAL — scan manually. */
    int saved_optind = optind;
    optind = 1;
    opterr = 0; /* suppress errors for unknown options (belong to EAL) */

    while ((opt = getopt_long(argc, argv, "W:M:P:r:t:d:C:", g_long_opts,
                              &opt_idx)) != -1) {
        switch (opt) {
        case 'W': a->num_worker_cores = (uint32_t)atoi(optarg); break;
        case 'M': a->num_mgmt_cores   = (uint32_t)atoi(optarg); break;
        case 'P': a->core_assignment_policy = optarg; break;
        case 'r': a->num_rx_desc      = (uint32_t)atoi(optarg); break;
        case 't': a->num_tx_desc      = (uint32_t)atoi(optarg); break;
        case 'd': a->pipeline_depth   = (uint32_t)atoi(optarg); break;
        case 'C': a->max_chain_depth  = (uint32_t)atoi(optarg); break;
        default:  break; /* unknown → EAL handles */
        }
    }
    optind = saved_optind;
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */
int tgen_eal_init(int argc, char **argv, tgen_eal_args_t *out_args)
{
    tgen_eal_args_t args;
    memset(&args, 0, sizeof(args));

    /* Standard DPDK convention: EAL consumes everything up to '--',
     * then returns the count of args it consumed.  Application-specific
     * options (-W, -M, -r, -t …) must come AFTER the '--' separator.  */
    int eal_consumed = rte_eal_init(argc, argv);
    if (eal_consumed < 0) {
        fprintf(stderr, "[TGEN] rte_eal_init failed: %s\n",
                rte_strerror(rte_errno));
        return -1;
    }

    /* Parse tgen-specific options from the remaining arguments.
     * rte_eal_init() consumed [0 .. eal_consumed-1]; application args follow.
     * getopt expects argv[0] to be the program name so we prepend a placeholder. */
    int    app_argc  = argc - eal_consumed;
    char **app_raw   = argv + eal_consumed;

    /* Build a wrapped argv with a fake program name at [0] */
    char *wrapped[64];
    static const char *prog = "vaigai";
    int wargc = (app_argc + 1 < 64) ? (app_argc + 1) : 64;
    wrapped[0] = (char *)(uintptr_t)prog;
    for (int i = 1; i < wargc; i++)
        wrapped[i] = app_raw[i - 1];

    if (parse_tgen_args(wargc, wrapped, &args) < 0)
        return -1;

    /* Validate manual mode constraints */
    if (args.core_assignment_policy &&
        strcmp(args.core_assignment_policy, "manual") == 0) {
        if (args.num_worker_cores == 0 || args.num_mgmt_cores == 0) {
            fprintf(stderr,
                "[TGEN] manual core-assignment policy requires "
                "--num-worker-cores >= 1 AND --num-mgmt-cores >= 1\n");
            return -1;
        }
    }

    /* Calibrate TSC once EAL timers are available */
    tgen_calibrate_tsc();

    if (out_args)
        *out_args = args;

    return eal_consumed;
}

void tgen_eal_cleanup(void)
{
    rte_eal_cleanup();
}
