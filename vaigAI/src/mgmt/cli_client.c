/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Remote CLI client — connects to a running vaigai process
 *         via Unix domain socket.  Invoked by `vaigai --attach`.
 *
 * This runs WITHOUT DPDK EAL initialisation — it's a thin readline
 * client that sends commands and prints responses.
 */
#include "cli_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#ifdef HAVE_READLINE
# include <readline/readline.h>
# include <readline/history.h>
#endif

/**
 * Connect to a running vaigai process and run an interactive CLI.
 *
 * @param sock_path  Path to the Unix socket, or NULL for auto-detect.
 * @return Exit code (0 = clean disconnect, 1 = error).
 */
int
cli_attach_client(const char *sock_path)
{
    /* Resolve socket path */
    const char *path = sock_path;
    if (!path || path[0] == '\0') {
        struct stat st;
        if (stat(CLI_SERVER_DEFAULT_PATH, &st) == 0)
            path = CLI_SERVER_DEFAULT_PATH;
        else if (stat(CLI_SERVER_FALLBACK_PATH, &st) == 0)
            path = CLI_SERVER_FALLBACK_PATH;
        else {
            fprintf(stderr, "No vaigai socket found at %s or %s\n",
                    CLI_SERVER_DEFAULT_PATH, CLI_SERVER_FALLBACK_PATH);
            return 1;
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to %s: %s\n"
                "Is vaigai running?\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    printf("Connected to vaigai via %s\n", path);

    const char *prompt = "vaigai(remote)> ";

#ifdef HAVE_READLINE
    char *line;
    while ((line = readline(prompt)) != NULL) {
        if (*line) add_history(line);

        /* Local disconnect */
        if (strcmp(line, "disconnect") == 0 ||
            strcmp(line, "quit") == 0 ||
            strcmp(line, "exit") == 0) {
            /* Send so server cleans up */
            char msg[64];
            int n = snprintf(msg, sizeof(msg), "%s\n", line);
            (void)send(fd, msg, (size_t)n, 0);
            free(line);
            break;
        }

        /* Send command */
        size_t len = strlen(line);
        char *sendbuf = malloc(len + 2);
        if (!sendbuf) { free(line); break; }
        memcpy(sendbuf, line, len);
        sendbuf[len] = '\n';
        sendbuf[len + 1] = '\0';
        (void)send(fd, sendbuf, len + 1, 0);
        free(sendbuf);
        free(line);

        /* Read response until NUL byte */
        char buf[4096];
        for (;;) {
            ssize_t nr = recv(fd, buf, sizeof(buf), 0);
            if (nr <= 0) {
                printf("\nConnection closed by server.\n");
                close(fd);
                return 0;
            }
            /* Check for NUL terminator in received data */
            int found_nul = 0;
            for (ssize_t j = 0; j < nr; j++) {
                if (buf[j] == '\0') {
                    /* Print everything before the NUL */
                    if (j > 0)
                        fwrite(buf, 1, (size_t)j, stdout);
                    found_nul = 1;
                    break;
                }
            }
            if (found_nul)
                break;
            fwrite(buf, 1, (size_t)nr, stdout);
        }
        fflush(stdout);
    }
#else
    /* Fallback: fgets-based */
    char linebuf[1024];
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!fgets(linebuf, sizeof(linebuf), stdin))
            break;
        linebuf[strcspn(linebuf, "\n")] = '\0';
        if (linebuf[0] == '\0') continue;

        if (strcmp(linebuf, "disconnect") == 0 ||
            strcmp(linebuf, "quit") == 0 ||
            strcmp(linebuf, "exit") == 0) {
            char msg[64];
            int n = snprintf(msg, sizeof(msg), "%s\n", linebuf);
            (void)send(fd, msg, (size_t)n, 0);
            break;
        }

        /* Send command + newline */
        size_t len = strlen(linebuf);
        (void)send(fd, linebuf, len, 0);
        char nl = '\n';
        (void)send(fd, &nl, 1, 0);

        /* Read response until NUL byte */
        char buf[4096];
        for (;;) {
            ssize_t nr = recv(fd, buf, sizeof(buf), 0);
            if (nr <= 0) {
                printf("\nConnection closed by server.\n");
                close(fd);
                return 0;
            }
            int found_nul = 0;
            for (ssize_t j = 0; j < nr; j++) {
                if (buf[j] == '\0') {
                    if (j > 0)
                        fwrite(buf, 1, (size_t)j, stdout);
                    found_nul = 1;
                    break;
                }
            }
            if (found_nul)
                break;
            fwrite(buf, 1, (size_t)nr, stdout);
        }
        fflush(stdout);
    }
#endif

    printf("Disconnected.\n");
    close(fd);
    return 0;
}
