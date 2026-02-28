/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Readline-based interactive CLI (§5.3).
 *
 * Runs on a management thread.  Commands are dispatched synchronously;
 * config changes are pushed to workers via config_push_to_workers().
 */
#ifndef TGEN_CLI_H
#define TGEN_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the CLI REPL.  Blocks until the user types "quit" or "exit".
 * Should be called from the management thread after EAL init.
 */
void cli_run(void);

/**
 * Register an application-specific command.
 * @param name   Command name (e.g. "show-stats")
 * @param help   One-line help string
 * @param fn     Callback: fn(argc, argv)
 */
typedef void (*cli_cmd_fn_t)(int argc, char **argv);
void cli_register(const char *name, const char *help, cli_cmd_fn_t fn);

/** Print current statistics to stdout. */
void cli_print_stats(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_CLI_H */
