/*
 * test_mailboxd_prterm.c — end-to-end test of the mailboxd_prterm
 * unix-domain-socket plugin (the PRTERM<->MailboxD link).
 *
 * Boots a real mailboxd_service_t against a throwaway temp INI/data
 * tree with only the mailboxd_prterm transport enabled, then drives
 * the wire protocol as PRTERM's src/mailboxdsock.c would: HELLO,
 * PING, RUN <mailboxd-command>, and the documented error replies.
 *
 * Only built when MAILBOXD_PLUGIN_MAILBOXD_PRTERM=ON (see
 * tests/CMakeLists.txt) — the plugin itself is off by default.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mailboxd/service.h"
#include "mailboxd/registry.h"
#include "mailboxd/config.h"
#include "mailboxd/commands_registry.h"
#include "mailboxd/plugin.h"
#include "mailboxd/util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

extern const mailboxd_transport_plugin_t mailboxd_plugin_mailboxd_prterm;

static unsigned g_failures;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_failures++;
}

static void expect_str(const char *label, const char *got, const char *want)
{
    if (got == NULL || want == NULL || strcmp(got, want) != 0) {
        fail("%s: got '%s' want '%s'", label,
             got != NULL ? got : "(null)", want != NULL ? want : "(null)");
    }
}

static void expect_true(const char *label, int cond)
{
    if (!cond) {
        fail("%s", label);
    }
}

/* ---------------------------------------------------------------------- */
/* Filesystem helpers                                                    */
/* ---------------------------------------------------------------------- */

static void rm_rf(const char *path)
{
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static void must_mkdir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fail("mkdir %s: %s", path, strerror(errno));
    }
}

static void write_ini(const char *path, const char *tmp_root)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fail("fopen %s for write: %s", path, strerror(errno));
        return;
    }
    fprintf(f,
        "[service]\n"
        "name = mailboxd-prterm-test\n"
        "\n"
        "[storage]\n"
        "backend = flatfile\n"
        "path = %s/data\n"
        "\n"
        "[auth]\n"
        "auto_login = yes\n"
        "\n"
        "[mail]\n"
        "enabled = yes\n"
        "path = %s/mail\n"
        "\n"
        "[log]\n"
        "enabled = no\n"
        "\n"
        "[monitor]\n"
        "enabled = no\n"
        "\n"
        "[security]\n"
        "enabled = no\n"
        "\n"
        "[networks]\n"
        "telnet = no\n"
        "\n"
        "[entertain]\n"
        "enabled = no\n"
        "\n"
        "[broadcast]\n"
        "enabled = no\n"
        "\n"
        "[transport.mailboxd_prterm]\n"
        "enabled = yes\n"
        "bind = %s/prterm.sock\n"
        "mode = 0600\n"
        "link_token = testtoken\n",
        tmp_root, tmp_root, tmp_root);
    fclose(f);
}

/* ---------------------------------------------------------------------- */
/* Wire protocol helpers (mirrors PRTERM's src/mailboxdsock.c)           */
/* ---------------------------------------------------------------------- */

static int connect_sock(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    mailboxd_strlcpy(addr.sun_path, path, sizeof addr.sun_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }

    struct timeval to = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
    return fd;
}

static int send_line(int fd, const char *line)
{
    size_t len = strlen(line);
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, line + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Reads one line (CRLF or LF terminated); returns length or -1 on
 * error/timeout/EOF. */
static ssize_t recv_line(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (c == '\n') {
            buf[n] = '\0';
            return (ssize_t)n;
        }
        if (c != '\r') {
            buf[n++] = c;
        }
    }
    buf[n] = '\0';
    return (ssize_t)n;
}

/* Drains OUT lines until an END line; returns 1 for "END ok", 0 for
 * "END err ...", -1 on I/O error. *out_count (if non-NULL) receives
 * the number of OUT lines seen. end_line (if non-NULL) receives the
 * raw END line. */
static int recv_until_end(int fd, int *out_count, char *end_line, size_t end_cap)
{
    int seen = 0;
    for (;;) {
        char line[2200];
        ssize_t n = recv_line(fd, line, sizeof line);
        if (n < 0) {
            return -1;
        }
        if (strncmp(line, "END", 3) == 0 &&
            (line[3] == ' ' || line[3] == '\0')) {
            if (end_line != NULL && end_cap > 0) {
                mailboxd_strlcpy(end_line, line, end_cap);
            }
            if (out_count != NULL) {
                *out_count = seen;
            }
            return strncmp(line, "END ok", 6) == 0 ? 1 : 0;
        }
        if (strncmp(line, "OUT ", 4) == 0) {
            seen++;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Test body                                                             */
/* ---------------------------------------------------------------------- */

int main(void)
{
    char tmp_root[256];
    char ini_path[320];
    char sock_path[320];
    char data_dir[320];
    char mail_dir[320];

    snprintf(tmp_root, sizeof tmp_root, "/tmp/mailboxd-prterm-test-%d",
              (int)getpid());
    snprintf(ini_path, sizeof ini_path, "%s/mailboxd.ini", tmp_root);
    snprintf(sock_path, sizeof sock_path, "%s/prterm.sock", tmp_root);
    snprintf(data_dir, sizeof data_dir, "%s/data", tmp_root);
    snprintf(mail_dir, sizeof mail_dir, "%s/mail", tmp_root);

    rm_rf(tmp_root);
    must_mkdir(tmp_root);
    must_mkdir(data_dir);
    must_mkdir(mail_dir);
    write_ini(ini_path, tmp_root);

    /* Point path resolution at the in-tree share/ so commands.yaml and
     * areas.yaml load the same way the installed layout does (both
     * files land flat in the install root - see src/CMakeLists.txt). */
    mailboxd_install_root_set(MAILBOXD_TEST_SHARE_DIR);

    mailboxd_registry_register(&mailboxd_plugin_mailboxd_prterm);

    mailboxd_service_t *service = mailboxd_service_create(NULL);
    if (service == NULL) {
        fail("mailboxd_service_create returned NULL");
        return EXIT_FAILURE;
    }

    mailboxd_config_t config;
    if (mailboxd_config_load(&config, ini_path) != MAILBOXD_OK) {
        fail("mailboxd_config_load(%s) failed", ini_path);
        mailboxd_service_destroy(service);
        return EXIT_FAILURE;
    }

    if (mailboxd_commands_registry_init() != MAILBOXD_OK) {
        fail("mailboxd_commands_registry_init failed (share dir: %s)",
             MAILBOXD_TEST_SHARE_DIR);
        mailboxd_config_free(&config);
        mailboxd_service_destroy(service);
        return EXIT_FAILURE;
    }

    if (mailboxd_service_apply_config(service, &config, ini_path) != MAILBOXD_OK) {
        fail("mailboxd_service_apply_config failed");
        mailboxd_commands_registry_shutdown();
        mailboxd_config_free(&config);
        mailboxd_service_destroy(service);
        return EXIT_FAILURE;
    }

    expect_true("socket file exists after start", access(sock_path, F_OK) == 0);

    /* --- Session 1: HELLO, PING, RUN /version, RUN /who, error paths --- */
    int fd = connect_sock(sock_path);
    expect_true("connect to prterm.sock", fd >= 0);

    if (fd >= 0) {
        char line[2200];

        expect_true("send HELLO", send_line(fd, "HELLO 1\n") == 0);
        expect_true("recv HELLO reply", recv_line(fd, line, sizeof line) >= 0);
        expect_str("HELLO reply", line, "OK MAILBOXD 1");

        expect_true("send PING", send_line(fd, "PING\n") == 0);
        expect_true("recv PONG", recv_line(fd, line, sizeof line) >= 0);
        expect_str("PING reply", line, "PONG");

        int out_count = 0;
        char end_line[64];

        expect_true("send RUN /version", send_line(fd, "RUN /version\n") == 0);
        int rc = recv_until_end(fd, &out_count, end_line, sizeof end_line);
        expect_true("RUN /version succeeds", rc == 1);
        expect_true("RUN /version produced output", out_count > 0);

        expect_true("send RUN /who", send_line(fd, "RUN /who\n") == 0);
        rc = recv_until_end(fd, &out_count, end_line, sizeof end_line);
        expect_true("RUN /who succeeds", rc == 1);

        /* Unknown top-level request. */
        expect_true("send BOGUS", send_line(fd, "BOGUS\n") == 0);
        expect_true("recv BOGUS reply", recv_line(fd, line, sizeof line) >= 0);
        expect_str("unknown request reply", line, "END err unknown-request");

        /* RUN with no command line. */
        expect_true("send RUN (empty)", send_line(fd, "RUN\n") == 0);
        expect_true("recv empty-command reply",
                    recv_line(fd, line, sizeof line) >= 0);
        expect_str("empty RUN reply", line, "END err empty-command");

        /* RUN with a non-mailboxd (non-slash) line. */
        expect_true("send RUN hello", send_line(fd, "RUN hello\n") == 0);
        rc = recv_until_end(fd, &out_count, end_line, sizeof end_line);
        expect_true("RUN hello rejected", rc == 0);
        expect_str("RUN hello END line", end_line, "END err not-a-mailboxd-command");

        close(fd);
    }

    /* --- Session 2: the accept loop must service a second client after
     * the first disconnects (single-client-at-a-time design). --- */
    usleep(50000);
    int fd2 = connect_sock(sock_path);
    expect_true("second connect to prterm.sock", fd2 >= 0);
    if (fd2 >= 0) {
        char line[64];
        expect_true("send PING (session 2)", send_line(fd2, "PING\n") == 0);
        expect_true("recv PONG (session 2)", recv_line(fd2, line, sizeof line) >= 0);
        expect_str("PING reply (session 2)", line, "PONG");
        close(fd2);
    }

    mailboxd_commands_registry_shutdown();
    mailboxd_config_free(&config);
    mailboxd_service_destroy(service);

    expect_true("socket file removed after shutdown", access(sock_path, F_OK) != 0);

    rm_rf(tmp_root);

    if (g_failures != 0) {
        fprintf(stderr, "%u failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    puts("test_mailboxd_prterm: ok");
    return EXIT_SUCCESS;
}
