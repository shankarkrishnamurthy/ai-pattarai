/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: EAL initialisation implementation.
 */
#include "eal_init.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

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
    { "max-conn",               required_argument, NULL, 'X' },
    { "rest-port",              required_argument, NULL, 'R' },
    { "src-ip",                 required_argument, NULL, 'I' },
    { "gateway",                required_argument, NULL, 'G' },
    { "netmask",                required_argument, NULL, 'N' },
    { "sslkeylog",              required_argument, NULL, 'K' },
    { "verbose",                no_argument,       NULL, 'v' },
    { "server",                 no_argument,       NULL, 'S' },
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
    a->max_conn               = 5000;  /* default max connections per worker */
    a->rest_port              = 0;
    a->src_ip                 = 0;
    a->gateway                = 0;
    a->netmask                = 0;

    /* We do not disturb optind for EAL — scan manually. */
    int saved_optind = optind;
    optind = 1;
    opterr = 0; /* suppress errors for unknown options (belong to EAL) */

    while ((opt = getopt_long(argc, argv, "W:M:P:r:t:d:C:X:R:I:G:N:K:vS", g_long_opts,
                              &opt_idx)) != -1) {
        switch (opt) {
        case 'W': a->num_worker_cores = (uint32_t)atoi(optarg); break;
        case 'M': a->num_mgmt_cores   = (uint32_t)atoi(optarg); break;
        case 'P': a->core_assignment_policy = optarg; break;
        case 'r': a->num_rx_desc      = (uint32_t)atoi(optarg); break;
        case 't': a->num_tx_desc      = (uint32_t)atoi(optarg); break;
        case 'd': a->pipeline_depth   = (uint32_t)atoi(optarg); break;
        case 'C': a->max_chain_depth  = (uint32_t)atoi(optarg); break;
        case 'X': a->max_conn         = (uint32_t)atoi(optarg); break;
        case 'R': a->rest_port        = (uint16_t)atoi(optarg); break;
        case 'I':
            if (tgen_parse_ipv4(optarg, &a->src_ip) < 0) {
                fprintf(stderr, "[TGEN] invalid --src-ip: %s\n", optarg);
                return -1;
            }
            break;
        case 'G':
            if (tgen_parse_ipv4(optarg, &a->gateway) < 0) {
                fprintf(stderr, "[TGEN] invalid --gateway: %s\n", optarg);
                return -1;
            }
            break;
        case 'N':
            if (tgen_parse_ipv4(optarg, &a->netmask) < 0) {
                fprintf(stderr, "[TGEN] invalid --netmask: %s\n", optarg);
                return -1;
            }
            break;
        case 'K':
            snprintf(a->sslkeylog_path, sizeof(a->sslkeylog_path),
                     "%s", optarg);
            break;
        case 'v': a->verbose = true; break;
        case 'S': a->server_mode = true; break;
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

    /* Pre-scan argv to decide what to inject into EAL args.
     * --log-level and --file-prefix only appear before the '--' separator.
     * -v/--verbose may appear anywhere (it is a tgen option after '--'). */
    bool has_prefix    = false;
    bool has_log_level = false;
    bool verbose       = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            break;
        if (strcmp(argv[i], "--file-prefix") == 0)
            has_prefix = true;
        else if (strcmp(argv[i], "--log-level") == 0)
            has_log_level = true;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
            break;
        }
    }

    /* Build eal_argv, optionally injecting --file-prefix and --log-level.
     * Max injected args: 4 (--file-prefix <pfx> --log-level warning). */
    static char prefix_buf[64];
    char *eal_argv[136];
    int   eal_argc = 0;

    eal_argv[eal_argc++] = argv[0];

    if (!has_prefix) {
        snprintf(prefix_buf, sizeof(prefix_buf), "vaigai_%d", (int)getpid());
        eal_argv[eal_argc++] = (char *)"--file-prefix";
        eal_argv[eal_argc++] = prefix_buf;
    }

    /* In quiet mode (default), cap EAL log output at WARNING so the
     * "EAL: Detected CPU lcores" family of messages is suppressed.
     * The user can pass --log-level explicitly to override. */
    if (!verbose && !has_log_level) {
        eal_argv[eal_argc++] = (char *)"--log-level";
        eal_argv[eal_argc++] = (char *)"warning";
    }

    for (int i = 1; i < argc && eal_argc < 136; i++)
        eal_argv[eal_argc++] = argv[i];

    /* Standard DPDK convention: EAL consumes everything up to '--',
     * then returns the count of args it consumed.  Application-specific
     * options (-W, -M, -r, -t …) must come AFTER the '--' separator.  */
    int eal_consumed = rte_eal_init(eal_argc, eal_argv);
    if (eal_consumed < 0) {
        fprintf(stderr, "[TGEN] rte_eal_init failed: %s\n",
                rte_strerror(rte_errno));
        return -1;
    }

    /* Parse tgen-specific options from the remaining arguments.
     * rte_eal_init() consumed [0 .. eal_consumed-1]; application args follow.
     * Use eal_argv/eal_argc (not the original argv/argc) because we may have
     * injected --file-prefix, shifting indices by 2.
     * getopt expects argv[0] to be the program name so we prepend a placeholder. */
    int    app_argc  = eal_argc - eal_consumed;
    char **app_raw   = eal_argv + eal_consumed;

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
