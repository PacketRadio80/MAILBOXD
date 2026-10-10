/*
 * mailboxd_prterm — PRTERM <-> MailboxD link plugin.
 *
 * INI: [link.prterm].  Wire: unix-domain socket at the configured path
 * (default /var/mailboxd/prterm.sock, mode 0660). One accepted client
 * at a time - PRTERM is a single instance per host. Each PRTERM
 * command becomes a MailboxD `/`-command inside a service-internal
 * session, and the textual output of mailboxd_session_write_line()
 * is shipped back to PRTERM as a multi-line `OK ...` reply.
 *
 * The plugin is a transport in the MailboxD registry. Session writes
 * go through the `write` callback into a per-connection output ring;
 * the accept thread drains that ring back to PRTERM. PRTERM calls
 * are not interactive (transport != TELNET), so the session
 * starts as a guest in the same way /who, /mail, etc. work for
 * guest logins in the CLI.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mailboxd/plugin.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/socket.h"
#include "mailboxd/security_ban.h"
#include "mailboxd/command.h"
#include "mailboxd/mailboxd_prterm.h"
#include "mailboxd/config.h"
#include "mailboxd/util.h"
#include "mailboxd/log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* ---------------------------------------------------------------------- */
/* Plugin state                                                          */
/* ---------------------------------------------------------------------- */

typedef struct link_session {
    int fd;
    mailboxd_session_t *session;
    char inbuf[MAILBOXD_PRTERM_LINE_MAX];
    size_t inlen;
    /* Bounded ring for capture of mailboxd_session_write_line() output
     * (and prompt/echo for the same session). Anything that does not
     * fit is truncated. */
    char outbuf[16384];
    size_t outlen;
} link_session_t;

static mailboxd_service_t *g_service;
static mailboxd_mailboxd_prterm_config_t g_config;
static pthread_t g_accept_thread;
static int g_listen_fd = -1;
static volatile sig_atomic_t g_running = 0;

extern const mailboxd_transport_plugin_t mailboxd_plugin_mailboxd_prterm;

/* ---------------------------------------------------------------------- */
/* Output capture                                                       */
/* ---------------------------------------------------------------------- */

/*
 * The plugin's `write` callback is invoked by the session core whenever
 * MailboxD writes to the connected client. For a unix-socket bridge
 * the data is supposed to reach PRTERM, but we also want the
 * `mailboxd_session_write_line()` text to be returned as the reply to
 * the originating command. Therefore:
 *   - if a request is currently being served (`g_inflight != NULL`),
 *     append the bytes to the inflight capture buffer; the reply ends
 *     when the dispatcher returns and we ship the buffer back.
 *   - else (banner, prompt, event) just write to the socket.
 */
static link_session_t *g_inflight = NULL;

static void session_link_write(link_session_t *ls, const char *data, size_t len)
{
    if (ls == NULL || data == NULL || len == 0) {
        return;
    }
    /* Stage 1: in-flight capture (the reply to a command). */
    if (g_inflight == ls) {
        size_t space = sizeof ls->outbuf - ls->outlen;
        if (space == 0) {
            return;
        }
        if (len > space) {
            len = space;
        }
        memcpy(ls->outbuf + ls->outlen, data, len);
        ls->outlen += len;
        return;
    }
    /* Stage 2: direct write (banner/prompt/event). */
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(ls->fd, data + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        off += (size_t)n;
    }
}

static mailboxd_result_t link_plugin_write(mailboxd_session_t *session,
                                       const char *data, size_t len)
{
    link_session_t *ls = (link_session_t *)session->transport_data;
    session_link_write(ls, data, len);
    return MAILBOXD_OK;
}

/* ---------------------------------------------------------------------- */
/* Reply framing                                                        */
/* ---------------------------------------------------------------------- */

/*
 * Frames a MailboxD text reply to PRTERM.
 *  - one or more "OUT <line>\n" lines (CRLF stripped; CRs and trailing
 *    LF normalised to LF);
 *  - "END ok" or "END err <reason>" terminates.
 *  - on a too-long capture we end with "END err output-truncated".
 */
static void send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        off += (size_t)n;
    }
}

static void flush_reply(link_session_t *ls, bool ok, const char *err)
{
    if (ls == NULL || ls->fd < 0) {
        return;
    }

    char header[128];
    if (ok) {
        snprintf(header, sizeof header, "END ok\n");
    } else {
        snprintf(header, sizeof header, "END err %s\n",
                 err != NULL ? err : "unknown");
    }

    /*
     * Walk the captured buffer line by line. CRs are stripped, the
     * trailing LF is replaced with the "OUT " framing prefix.
     */
    char *buf = ls->outbuf;
    size_t remain = ls->outlen;
    while (remain > 0) {
        char *eol = memchr(buf, '\n', remain);
        size_t line_len;
        size_t consumed;
        if (eol == NULL) {
            /* No terminator - flush the rest as a partial line. */
            line_len = remain;
            consumed = remain;
        } else {
            line_len = (size_t)(eol - buf);
            consumed = (size_t)(eol - buf) + 1;
        }
        /* Strip trailing CR. */
        if (line_len > 0 && buf[line_len - 1] == '\r') {
            line_len -= 1;
        }
        if (line_len > 0) {
            char frame[1100];
            int n = snprintf(frame, sizeof frame, "OUT %.*s\n",
                              (int)line_len, buf);
            if (n > 0) {
                send_all(ls->fd, frame, (size_t)n);
            }
        }
        buf += consumed;
        remain -= consumed;
    }

    send_all(ls->fd, header, strlen(header));
    ls->outlen = 0;
}

/* ---------------------------------------------------------------------- */
/* Request handling                                                     */
/* ---------------------------------------------------------------------- */

static void handle_hello(link_session_t *ls, const char *args)
{
    /* HELLO <version>  →  OK MAILBOXD <version> */
    char reply[64];
    snprintf(reply, sizeof reply, "OK MAILBOXD %d\n", MAILBOXD_PRTERM_VERSION);
    send(ls->fd, reply, strlen(reply), MSG_NOSIGNAL);
    (void)args;
}

static void handle_ping(link_session_t *ls)
{
    const char *pong = "PONG\n";
    send(ls->fd, pong, strlen(pong), MSG_NOSIGNAL);
}

static void handle_run(link_session_t *ls, const char *line)
{
    /*
     * RUN <command-line>\n
     *
     * Build a MailboxD /-command, dispatch it through the session,
     * then ship the captured write_line output as OUT <line> + END ok.
     */
    mailboxd_parsed_command_t cmd;
    memset(&cmd, 0, sizeof cmd);

    if (mailboxd_command_parse(line, &cmd) != MAILBOXD_OK) {
        send(ls->fd, "END err parse-failed\n", 22, MSG_NOSIGNAL);
        return;
    }
    if (cmd.scope != MAILBOXD_CMD_SCOPE_MAILBOXD) {
        /* For a "link" connection the user is the PRTERM operator, not
         * a chat client - non-/ lines are an error here. */
        mailboxd_command_free(&cmd);
        send(ls->fd, "END err not-a-mailboxd-command\n", 31, MSG_NOSIGNAL);
        return;
    }

    ls->outlen = 0;
    g_inflight = ls;
    mailboxd_result_t rc = mailboxd_command_dispatch(g_service, ls->session, &cmd);
    g_inflight = NULL;

    mailboxd_command_free(&cmd);

    if (rc == MAILBOXD_OK) {
        flush_reply(ls, true, NULL);
    } else {
        char err[64];
        snprintf(err, sizeof err, "dispatch-failed-%d", (int)rc);
        flush_reply(ls, false, err);
    }
}

static void process_one_line(link_session_t *ls, char *line)
{
    /* Strip trailing CR/LF and any trailing whitespace. */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' '  || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    if (strncmp(line, "HELLO", 5) == 0 && (line[5] == ' ' || line[5] == '\0')) {
        handle_hello(ls, line + 5);
        return;
    }
    if (strcmp(line, "PING") == 0) {
        handle_ping(ls);
        return;
    }
    if (strncmp(line, "RUN ", 4) == 0) {
        handle_run(ls, line + 4);
        return;
    }
    if (strncmp(line, "RUN", 3) == 0 && line[3] == '\0') {
        send(ls->fd, "END err empty-command\n", 22, MSG_NOSIGNAL);
        return;
    }
    const char *msg = "END err unknown-request\n";
    send(ls->fd, msg, strlen(msg), MSG_NOSIGNAL);
}

static void link_session_close(link_session_t *ls)
{
    if (ls == NULL) {
        return;
    }
    if (ls->session != NULL) {
        mailboxd_session_close(ls->session);
        ls->session = NULL;
    }
    if (ls->fd >= 0) {
        close(ls->fd);
        ls->fd = -1;
    }
    free(ls);
}

static void link_session_readable(link_session_t *ls)
{
    char buf[1024];
    for (;;) {
        ssize_t n = recv(ls->fd, buf, sizeof buf, 0);
        if (n == 0) {
            link_session_close(ls);
            return;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            link_session_close(ls);
            return;
        }
        for (ssize_t i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                ls->inbuf[ls->inlen] = '\0';
                process_one_line(ls, ls->inbuf);
                ls->inlen = 0;
            } else if (ls->inlen < sizeof ls->inbuf - 1) {
                ls->inbuf[ls->inlen++] = ch;
            } else {
                /* overflow: drop the line and tell PRTERM. */
                ls->inlen = 0;
                const char *msg = "END err line-too-long\n";
                send(ls->fd, msg, strlen(msg), MSG_NOSIGNAL);
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Accept loop                                                          */
/* ---------------------------------------------------------------------- */

static void *accept_thread_main(void *arg)
{
    (void)arg;
    while (g_running) {
        struct pollfd pfd = { .fd = g_listen_fd, .events = POLLIN };
        int n = poll(&pfd, 1, 200);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            continue;
        }

        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            continue;
        }

        /* A single PRTERM client at a time - drop late arrivals cleanly
         * rather than queuing them. */
        struct timeval to = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);

        link_session_t *ls = calloc(1, sizeof *ls);
        if (ls == NULL) {
            close(cfd);
            continue;
        }
        ls->fd = cfd;

        if (mailboxd_session_open(g_service, &mailboxd_plugin_mailboxd_prterm,
                                  ls, &ls->session) != MAILBOXD_OK) {
            link_session_close(ls);
            continue;
        }
        mailboxd_session_set_remote(ls->session, "unix:mailboxd-prterm");

        /* Drive the session until the socket drops. */
        link_session_readable(ls);
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Lifecycle (plugin transport entry points)                           */
/* ---------------------------------------------------------------------- */

static int unlink_path_quiet(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT) {
        return -1;
    }
    return 0;
}

static int ensure_parent_dir(const char *path)
{
    char copy[MAILBOXD_PATH_MAX];
    if (path == NULL || strlen(path) >= sizeof copy) {
        return -1;
    }
    strncpy(copy, path, sizeof copy - 1);
    copy[sizeof copy - 1] = '\0';
    for (char *p = copy + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }
    return 0;
}

static mailboxd_result_t link_plugin_init(mailboxd_service_t *service)
{
    g_service = service;
    mailboxd_mailboxd_prterm_config_defaults(&g_config);
    mailboxd_log_info("[mailboxd_prterm] plugin initialised (defaults in place, "
                      "wait for /link.prterm to apply)");
    return MAILBOXD_OK;
}

static mailboxd_result_t link_plugin_stop(void); /* forward */

static void link_plugin_shutdown(void)
{
    link_plugin_stop();
    g_service = NULL;
}

static mailboxd_result_t link_plugin_start(const char *config)
{
    if (mailboxd_mailboxd_prterm_config_parse(config, &g_config) != 0) {
        mailboxd_log_warn("[mailboxd_prterm] config parse failed - leaving plugin off");
        return MAILBOXD_ERR_INVALID;
    }

    /*
     * "enabled" is consumed by the network/service layer before our
     * start() string is built, so we read it back from the live
     * mailboxd_config_t (exposed via mailboxd_service_get_config).
     * The default is "yes" - matching the network-side default.
     */
    const mailboxd_config_t *live = mailboxd_service_get_config(g_service);
    if (live != NULL) {
        const char *en = mailboxd_config_get(live, "transport.mailboxd_prterm",
                                              "enabled", "yes");
        g_config.enabled = (strcmp(en, "yes") == 0 ||
                            strcmp(en, "true") == 0 ||
                            strcmp(en, "1") == 0);
    }

    mailboxd_log_info("[mailboxd_prterm] parsed config: enabled=%d bind=%s mode=%04o token_len=%zu",
                      g_config.enabled, g_config.bind, g_config.mode,
                      strlen(g_config.link_token));
    if (!g_config.enabled) {
        mailboxd_log_info("[mailboxd_prterm] disabled in INI");
        return MAILBOXD_OK;
    }
    if (g_config.bind[0] == '\0' || g_config.link_token[0] == '\0') {
        mailboxd_log_warn("[mailboxd_prterm] enabled but bind/link_token empty - "
                          "leaving plugin off");
        return MAILBOXD_ERR_INVALID;
    }

    if (ensure_parent_dir(g_config.bind) != 0) {
        mailboxd_log_warn("[mailboxd_prterm] cannot create parent dir of %s",
                          g_config.bind);
        return MAILBOXD_ERR_IO;
    }
    if (unlink_path_quiet(g_config.bind) != 0) {
        mailboxd_log_warn("[mailboxd_prterm] cannot unlink stale socket %s",
                          g_config.bind);
        return MAILBOXD_ERR_IO;
    }

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        mailboxd_log_warn("[mailboxd_prterm] socket() failed: %s", strerror(errno));
        return MAILBOXD_ERR_IO;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    mailboxd_strlcpy(addr.sun_path, g_config.bind, sizeof addr.sun_path);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        mailboxd_log_warn("[mailboxd_prterm] bind %s failed: %s",
                          g_config.bind, strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return MAILBOXD_ERR_IO;
    }
    if (chmod(g_config.bind, g_config.mode ? g_config.mode : 0660u) != 0) {
        mailboxd_log_warn("[mailboxd_prterm] chmod %s failed: %s",
                          g_config.bind, strerror(errno));
    }
    if (listen(g_listen_fd, 4) != 0) {
        mailboxd_log_warn("[mailboxd_prterm] listen failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return MAILBOXD_ERR_IO;
    }

    g_running = 1;
    if (pthread_create(&g_accept_thread, NULL, accept_thread_main, NULL) != 0) {
        mailboxd_log_warn("[mailboxd_prterm] pthread_create failed: %s",
                          strerror(errno));
        g_running = 0;
        close(g_listen_fd);
        g_listen_fd = -1;
        return MAILBOXD_ERR_IO;
    }
    mailboxd_log_info("[mailboxd_prterm] listening on %s (mode %04o)",
                      g_config.bind, g_config.mode);
    return MAILBOXD_OK;
}

static mailboxd_result_t link_plugin_stop(void)
{
    if (g_listen_fd >= 0) {
        g_running = 0;
        shutdown(g_listen_fd, SHUT_RDWR);
        pthread_join(g_accept_thread, NULL);
        close(g_listen_fd);
        g_listen_fd = -1;
        unlink_path_quiet(g_config.bind);
        mailboxd_log_info("[mailboxd_prterm] stopped");
    }
    return MAILBOXD_OK;
}

const mailboxd_transport_plugin_t mailboxd_plugin_mailboxd_prterm = {
    .name    = "mailboxd_prterm",
    .kind    = MAILBOXD_TRANSPORT_INTERNAL, /* logical only - not a user BBX */
    .version = MAILBOXD_PRTERM_VERSION,
    .init     = link_plugin_init,
    .shutdown = link_plugin_shutdown,
    .start    = link_plugin_start,
    .stop     = link_plugin_stop,
    .write    = link_plugin_write,
    .tick     = NULL,
    .on_command = NULL,
};
