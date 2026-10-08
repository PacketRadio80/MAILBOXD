/**
 * mailboxd-telnet — MailboxD-native telnet client (CLI only, no GUI).
 *
 * Full telnet negotiation support for testing MailboxD servers.
 * Optional 2400 baud pacing and 80-column display profile.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__linux__) || defined(__GLIBC__)
#define _DEFAULT_SOURCE 1
#endif

#include "client_line.h"
#include "client_display.h"
#include "mailboxd/limits.h"
#include "mailboxd/socket.h"
#include "mailboxd/traffic.h"
#include "mailboxd/util.h"
#include "telnet_proto.h"

#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__AMIGA__)
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#else
#include <netdb.h>
#include <poll.h>
#endif

#define MAILBOXD_TELNET_DEFAULT_PORT 2323u
#define MAILBOXD_TELNET_POLL_MS      50

typedef struct mailboxd_telnet_client_config {
    char host[256];
    unsigned port;
    char login_user[64];
    char login_password[128];
    unsigned baud;
    unsigned line_width;
    int pace_output;
    int ansi;
    int verbose;
    unsigned timeout_sec;
    int ipv6;
} mailboxd_telnet_client_config_t;

static void config_defaults(mailboxd_telnet_client_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    mailboxd_strlcpy(cfg->host, "127.0.0.1", sizeof(cfg->host));
    cfg->port = MAILBOXD_TELNET_DEFAULT_PORT;
    cfg->baud = MAILBOXD_BAUD2400;
    cfg->line_width = MAILBOXD_LINE_WIDTH;
    cfg->pace_output = 1;
    cfg->timeout_sec = 30;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "mailboxd-telnet — MailboxD telnet client for testing (CLI only)\n"
            "\n"
            "Usage: %s [options] [host [port]]\n"
            "\n"
            "Options:\n"
            "  -H, --host HOST       Remote host (default 127.0.0.1)\n"
            "  -p, --port PORT       Telnet port (default 2323)\n"
            "  -u, --user USER       Send /login USER after connect\n"
            "  -P, --password PASS   Password for /login\n"
            "      --baud N          Pace output like MailboxD (default 2400, 0=off)\n"
            "      --line-width N    Wrap display columns (default 80, 0=off)\n"
            "      --pace yes|no     Output pacing (default yes)\n"
            "      --ansi yes|no     Pass ANSI sequences (default no)\n"
            "  -6, --ipv6            Prefer IPv6 when resolving host\n"
            "  -t, --timeout SEC     Connect timeout (default 30)\n"
            "  -v, --verbose         Log telnet negotiation on stderr\n"
            "  -h, --help            Show help\n"
            "\n"
            "Environment: MAILBOXD_HOST, MAILBOXD_PORT\n"
            "\n",
            prog);
}

static int tcp_connect(const mailboxd_telnet_client_config_t *cfg, int *out_fd)
{
#if defined(__AMIGA__)
    struct sockaddr_in addr;
    struct hostent *he;
    int fd;
    int on = 1;

    if (cfg->ipv6) {
        fprintf(stderr, "mailboxd-telnet: IPv6 is not supported on AmigaOS\n");
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "mailboxd-telnet: socket failed for %s:%u\n",
                cfg->host, cfg->port);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg->port);

    if (inet_aton(cfg->host, &addr.sin_addr) == 0) {
        he = gethostbyname(cfg->host);
        if (he == NULL || he->h_addrtype != AF_INET || he->h_length == 0) {
            fprintf(stderr, "mailboxd-telnet: unknown host %s\n", cfg->host);
            close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr, (size_t)he->h_length);
    }

    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    mailboxd_socket_nosigpipe(fd);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "mailboxd-telnet: could not connect to %s:%u\n",
                cfg->host, cfg->port);
        close(fd);
        return -1;
    }

    *out_fd = fd;
    return 0;
#else
    char portbuf[16];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int fd = -1;
    int rc;

    snprintf(portbuf, sizeof(portbuf), "%u", cfg->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = cfg->ipv6 ? AF_INET6 : AF_UNSPEC;

    rc = getaddrinfo(cfg->host, portbuf, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "mailboxd-telnet: connect %s:%u failed: %s\n",
                cfg->host, cfg->port, gai_strerror(rc));
        return -1;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int on = 1;

        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        mailboxd_socket_nosigpipe(fd);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "mailboxd-telnet: could not connect to %s:%u\n",
                cfg->host, cfg->port);
        return -1;
    }

    *out_fd = fd;
    return 0;
#endif
}

typedef struct display_ctx {
    mailboxd_client_display_t display;
} display_ctx_t;

static void on_telnet_data(void *userdata, const uint8_t *data, size_t len)
{
    display_ctx_t *ctx = (display_ctx_t *)userdata;

    mailboxd_client_display_write(&ctx->display, data, len);
}

static mailboxd_result_t send_raw(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return MAILBOXD_ERR_IO;
        }
        if (n == 0) {
            return MAILBOXD_ERR_IO;
        }
        off += (size_t)n;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t send_line(int fd, const char *line)
{
    size_t len = strlen(line);
    uint8_t stack[256];
    uint8_t *buf = stack;
    size_t cap = sizeof(stack);
    size_t need = len;
    size_t i;
    size_t pos = 0;
    mailboxd_result_t rc;

    for (i = 0; i < len; i++) {
        if (line[i] == (char)255) {
            need++;
        }
    }
    need += 1;

    if (need > cap) {
        buf = malloc(need);
        if (buf == NULL) {
            return MAILBOXD_ERR_NOMEM;
        }
        cap = need;
    }

    for (i = 0; i < len; i++) {
        buf[pos++] = (uint8_t)line[i];
        if (line[i] == (char)255) {
            buf[pos++] = 255;
        }
    }
    buf[pos++] = (uint8_t)'\n';

    rc = send_raw(fd, buf, pos);
    if (buf != stack) {
        free(buf);
    }
    return rc;
}

static void maybe_auto_login(int fd, const mailboxd_telnet_client_config_t *cfg)
{
    char line[192];

    if (cfg->login_user[0] == '\0') {
        return;
    }

    snprintf(line, sizeof(line), "/login %s", cfg->login_user);
    (void)send_line(fd, line);
    if (cfg->login_password[0] != '\0') {
        (void)send_line(fd, cfg->login_password);
    }
}

static int run_session(int fd, const mailboxd_telnet_client_config_t *cfg)
{
    mailboxd_telnet_parser_t parser;
    display_ctx_t display;
    mailboxd_client_line_editor_t editor;
    mailboxd_traffic_config_t traffic;
    char inbuf[MAILBOXD_LINE_MAX + 2];
    int running = 1;
    int editor_ready = 0;

    mailboxd_traffic_config_defaults(&traffic);
    traffic.baud = cfg->baud;
    traffic.line_width = cfg->line_width;
    traffic.pace_output = cfg->pace_output;
    traffic.ansi = cfg->ansi;
    mailboxd_client_display_init(&display.display, &traffic);
    mailboxd_telnet_parser_init(&parser);
    if (mailboxd_client_line_editor_start(&editor, STDIN_FILENO) == 0) {
        editor_ready = 1;
    }

    (void)mailboxd_telnet_send_greeting(fd);
    maybe_auto_login(fd, cfg);

    while (running) {
#if defined(__AMIGA__)
        fd_set rfds;
        struct timeval tv;
        int maxfd;
        int pr;

        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(fd, &rfds);
        maxfd = fd;
        if (STDIN_FILENO > maxfd) {
            maxfd = STDIN_FILENO;
        }
        tv.tv_sec = MAILBOXD_TELNET_POLL_MS / 1000;
#if defined(__AMIGA__)
        tv.tv_usec = (unsigned long)((MAILBOXD_TELNET_POLL_MS % 1000) * 1000);
#else
        tv.tv_usec = (MAILBOXD_TELNET_POLL_MS % 1000) * 1000;
#endif

        pr = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            int have_line = 0;
            int eof_seen = 0;

            if (!editor_ready ||
                mailboxd_client_line_editor_poll(&editor, inbuf, sizeof(inbuf),
                                              &have_line, &eof_seen) != MAILBOXD_OK) {
                running = 0;
            } else if (eof_seen) {
                running = 0;
            } else if (have_line) {
                if (send_line(fd, inbuf) != MAILBOXD_OK) {
                    break;
                }
            }
        }

        if (FD_ISSET(fd, &rfds)) {
            uint8_t buf[512];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (n == 0) {
                break;
            }

            if (cfg->verbose) {
                fprintf(stderr, "mailboxd-telnet: recv %zd bytes\n", n);
            }

            if (mailboxd_telnet_parser_feed(&parser, fd, buf, (size_t)n,
                                         on_telnet_data, &display) != MAILBOXD_OK) {
                break;
            }
        }
#else
        struct pollfd pfds[2];
        int pr;

        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = fd;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        pr = poll(pfds, 2, MAILBOXD_TELNET_POLL_MS);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if ((pfds[0].revents & POLLIN) != 0) {
            int have_line = 0;
            int eof_seen = 0;

            if (!editor_ready ||
                mailboxd_client_line_editor_poll(&editor, inbuf, sizeof(inbuf),
                                              &have_line, &eof_seen) != MAILBOXD_OK) {
                running = 0;
            } else if (eof_seen) {
                running = 0;
            } else if (have_line) {
                if (send_line(fd, inbuf) != MAILBOXD_OK) {
                    break;
                }
            }
        }

        if ((pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }

        if ((pfds[1].revents & POLLIN) != 0) {
            uint8_t buf[512];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (n == 0) {
                break;
            }

            if (cfg->verbose) {
                fprintf(stderr, "mailboxd-telnet: recv %zd bytes\n", n);
            }

            if (mailboxd_telnet_parser_feed(&parser, fd, buf, (size_t)n,
                                         on_telnet_data, &display) != MAILBOXD_OK) {
                break;
            }
        }
#endif
    }

    if (editor_ready) {
        mailboxd_client_line_editor_stop(&editor);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    mailboxd_telnet_client_config_t cfg;
    int fd = -1;
    int opt;
    int argi = 1;
    const char *env;

    static const struct option long_opts[] = {
        {"host", required_argument, NULL, 'H'},
        {"port", required_argument, NULL, 'p'},
        {"user", required_argument, NULL, 'u'},
        {"password", required_argument, NULL, 'P'},
        {"baud", required_argument, NULL, 1000},
        {"line-width", required_argument, NULL, 1001},
        {"pace", required_argument, NULL, 1002},
        {"ansi", required_argument, NULL, 1003},
        {"ipv6", no_argument, NULL, '6'},
        {"timeout", required_argument, NULL, 't'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    config_defaults(&cfg);

    env = getenv("MAILBOXD_HOST");
    if (env != NULL && env[0] != '\0') {
        mailboxd_strlcpy(cfg.host, env, sizeof(cfg.host));
    }
    env = getenv("MAILBOXD_PORT");
    if (env != NULL && env[0] != '\0') {
        cfg.port = (unsigned)strtoul(env, NULL, 10);
    }

    while ((opt = getopt_long(argc, argv, "H:p:u:P:6t:vh",
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'H':
            mailboxd_strlcpy(cfg.host, optarg, sizeof(cfg.host));
            break;
        case 'p':
            cfg.port = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'u':
            mailboxd_strlcpy(cfg.login_user, optarg, sizeof(cfg.login_user));
            break;
        case 'P':
            mailboxd_strlcpy(cfg.login_password, optarg, sizeof(cfg.login_password));
            break;
        case '6':
            cfg.ipv6 = 1;
            break;
        case 't':
            cfg.timeout_sec = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'v':
            cfg.verbose = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        case 1000:
            cfg.baud = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 1001:
            cfg.line_width = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 1002:
            cfg.pace_output = mailboxd_parse_bool(optarg, cfg.pace_output);
            break;
        case 1003:
            cfg.ansi = mailboxd_parse_bool(optarg, cfg.ansi);
            break;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    argi = optind;
    if (argi < argc) {
        mailboxd_strlcpy(cfg.host, argv[argi++], sizeof(cfg.host));
    }
    if (argi < argc) {
        cfg.port = (unsigned)strtoul(argv[argi++], NULL, 10);
    }

    (void)cfg.timeout_sec;

    if (tcp_connect(&cfg, &fd) != 0) {
        return EXIT_FAILURE;
    }

    if (cfg.verbose) {
        fprintf(stderr, "mailboxd-telnet: connected to %s:%u (baud=%u width=%u)\n",
                cfg.host, cfg.port, cfg.baud, cfg.line_width);
    }

    run_session(fd, &cfg);
    close(fd);
    return EXIT_SUCCESS;
}
