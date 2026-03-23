/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Unix domain socket CLI server implementation.
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
#include <poll.h>
#include <fcntl.h>

#include <rte_log.h>
#include <rte_eal.h>
#include "../common/types.h"

/* ── Server state ─────────────────────────────────────────────────── */
static int  g_listen_fd = -1;
static int  g_client_fds[CLI_SERVER_MAX_CLIENTS];
static int  g_n_clients;
static char g_sock_path[256];

/* ── Helpers ──────────────────────────────────────────────────────── */
static void
set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void
close_client(int idx)
{
    if (idx < 0 || idx >= g_n_clients) return;
    close(g_client_fds[idx]);
    g_client_fds[idx] = g_client_fds[g_n_clients - 1];
    g_n_clients--;
}

/* ── Init ─────────────────────────────────────────────────────────── */
int
cli_server_init(const char *sock_path)
{
    const char *path = sock_path;
    char auto_path[256];

    /* Resolve path: prefer DPDK runtime dir (unique per --file-prefix),
     * then /var/run/vaigai/, then /tmp/. */
    if (!path || path[0] == '\0') {
        const char *rtdir = rte_eal_get_runtime_dir();
        if (rtdir && rtdir[0]) {
            snprintf(auto_path, sizeof(auto_path), "%s/vaigai.sock", rtdir);
            path = auto_path;
        } else {
            path = CLI_SERVER_DEFAULT_PATH;
        }
    }

    /* Try to create parent directory for default path */
    if (strcmp(path, CLI_SERVER_DEFAULT_PATH) == 0) {
        struct stat st;
        if (stat("/var/run/vaigai", &st) != 0) {
            if (mkdir("/var/run/vaigai", 0755) != 0) {
                RTE_LOG(INFO, TGEN,
                    "Cannot create /var/run/vaigai/, using %s\n",
                    CLI_SERVER_FALLBACK_PATH);
                path = CLI_SERVER_FALLBACK_PATH;
            }
        }
    }

    /* Remove stale socket */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        RTE_LOG(ERR, TGEN, "cli_server: socket(): %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        RTE_LOG(ERR, TGEN, "cli_server: bind(%s): %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    /* Allow group access for operators */
    chmod(path, 0660);

    if (listen(fd, CLI_SERVER_MAX_CLIENTS) < 0) {
        RTE_LOG(ERR, TGEN, "cli_server: listen(): %s\n", strerror(errno));
        close(fd);
        unlink(path);
        return -1;
    }

    set_nonblock(fd);

    g_listen_fd = fd;
    g_n_clients = 0;
    strncpy(g_sock_path, path, sizeof(g_sock_path) - 1);
    g_sock_path[sizeof(g_sock_path) - 1] = '\0';

    RTE_LOG(INFO, TGEN, "CLI server listening on %s\n", g_sock_path);
    return 0;
}

/* ── Poll ─────────────────────────────────────────────────────────── */
int
cli_server_poll(cli_dispatch_fn_t dispatch_fn)
{
    if (g_listen_fd < 0) return 0;

    int cmds = 0;

    /* Accept new connections (non-blocking) */
    for (;;) {
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) break;

        if (g_n_clients >= CLI_SERVER_MAX_CLIENTS) {
            const char *msg = "ERROR: max clients reached\0";
            (void)write(cfd, msg, strlen(msg) + 1);
            close(cfd);
            continue;
        }

        set_nonblock(cfd);
        g_client_fds[g_n_clients++] = cfd;
        RTE_LOG(INFO, TGEN, "CLI client connected (fd=%d, total=%d)\n",
                cfd, g_n_clients);
    }

    /* Check each client for incoming commands */
    for (int i = 0; i < g_n_clients; /* advance in body */) {
        char buf[1024];
        ssize_t n = recv(g_client_fds[i], buf, sizeof(buf) - 1, 0);

        if (n == 0) {
            /* Client disconnected */
            RTE_LOG(INFO, TGEN, "CLI client disconnected (fd=%d)\n",
                    g_client_fds[i]);
            close_client(i);
            continue;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                i++;
                continue;
            }
            /* Real error */
            close_client(i);
            continue;
        }

        buf[n] = '\0';
        /* Strip trailing newline(s) */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
            buf[--n] = '\0';

        if (n == 0) { i++; continue; }

        /* Handle disconnect commands locally */
        if (strcmp(buf, "quit") == 0 || strcmp(buf, "exit") == 0 ||
            strcmp(buf, "disconnect") == 0) {
            const char *bye = "Disconnected.\0";
            (void)send(g_client_fds[i], bye, strlen(bye) + 1, MSG_NOSIGNAL);
            close_client(i);
            continue;
        }

        /* Capture stdout output during dispatch */
        char *membuf = NULL;
        size_t membuf_sz = 0;
        FILE *memfp = open_memstream(&membuf, &membuf_sz);
        if (!memfp) { i++; continue; }

        FILE *old_stdout = stdout;
        stdout = memfp;

        dispatch_fn(buf);

        fflush(stdout);
        fclose(stdout);
        stdout = old_stdout;

        /* Send captured output + NUL terminator */
        if (membuf && membuf_sz > 0)
            (void)send(g_client_fds[i], membuf, membuf_sz, MSG_NOSIGNAL);
        /* NUL byte = end-of-response marker */
        char nul = '\0';
        (void)send(g_client_fds[i], &nul, 1, MSG_NOSIGNAL);

        free(membuf);
        cmds++;
        i++;
    }

    return cmds;
}

int
cli_server_fd(void)
{
    return g_listen_fd;
}

const char *
cli_server_path(void)
{
    return g_sock_path[0] ? g_sock_path : NULL;
}

void
cli_server_destroy(void)
{
    for (int i = 0; i < g_n_clients; i++)
        close(g_client_fds[i]);
    g_n_clients = 0;

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    if (g_sock_path[0]) {
        unlink(g_sock_path);
        g_sock_path[0] = '\0';
    }

    RTE_LOG(INFO, TGEN, "CLI server stopped\n");
}
