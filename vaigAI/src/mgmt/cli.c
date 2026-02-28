/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: CLI implementation (readline when available, fallback fgets).
 */
#include "cli.h"
#include "config_mgr.h"
#include "../net/icmp.h"
#include "../telemetry/pktrace.h"
#include "../common/util.h"
#include "../telemetry/metrics.h"
#include "../telemetry/export.h"
#include "../telemetry/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#ifdef HAVE_READLINE
# include <readline/readline.h>
# include <readline/history.h>
#endif

#define MAX_CMDS  64u
#define MAX_ARGS  16u

typedef struct {
    char           name[64];
    char           help[128];
    cli_cmd_fn_t   fn;
} cli_entry_t;

static cli_entry_t g_cmds[MAX_CMDS];
static uint32_t    g_n_cmds;

/* ------------------------------------------------------------------ */
/* Built-in commands                                                    */
/* ------------------------------------------------------------------ */
static void
cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Available commands:\n");
    for (uint32_t i = 0; i < g_n_cmds; i++)
        printf("  %-24s  %s\n", g_cmds[i].name, g_cmds[i].help);
}

static void
cmd_stats(int argc, char **argv)
{
    (void)argc; (void)argv;
    cli_print_stats();
}

static void
cmd_load(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: load <config.json>\n"); return; }
    int rc = config_load_json(argv[1]);
    if (rc < 0) printf("Load failed: %s\n", strerror(-rc));
    else        printf("Config loaded from %s\n", argv[1]);
}

static void
cmd_save(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: save <config.json>\n"); return; }
    int rc = config_save_json(argv[1]);
    if (rc < 0) printf("Save failed: %s\n", strerror(-rc));
    else        printf("Config saved to %s\n", argv[1]);
}

static void
cmd_set_cps(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: set-cps <value>\n"); return; }
    g_config.load.target_cps = (uint64_t)strtoull(argv[1], NULL, 10);
    config_push_to_workers();
    printf("target_cps = %"PRIu64"\n", g_config.load.target_cps);
}

static void
cmd_trace(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: trace start [port=0] [queue=0] [count=100]\n"
               "       trace stop\n"
               "       trace save <file.pcapng>\n");
        return;
    }
    if (strcmp(argv[1], "start") == 0) {
        uint16_t port  = (argc >= 3) ? (uint16_t)strtoul(argv[2], NULL, 10) : 0;
        uint16_t queue = (argc >= 4) ? (uint16_t)strtoul(argv[3], NULL, 10) : 0;
        uint32_t count = (argc >= 5) ? (uint32_t)strtoul(argv[4], NULL, 10) : 100;
        int rc = pktrace_start(port, queue, count);
        if (rc == 0)
            printf("Capture started on port %u queue %u (max %u pkts)\n",
                   port, queue, count);
        else
            printf("trace start failed: %s\n", strerror(-rc));
    } else if (strcmp(argv[1], "stop") == 0) {
        pktrace_stop();
        printf("Capture stopped (%u packets in ring)\n", pktrace_count());
    } else if (strcmp(argv[1], "save") == 0) {
        if (argc < 3) { printf("Usage: trace save <file.pcapng>\n"); return; }
        int n = pktrace_save(argv[2]);
        if (n >= 0)
            printf("Saved %d packets → %s\n", n, argv[2]);
        else
            printf("trace save failed: %s\n", strerror(-n));
    } else {
        printf("Unknown trace sub-command '%s'\n", argv[1]);
    }
}

static void
cmd_ping(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: ping <dst_ip> [count=5] [size=56] [interval_ms=1000]\n");
        return;
    }
    uint32_t dst_ip = 0;
    if (tgen_parse_ipv4(argv[1], &dst_ip) < 0) {
        printf("ping: invalid IP address '%s'\n", argv[1]);
        return;
    }
    uint32_t count       = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 10) : 5;
    uint32_t size        = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 56;
    uint32_t interval_ms = (argc >= 5) ? (uint32_t)strtoul(argv[4], NULL, 10) : 1000;
    uint16_t port_id     = 0; /* always use port 0 for diagnostic ping */
    printf("PING %s: %u bytes of data, %u packet(s)\n", argv[1], size, count);
    icmp_ping_start(port_id, dst_ip, count, size, interval_ms);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */
void
cli_register(const char *name, const char *help, cli_cmd_fn_t fn)
{
    if (g_n_cmds >= MAX_CMDS) return;
    strncpy(g_cmds[g_n_cmds].name, name, sizeof(g_cmds[0].name)-1);
    strncpy(g_cmds[g_n_cmds].help, help, sizeof(g_cmds[0].help)-1);
    g_cmds[g_n_cmds].fn = fn;
    g_n_cmds++;
}

void
cli_print_stats(void)
{
    metrics_snapshot_t snap;
    metrics_snapshot(&snap, TGEN_MAX_WORKERS);
    char buf[4096];
    export_json(&snap, buf, sizeof(buf));
    puts(buf);
}

static void
dispatch(char *line)
{
    /* Tokenise */
    char *argv[MAX_ARGS];
    int   argc = 0;
    char *tok  = strtok(line, " \t\r\n");
    while (tok && (uint32_t)argc < MAX_ARGS) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    if (argc == 0) return;

    for (uint32_t i = 0; i < g_n_cmds; i++) {
        if (strcmp(argv[0], g_cmds[i].name) == 0) {
            g_cmds[i].fn(argc, argv);
            return;
        }
    }
    printf("Unknown command: %s (type 'help' for list)\n", argv[0]);
}

void
cli_run(void)
{
    extern volatile int g_run;

    /* Register built-ins */
    cli_register("help",     "Show this help",           cmd_help);
    cli_register("stats",    "Print current statistics", cmd_stats);
    cli_register("load",     "Load config JSON file",    cmd_load);
    cli_register("save",     "Save config JSON file",    cmd_save);
    cli_register("set-cps",  "Set target connections/s", cmd_set_cps);
    cli_register("ping",     "ICMP ping: ping <ip> [count] [size] [ms]", cmd_ping);
    cli_register("trace",    "Packet capture: trace start/stop/save",    cmd_trace);

    /* Detect daemon mode: stdin is /dev/null (char device, non-tty).
     * When launched as a background daemon (nohup ... </dev/null),
     * skip the interactive loop and block until g_run becomes 0.
     * When stdin is a pipe (subprocess.run with input=), process normally. */
    struct stat st_in;
    if (fstat(STDIN_FILENO, &st_in) == 0 && S_ISCHR(st_in.st_mode) &&
        !isatty(STDIN_FILENO)) {
        TGEN_INFO(TGEN_LOG_MGMT,
                  "stdin is /dev/null — running in headless mode "
                  "(send SIGTERM to stop)\n");
        while (g_run) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100 ms */
            nanosleep(&ts, NULL);
        }
        return;
    }

    const char *prompt = g_config.cli_prompt[0] ? g_config.cli_prompt : "tgen> ";
    if (isatty(STDIN_FILENO))
        printf("vaigai CLI  (type 'help' for commands, 'quit' to exit)\n");

#ifdef HAVE_READLINE
    rl_bind_key('\t', rl_complete);
    char *line;
    while ((line = readline(prompt)) != NULL) {
        if (*line) add_history(line);
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            free(line); break;
        }
        dispatch(line);
        free(line);
    }
#else
    char buf[1024];
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin)) break;
        buf[strcspn(buf, "\n")] = '\0';
        if (strcmp(buf, "quit") == 0 || strcmp(buf, "exit") == 0) break;
        dispatch(buf);
    }
#endif
}
