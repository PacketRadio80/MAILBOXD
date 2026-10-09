/*
 * telnet — TCP transport plugin (port 2323). INI: [transport.telnet].
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mailboxd/plugin.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/socket.h"
#include "mailboxd/security_ban.h"
#include "mailboxd/telnet.h"
#include "telnet_proto.h"
#include "mailboxd/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct telnet_client {
    int fd;
    mailboxd_telnet_parser_t parser;
} telnet_client_t;

typedef struct telnet_client_ctx {
    mailboxd_session_t *session;
    mailboxd_result_t last_rc;
} telnet_client_ctx_t;

extern const mailboxd_transport_plugin_t mailboxd_plugin_telnet;

static mailboxd_service_t *g_service;
static mailboxd_telnet_config_t g_config;
static pthread_t g_accept_thread;
static int g_listen_v4 = -1;
static int g_listen_v6 = -1;
static volatile int g_telnet_running = 0;

static mailboxd_result_t telnet_stop(void);

static int set_socket_options(int fd, int family)
{
    int on = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        return -1;
    }

    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) {
        return -1;
    }

    /* Keepalive: detect dead peers (e.g. cable pull, NAT timeout) after
     * ~2 minutes of silence.  Essential for long-running chat sessions
     * and helps all platforms (Linux, FreeBSD, macOS, Windows). */
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));

    mailboxd_socket_nosigpipe(fd);

#ifdef IPV6_V6ONLY
    if (family == AF_INET6) {
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) != 0) {
            return -1;
        }
    }
#else
    (void)family;
#endif

    return 0;
}

static int create_listen_socket(int family, const char *bind_addr, unsigned port)
{
    int fd;
    int rc;

    fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    if (set_socket_options(fd, family) != 0) {
        close(fd);
        return -1;
    }

    if (family == AF_INET6) {
        struct sockaddr_in6 addr6;

        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons((uint16_t)port);

        if (inet_pton(AF_INET6, bind_addr, &addr6.sin6_addr) != 1) {
            close(fd);
            return -1;
        }

        rc = bind(fd, (struct sockaddr *)&addr6, sizeof(addr6));
    } else {
        struct sockaddr_in addr4;

        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons((uint16_t)port);

        if (inet_pton(AF_INET, bind_addr, &addr4.sin_addr) != 1) {
            close(fd);
            return -1;
        }

        rc = bind(fd, (struct sockaddr *)&addr4, sizeof(addr4));
    }

    if (rc != 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static mailboxd_result_t telnet_send_byte(int fd, char ch)
{
    ssize_t sent;
    const char *p = &ch;

    sent = send(fd, p, 1, MSG_NOSIGNAL);
    if (sent < 0) {
        return MAILBOXD_ERR_IO;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t telnet_write(mailboxd_session_t *session,
                                   const char *data, size_t len)
{
    telnet_client_t *client;
    size_t i;
    mailboxd_result_t rc;

    if (session == NULL || data == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    client = (telnet_client_t *)session->transport_data;
    if (client == NULL || client->fd < 0) {
        return MAILBOXD_ERR_INVALID;
    }

    for (i = 0; i < len; i++) {
        char ch = data[i];

        if (ch == '\n' && (i == 0 || data[i - 1] != '\r')) {
            rc = telnet_send_byte(client->fd, '\r');
            if (rc != MAILBOXD_OK) {
                return rc;
            }
        }

        {
            ssize_t sent = send(client->fd, &ch, 1, MSG_NOSIGNAL);

            if (sent < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return MAILBOXD_ERR_IO;
            }
            if (sent == 0) {
                return MAILBOXD_ERR_IO;
            }
        }
    }

    return MAILBOXD_OK;
}

static void telnet_on_user_data(void *ctx, const uint8_t *data, size_t len)
{
    telnet_client_ctx_t *tctx = (telnet_client_ctx_t *)ctx;
    mailboxd_result_t rc;

    if (tctx == NULL || tctx->session == NULL || data == NULL || len == 0) {
        return;
    }

    rc = mailboxd_session_handle_input(tctx->session, data, len);
    if (rc != MAILBOXD_OK) {
        tctx->last_rc = rc;
    }
}

#define TELNET_CLIENT_POLL_MS 30000

static void telnet_send_busy(int fd)
{
    static const char msg[] = "All nodes in use. Try later.\r\n";

    if (fd < 0) {
        return;
    }

    (void)send(fd, msg, sizeof(msg) - 1u, MSG_NOSIGNAL);
}

static void *telnet_client_thread(void *arg)
{
    telnet_client_t *client = (telnet_client_t *)arg;
    mailboxd_session_t *session = NULL;
    telnet_client_ctx_t tctx;
    uint8_t buf[256];
    ssize_t n;
    mailboxd_result_t rc;

    if (client == NULL || g_service == NULL) {
        free(client);
        return NULL;
    }

    (void)set_socket_options(client->fd, 0);

    rc = mailboxd_telnet_send_greeting(client->fd);
    if (rc != MAILBOXD_OK) {
        close(client->fd);
        free(client);
        return NULL;
    }

    rc = mailboxd_session_open(g_service, &mailboxd_plugin_telnet, client, &session);
    if (rc != MAILBOXD_OK || session == NULL) {
        if (rc == MAILBOXD_ERR_BUSY) {
            telnet_send_busy(client->fd);
        }
        close(client->fd);
        free(client);
        return NULL;
    }

    {
        char remote[64];

        if (mailboxd_socket_peer_name(client->fd, remote, sizeof(remote)) == MAILBOXD_OK) {
            (void)mailboxd_session_set_remote(session, remote);
        }
    }

    mailboxd_telnet_parser_init(&client->parser);
    tctx.session = session;
    tctx.last_rc = MAILBOXD_OK;

    while (g_telnet_running) {
        struct pollfd pfd;
        int pr;

        pfd.fd = client->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        pr = poll(&pfd, 1, TELNET_CLIENT_POLL_MS);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pr == 0) {
            rc = mailboxd_session_tick(session);
            if (rc == MAILBOXD_SESSION_END) {
                tctx.last_rc = MAILBOXD_SESSION_END;
                break;
            }
            continue;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }

        n = recv(client->fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }

        rc = mailboxd_telnet_parser_feed(&client->parser, client->fd, buf, (size_t)n,
                                      telnet_on_user_data, &tctx);
        if (rc != MAILBOXD_OK) {
            break;
        }

        if (tctx.last_rc == MAILBOXD_SESSION_END) {
            break;
        }
    }

    {
        const char *uname = mailboxd_session_username(session);
        mailboxd_log_info("[telnet] session end: user=%s fd=%d last_rc=%d",
                       (uname && uname[0]) ? uname : "?",
                       client->fd, (int)tctx.last_rc);
    }

    mailboxd_session_close(session);

    /*
     * Close the socket cleanly.  The session_close above has already
     * sent "Goodbye.\r\n" through telnet_write (TCP_NODELAY ensures
     * immediate delivery).  A single shutdown(SHUT_WR) sends TCP FIN
     * so the client sees EOF; then close() releases the fd.
     *
     * We intentionally do NOT drain here — some telnet clients
     * (notably FreeBSD's and PuTTY) block in send() during the
     * option negotiation teardown, and a recv() drain loop would
     * hang the server thread until the client times out.
     */
    shutdown(client->fd, SHUT_WR);
    close(client->fd);
    free(client);
    return NULL;
}

static void accept_client(int client_fd)
{
    telnet_client_t *client;
    pthread_t thread;
    pthread_attr_t attr;

    client = calloc(1, sizeof(*client));
    if (client == NULL) {
        close(client_fd);
        return;
    }

    client->fd = client_fd;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&thread, &attr, telnet_client_thread, client) != 0) {
        close(client_fd);
        free(client);
    }

    pthread_attr_destroy(&attr);
}

static void *telnet_accept_thread(void *arg)
{
    (void)arg;

    while (g_telnet_running) {
        struct pollfd fds[2];
        nfds_t nfds = 0;
        int i;
        int ready;

        memset(fds, 0, sizeof(fds));

        if (g_listen_v4 >= 0) {
            fds[nfds].fd = g_listen_v4;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (g_listen_v6 >= 0) {
            fds[nfds].fd = g_listen_v6;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (nfds == 0) {
            break;
        }

        ready = poll(fds, nfds, 500);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (ready == 0) {
            continue;
        }

        for (i = 0; i < (int)nfds; i++) {
            if (fds[i].revents & POLLIN) {
                int client_fd = accept(fds[i].fd, NULL, NULL);

                if (client_fd >= 0) {
                    if (!mailboxd_security_ban_accept_fd(client_fd)) {
                        close(client_fd);
                        continue;
                    }
                    accept_client(client_fd);
                }
            }
        }
    }

    return NULL;
}

static mailboxd_result_t telnet_init(mailboxd_service_t *service)
{
    g_service = service;
    return MAILBOXD_OK;
}

static void telnet_shutdown(void)
{
    telnet_stop();
}

static mailboxd_result_t telnet_start(const char *config)
{
    mailboxd_result_t rc;

    if (g_telnet_running) {
        return MAILBOXD_ERR_BUSY;
    }

    rc = mailboxd_telnet_config_parse(config, &g_config);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    g_listen_v4 = -1;
    g_listen_v6 = -1;

    if (g_config.ipv4) {
        g_listen_v4 = create_listen_socket(AF_INET, g_config.bind_v4, g_config.port);
        if (g_listen_v4 < 0) {
            mailboxd_socket_log_bind_failure("telnet", g_config.bind_v4,
                                          g_config.port);
            return MAILBOXD_ERR_IO;
        }
    }

    if (g_config.ipv6) {
        g_listen_v6 = create_listen_socket(AF_INET6, g_config.bind_v6, g_config.port);
        if (g_listen_v6 < 0) {
            mailboxd_log_warn("[telnet] IPv6 bind [%s]:%u skipped (%s)",
                           g_config.bind_v6, g_config.port, strerror(errno));
        }
    }

    if (g_listen_v4 < 0 && g_listen_v6 < 0) {
        return MAILBOXD_ERR_IO;
    }

    g_telnet_running = 1;

    if (pthread_create(&g_accept_thread, NULL, telnet_accept_thread, NULL) != 0) {
        g_telnet_running = 0;
        if (g_listen_v4 >= 0) {
            close(g_listen_v4);
            g_listen_v4 = -1;
        }
        if (g_listen_v6 >= 0) {
            close(g_listen_v6);
            g_listen_v6 = -1;
        }
        return MAILBOXD_ERR_IO;
    }

    {
        char msg[256];
        size_t pos = 0;

        pos += (size_t)snprintf(msg + pos, sizeof(msg) - pos, "[telnet] listening");
        if (g_listen_v4 >= 0) {
            pos += (size_t)snprintf(msg + pos, sizeof(msg) - pos, " IPv4 %s:%u",
                                    g_config.bind_v4, g_config.port);
        }
        if (g_listen_v6 >= 0) {
            pos += (size_t)snprintf(msg + pos, sizeof(msg) - pos, " IPv6 [%s]:%u",
                                    g_config.bind_v6, g_config.port);
        }
        mailboxd_log_info("%s", msg);
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t telnet_stop(void)
{
    if (!g_telnet_running) {
        return MAILBOXD_OK;
    }

    g_telnet_running = 0;

    if (g_listen_v4 >= 0) {
        shutdown(g_listen_v4, SHUT_RDWR);
        close(g_listen_v4);
        g_listen_v4 = -1;
    }

    if (g_listen_v6 >= 0) {
        shutdown(g_listen_v6, SHUT_RDWR);
        close(g_listen_v6);
        g_listen_v6 = -1;
    }

    pthread_join(g_accept_thread, NULL);
    mailboxd_log_info("[telnet] stop");
    return MAILBOXD_OK;
}

const mailboxd_transport_plugin_t mailboxd_plugin_telnet = {
    .name = "telnet",
    .kind = MAILBOXD_TRANSPORT_TELNET,
    .version = 1,
    .init = telnet_init,
    .shutdown = telnet_shutdown,
    .start = telnet_start,
    .stop = telnet_stop,
    .write = telnet_write,
};
