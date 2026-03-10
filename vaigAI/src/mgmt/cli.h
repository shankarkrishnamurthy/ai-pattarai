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
 * @param usage  Multi-line usage string shown by "help <cmd>" (NULL = none)
 * @param fn     Callback: fn(argc, argv)
 */
typedef void (*cli_cmd_fn_t)(int argc, char **argv);
void cli_register(const char *name, const char *help, const char *usage,
                  cli_cmd_fn_t fn);

/** Print current statistics to stdout. */
void cli_print_stats(void);

/**
 * Connect to a running vaigai process as a remote CLI client.
 * Called when `vaigai --attach` is used (no EAL init).
 * @param sock_path  Socket path, or NULL for auto-detect.
 * @return Exit code (0 = clean disconnect, 1 = error).
 */
int cli_attach_client(const char *sock_path);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_CLI_H */
