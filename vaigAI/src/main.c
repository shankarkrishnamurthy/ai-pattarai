/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Main entry point.
 *
 * Startup sequence (§1.1):
 *  1.  Parse CLI → tgen_eal_init()
 *  2.  Calibrate TSC
 *  3.  Core assignment
 *  4.  Mempool creation
 *  5.  Port initialisation
 *  6.  IPC ring creation
 *  7.  Load configuration (JSON)
 *  8.  TLS context init
 *  9.  TCP state init (TCB stores, port pools)
 * 10.  Cryptodev init
 * 11.  Launch worker lcores
 * 12.  Start REST server
 * 13.  Run CLI (blocks until "quit")
 * 14.  Tear down
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>

#include "common/types.h"
#include "common/util.h"
#include "core/eal_init.h"
#include "core/core_assign.h"
#include "core/mempool.h"
#include "core/ipc.h"
#include "core/worker_loop.h"
#include "port/port_init.h"
#include "port/soft_nic.h"
#include "net/tcp_tcb.h"
#include "net/tcp_port_pool.h"
#include "net/arp.h"
#include "net/icmp.h"
#include "net/udp.h"
#include "tls/cert_mgr.h"
#include "tls/tls_session.h"
#include "tls/cryptodev.h"
#include "telemetry/log.h"
#include "telemetry/metrics.h"
#include "telemetry/pktrace.h"
#include "mgmt/config_mgr.h"
#include "mgmt/cli.h"
#include "mgmt/rest.h"

/* ------------------------------------------------------------------ */
/* Global run flag (workers poll this) — defined in worker_loop.c      */
/* ------------------------------------------------------------------ */
extern volatile int g_run;

/* ------------------------------------------------------------------ */
/* TLS global contexts                                                  */
/* ------------------------------------------------------------------ */
static tls_ctx_t g_tls_client;
static tls_ctx_t g_tls_server;

/* ------------------------------------------------------------------ */
/* Signal handling                                                      */
/* ------------------------------------------------------------------ */
static void
sigint_handler(int sig)
{
    (void)sig;
    printf("\nSIGINT received — stopping workers...\n");
    g_run = 0;
}

/* ------------------------------------------------------------------ */
/* Worker launch wrapper                                                */
/* ------------------------------------------------------------------ */
static int
worker_launch(void *arg)
{
    return tgen_worker_loop(arg);
}

/* ------------------------------------------------------------------ */
/* Teardown                                                             */
/* ------------------------------------------------------------------ */
static void
tgen_cleanup(void)
{
    RTE_LOG(INFO, USER1, "Stopping management servers...\n");
    rest_server_stop();

    RTE_LOG(INFO, USER1, "Waiting for workers to stop...\n");
    rte_eal_mp_wait_lcore();

    RTE_LOG(INFO, USER1, "Releasing resources...\n");
    pktrace_destroy();
    tcp_port_pool_fini();
    tls_session_store_fini();
    cryptodev_fini();
    icmp_destroy();
    udp_destroy();
    arp_destroy();
    cert_mgr_fini(&g_tls_client, &g_tls_server);

    for (uint32_t i = 0; i < g_n_ports; i++)
        rte_eth_dev_stop(i);

    tgen_ipc_destroy();
    tgen_mempool_destroy_all();
    tgen_eal_cleanup();

    RTE_LOG(INFO, USER1, "vaigai stopped cleanly.\n");
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */
int
main(int argc, char **argv)
{
    int rc;

    /* ---- 1. EAL + custom argument parsing ---- */
    tgen_eal_args_t eal_args;
    rc = tgen_eal_init(argc, argv, &eal_args);
    if (rc < 0) {
        fprintf(stderr, "EAL init failed: %s\n", strerror(-rc));
        return EXIT_FAILURE;
    }

    /* ---- 2. TSC calibration (done inside tgen_eal_init) ---- */
    RTE_LOG(INFO, USER1, "TSC frequency: %"PRIu64" Hz\n", g_tsc_hz);

    /* ---- 3. Core assignment ---- */
    rc = tgen_core_assign_init(eal_args.num_worker_cores,
                               eal_args.num_mgmt_cores,
                               false,
                               rte_eth_dev_count_avail());
    if (rc < 0) {
        RTE_LOG(ERR, USER1, "Core assignment failed\n");
        goto fail_eal;
    }
    RTE_LOG(INFO, USER1, "Workers: %u  Management: %u\n",
            g_core_map.num_workers, g_core_map.num_mgmt);

    /* ---- 4. Mempool ---- */
    rc = tgen_mempool_create_all(eal_args.num_rx_desc,
                                  eal_args.num_tx_desc,
                                  eal_args.pipeline_depth,
                                  1);
    if (rc < 0) {
        RTE_LOG(ERR, USER1, "Mempool creation failed\n");
        goto fail_eal;
    }

    /* ---- 5. Port initialisation ---- */
    rc = tgen_ports_init(eal_args.num_rx_desc, eal_args.num_tx_desc);
    if (rc < 0) {
        RTE_LOG(ERR, USER1, "Port init failed\n");
        goto fail_pools;
    }

    /* ---- 5a. ARP + ICMP subsystems ---- */
    rc = arp_init();
    if (rc < 0) {
        RTE_LOG(ERR, USER1, "ARP init failed\n");
        goto fail_ports;
    }
    rc = icmp_init();
    if (rc < 0) {
        RTE_LOG(ERR, USER1, "ICMP init failed\n");
        goto fail_ports;
    }
    rc = udp_init();
    if (rc < 0) {
        RTE_LOG(ERR, USER1, "UDP init failed\n");
        goto fail_ports;
    }
    rc = pktrace_init();
    if (rc < 0)
        RTE_LOG(WARNING, USER1, "pktrace init failed — capture disabled\n");

    /* ---- 6. IPC rings ---- */
    rc = tgen_ipc_init(g_core_map.num_workers);
    if (rc < 0) {
        RTE_LOG(ERR, USER1, "IPC init failed\n");
        goto fail_ports;
    }

    /* ---- 7. Load configuration ---- */
    const char *cfg_path = getenv("VAIGAI_CONFIG");
    if (!cfg_path) {
        RTE_LOG(ERR, USER1,
            "No config specified. Set VAIGAI_CONFIG or pass --config <file>\n");
        goto fail_ports;
    }
    rc = config_load_json(cfg_path);
    if (rc < 0) {
        RTE_LOG(WARNING, USER1,
                "Config load failed (%s) — using defaults\n", cfg_path);
        /* Set minimal default so validation passes */
        g_config.flows[0].dst_ip   = rte_cpu_to_be_32(RTE_IPV4(127,0,0,1));
        g_config.flows[0].dst_port = 80;
        g_config.n_flows           = 1;
    }
    /* Push config to ARP local_ip now that ports are up */
    config_push_to_workers();

    /* ---- 8. TLS contexts ---- */
    if (g_config.tls_enabled) {
        rc = cert_mgr_init(&g_config.cert, &g_tls_client, &g_tls_server);
        if (rc < 0) {
            RTE_LOG(WARNING, USER1, "TLS init failed — TLS disabled\n");
            g_config.tls_enabled = false;
        }
        if (g_config.tls_enabled) {
            rc = tls_session_store_init(&g_tls_client, &g_tls_server);
            if (rc < 0) goto fail_ipc;
        }
    }

    /* ---- 9. TCP subsystem ---- */
    rc = tcb_stores_init(g_config.load.max_concurrent);
    if (rc < 0) {
        RTE_LOG(ERR, USER1, "TCB store init failed\n");
        goto fail_tls;
    }

    rc = tcp_port_pool_init(g_core_map.num_workers);
    if (rc < 0) {
        RTE_LOG(ERR, USER1, "Port pool init failed\n");
        goto fail_tcb;
    }

    /* ---- 10. Cryptodev ---- */
    cryptodev_init(); /* failure is non-fatal — falls back to SW */

    /* ---- Signal handler ---- */
    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* ---- 11. Launch worker lcores ---- */
    g_run = 1;
    tgen_worker_ctx_init();
    for (uint32_t w = 0; w < g_core_map.num_workers; w++) {
        unsigned lcore = g_core_map.worker_lcores[w];
        if (rte_eal_remote_launch(worker_launch, &g_worker_ctx[w],
                                  lcore) < 0) {
            RTE_LOG(ERR, USER1, "Failed to launch worker %u on lcore %u\n",
                    w, lcore);
        }
    }
    RTE_LOG(INFO, USER1, "Launched %u worker lcores\n",
            g_core_map.num_workers);

    /* ---- 12. Management servers ---- */
    if (g_config.rest_port)
        rest_server_start(g_config.rest_port);

    /* ---- 13. CLI (blocks) ---- */
    cli_run();

    /* ---- 14. Tear down ---- */
    g_run = 0;
    tgen_cleanup();
    return EXIT_SUCCESS;

    /* Error paths */
fail_tcb:
    tcb_stores_destroy();
fail_tls:
    tls_session_store_fini();
    cert_mgr_fini(&g_tls_client, &g_tls_server);
fail_ipc:
    tgen_ipc_destroy();
fail_ports:
    for (uint32_t i = 0; i < g_n_ports; i++)
        rte_eth_dev_stop(i);
fail_pools:
    tgen_mempool_destroy_all();
fail_eal:
    tgen_eal_cleanup();
    return EXIT_FAILURE;
}
