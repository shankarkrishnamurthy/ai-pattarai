/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Unix domain socket CLI server.
 *
 * Allows multiple remote CLI clients to connect to a running vaigai
 * process and execute commands.  Output is captured via open_memstream()
 * and sent back over the socket with a NUL terminator.
 */
#ifndef TGEN_CLI_SERVER_H
#define TGEN_CLI_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLI_SERVER_MAX_CLIENTS  8
#define CLI_SERVER_DEFAULT_PATH "/var/run/vaigai/vaigai.sock"
#define CLI_SERVER_FALLBACK_PATH "/tmp/vaigai.sock"

/**
 * Initialise the CLI server: create and bind a Unix domain socket.
 * Tries 'sock_path' first; if NULL or the directory doesn't exist,
 * falls back to /tmp/vaigai.sock.
 *
 * @param sock_path  Desired socket path, or NULL for default.
 * @return 0 on success, -1 on error.
 */
int cli_server_init(const char *sock_path);

/**
 * Poll for new connections and incoming commands from remote clients.
 * Dispatches commands via the provided callback (typically the CLI
 * dispatch function), captures stdout output, and sends it back.
 *
 * @param dispatch_fn  Function to call with (line) for each command.
 *                     All printf() output during dispatch is captured.
 * @return Number of commands processed.
 */
typedef void (*cli_dispatch_fn_t)(char *line);
int cli_server_poll(cli_dispatch_fn_t dispatch_fn);

/**
 * Return the listener fd for inclusion in external poll() arrays.
 * Returns -1 if server is not initialised.
 */
int cli_server_fd(void);

/**
 * Return the actual socket path used (after fallback resolution).
 */
const char *cli_server_path(void);

/**
 * Shut down the server: close all client connections, close and
 * unlink the listener socket.
 */
void cli_server_destroy(void);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_CLI_SERVER_H */
