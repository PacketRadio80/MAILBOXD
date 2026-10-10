/*
 * beacon.c — MailboxD autonomous CQ beacon + RX listener daemon.
 *
 * Two tasks in one thread:
 *
 * 1. CQ BEACON — every `interval` seconds, sends a CQ AX.25 broadcast
 *    via PRTERM's mbox_cqbeacon HTTP action.  Band-free (≥150 s) is
 *    enforced by PRTERM's TNC state.  Devices rotate automatically.
 *
 * 2. RX LISTENER — every 5 seconds, polls PRTERM for received AX.25
 *    frames addressed to this MailboxD instance's callerid (e.g.
 *    MGHBX1).  New frames are dispatched as commands through the
 *    mailboxd_prterm bridge.
 *
 * INI: [beacon]
 *   enabled    = yes
 *   callerid   = MGHBX1
 *   interval   = 150          ; CQ beacon cycle (seconds)
 *   prterm_url = http://127.0.0.1/prterm.cgi
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include "mailboxd/beacon.h"
#include "mailboxd/config.h"
#include "mailboxd/log.h"
#include "mailboxd/socket.h"
#include "mailboxd/util.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ---------------------------------------------------------------------- */
/* Configuration                                                          */
/* ---------------------------------------------------------------------- */

#define BEACON_CALLERID_MAX  32
#define BEACON_URL_MAX       256
#define BEACON_MSG_MAX       128
#define BEACON_INTERVAL_MIN  150
#define RX_POLL_SEC           5

typedef struct beacon_config {
    int        enabled;
    char       callerid[BEACON_CALLERID_MAX];
    char       msg[BEACON_MSG_MAX];
    unsigned   interval;
    char       prterm_url[BEACON_URL_MAX];
} beacon_config_t;

static void beacon_config_defaults(beacon_config_t *bc)
{
    memset(bc, 0, sizeof *bc);
    bc->enabled  = 0;
    bc->interval = BEACON_INTERVAL_MIN;
    mailboxd_strlcpy(bc->prterm_url,
                     "http://127.0.0.1/prterm.cgi",
                     sizeof bc->prterm_url);
}

static void beacon_config_load(beacon_config_t *bc,
                               const mailboxd_config_t *config)
{
    beacon_config_defaults(bc);
    if (config == NULL) return;

    bc->enabled = mailboxd_config_get_bool(config, "beacon", "enabled", 0);

    const char *v;
    v = mailboxd_config_get(config, "beacon", "callerid", "");
    if (v[0] != '\0')
        mailboxd_strlcpy(bc->callerid, v, sizeof bc->callerid);

    v = mailboxd_config_get(config, "beacon", "msg", "");
    if (v[0] != '\0')
        mailboxd_strlcpy(bc->msg, v, sizeof bc->msg);
    else if (bc->callerid[0] != '\0') {
        /* Default: "MGHBX1 = online" (callerid = online). */
        snprintf(bc->msg, sizeof bc->msg, "%s = online", bc->callerid);
    }

    bc->interval = mailboxd_config_get_uint(config, "beacon",
                                            "interval", BEACON_INTERVAL_MIN,
                                            BEACON_INTERVAL_MIN, 3600);
}

/* ---------------------------------------------------------------------- */
/* Minimal HTTP client                                                    */
/* ---------------------------------------------------------------------- */

static int parse_url(const char *url, char *host, size_t hlen,
                     int *port, char *path, size_t plen)
{
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else return -1;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        size_t hl = (size_t)(colon - p);
        if (hl >= hlen) hl = hlen - 1;
        memcpy(host, p, hl);
        host[hl] = '\0';
        *port = atoi(colon + 1);
    } else {
        size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        if (hl >= hlen) hl = hlen - 1;
        memcpy(host, p, hl);
        host[hl] = '\0';
        *port = 80;
    }
    mailboxd_strlcpy(path, slash ? slash : "/", plen);
    return 0;
}

static size_t url_encode(const char *src, char *dst, size_t dstlen)
{
    static const char hex[] = "0123456789abcdef";
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (w + 3 >= dstlen) break;
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.') {
            dst[w++] = (char)*p;
        } else if (*p == ' ') {
            dst[w++] = '+';
        } else {
            dst[w++] = '%';
            dst[w++] = hex[*p >> 4];
            dst[w++] = hex[*p & 0x0f];
        }
    }
    dst[w] = '\0';
    return w;
}

static int http_post(const char *url, const char *body,
                     char *resp, size_t resp_len)
{
    char host[128];
    int  port;
    char path[256];

    if (parse_url(url, host, sizeof host, &port, path, sizeof path) != 0) {
        mailboxd_log_warn("[beacon] bad URL: %s", url);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (he == NULL || he->h_addr_list[0] == NULL) {
            close(fd);
            return -1;
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
    }

    /* 3-second connect + send/recv timeout — we cannot block the
     * daemon thread for the full 75 s socket defaults. */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }

    char req[2048];
    int n = snprintf(req, sizeof req,
                     "POST %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Content-Type: application/x-www-form-urlencoded\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n%s",
                     path, host, strlen(body), body);
    if (n < 0 || (size_t)n >= sizeof req) {
        close(fd);
        return -1;
    }

    size_t sent = 0;
    size_t rlen = (size_t)n;
    while (sent < rlen) {
        ssize_t w = send(fd, req + sent, rlen - sent, MSG_NOSIGNAL);
        if (w < 0) { close(fd); return -1; }
        sent += (size_t)w;
    }

    resp[0] = '\0';
    size_t off = 0;
    for (;;) {
        if (off + 1 >= resp_len) break;
        ssize_t r = recv(fd, resp + off, resp_len - off - 1, 0);
        if (r <= 0) break;
        off += (size_t)r;
    }
    resp[off] = '\0';
    close(fd);
    return 0;
}

static const char *http_body(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : resp;
}

static int json_is_ok(const char *body)
{
    return strstr(body, "\"ok\":true") != NULL ||
           strstr(body, "\"ok\": true") != NULL;
}

/* ---------------------------------------------------------------------- */
/* Bridge client (HELLO + RUN via unix socket)                            */
/* ---------------------------------------------------------------------- */

static int bridge_open(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    mailboxd_strlcpy(addr.sun_path, sock_path, sizeof addr.sun_path);

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int bridge_send_line(int fd, const char *line)
{
    char buf[2100];
    int n = snprintf(buf, sizeof buf, "%s\n", line);
    if (n < 0 || (size_t)n >= sizeof buf) return -1;

    size_t off = 0;
    size_t len = (size_t)n;
    while (off < len) {
        ssize_t w = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (w < 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static int bridge_recv_line(int fd, char *out, size_t outlen)
{
    size_t off = 0;
    while (off + 1 < outlen) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') { out[off] = '\0'; return 0; }
        if (c != '\r') out[off++] = c;
    }
    out[off] = '\0';
    return -1;
}

static int bridge_hello(int fd)
{
    if (bridge_send_line(fd, "HELLO 1") != 0) return -1;
    char line[256];
    if (bridge_recv_line(fd, line, sizeof line) != 0) return -1;
    return (strncmp(line, "OK MAILBOXD", 11) == 0) ? 0 : -1;
}

static int bridge_run(int fd, const char *cmdline,
                      char *out, size_t outlen)
{
    char cmd[2100];
    snprintf(cmd, sizeof cmd, "RUN %s", cmdline);
    if (bridge_send_line(fd, cmd) != 0) return -1;

    out[0] = '\0';
    size_t used = 0;
    for (;;) {
        char line[2100];
        if (bridge_recv_line(fd, line, sizeof line) != 0) return -1;
        if (strncmp(line, "END ok", 6) == 0) return 0;
        if (strncmp(line, "END err", 7) == 0) return -1;
        if (strncmp(line, "OUT ", 4) == 0) {
            const char *body = line + 4;
            size_t blen = strlen(body);
            if (used + blen + 1 < outlen) {
                memcpy(out + used, body, blen);
                used += blen;
                out[used++] = '\n';
                out[used] = '\0';
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* JSON helpers (minimal, no dependency on html.h / pr_buf)               */
/* ---------------------------------------------------------------------- */

/* Find "key":"value" in a JSON string, return pointer into `json` or
 * NULL.  Only handles simple flat JSON — enough for mbox_rxpoll. */
static const char *json_str(const char *json, const char *key,
                            char *dst, size_t dstlen)
{
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) {
        snprintf(needle, sizeof needle, "\"%s\": \"", key);
        p = strstr(json, needle);
        if (!p) return NULL;
    }
    p = strchr(p + strlen(needle), '"'); /* skip to value start */
    if (!p) return NULL;
    p++;
    size_t w = 0;
    while (*p && *p != '"' && w + 1 < dstlen) {
        if (*p == '\\' && p[1]) p++;
        dst[w++] = *p++;
    }
    dst[w] = '\0';
    return dst;
}

static long long json_int(const char *json, const char *key)
{
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return atoll(p);
}

/* ---------------------------------------------------------------------- */
/* Daemon thread                                                          */
/* ---------------------------------------------------------------------- */

static pthread_t     g_thread;
static volatile sig_atomic_t g_running = 0;
static beacon_config_t g_cfg;

static void *beacon_thread(void *arg)
{
    (void)arg;

    mailboxd_log_info("[beacon] daemon started: callerid=%s msg='%s' interval=%us rxpoll=%us url=%s",
                      g_cfg.callerid, g_cfg.msg, g_cfg.interval, RX_POLL_SEC, g_cfg.prterm_url);

    /* PRTERM socket path: derived from the standard layout. */
    char sock_path[256];
    snprintf(sock_path, sizeof sock_path,
             "/var/mailboxd/prterm.sock");

    long long rx_last_ts = 0;          /* epoch of last processed RX frame */
    unsigned  beacon_tick = g_cfg.interval; /* fires immediately on first cycle */
    unsigned  rx_tick = 0;

    while (g_running) {
        sleep(1);
        if (!g_running) break;

        /* ── RX poll (every RX_POLL_SEC seconds) ─────────────────── */
        rx_tick++;
        if (rx_tick >= RX_POLL_SEC) {
            rx_tick = 0;

            char enc_cid[128];
            url_encode(g_cfg.callerid, enc_cid, sizeof enc_cid);

            char body[512];
            snprintf(body, sizeof body,
                     "action=mbox_rxpoll&callerid=%s&since=%lld",
                     enc_cid, rx_last_ts);

            char resp[4096];
            if (http_post(g_cfg.prterm_url, body, resp, sizeof resp) == 0) {
                const char *rbody = http_body(resp);
                if (json_is_ok(rbody)) {
                    /* Walk the "frames" array manually — find each
                     * {"from":"…","text":"…","ts":…} object. */
                    const char *p = rbody;
                    while ((p = strstr(p, "\"from\":\"")) != NULL) {
                        char from[32] = "";
                        char text[512] = "";
                        json_str(p, "from", from, sizeof from);
                        json_str(p, "text", text, sizeof text);
                        long long ts = json_int(p, "ts");
                        if (ts > rx_last_ts) rx_last_ts = ts;

                        if (from[0] && text[0]) {
                            mailboxd_log_info("[beacon] RX from=%s: %.120s",
                                              from, text);

                            /* Dispatch through the mailboxd_prterm bridge. */
                            int bfd = bridge_open(sock_path);
                            if (bfd >= 0) {
                                if (bridge_hello(bfd) == 0) {
                                    char run_out[2048];
                                    /* If the text looks like a command,
                                     * run it directly; otherwise treat
                                     * as a chat message. */
                                    if (text[0] == '/') {
                                        bridge_run(bfd, text, run_out, sizeof run_out);
                                    } else {
                                        char wrapped[600];
                                        snprintf(wrapped, sizeof wrapped,
                                                 "/msg %s %s", from, text);
                                        bridge_run(bfd, wrapped, run_out, sizeof run_out);
                                    }
                                }
                                close(bfd);
                            }
                        }
                        p += 8; /* advance past "from":" */
                    }
                }
            }
        }

        /* ── CQ beacon (every `interval` seconds) ────────────────── */
        beacon_tick++;
        if (beacon_tick < g_cfg.interval) continue;
        beacon_tick = 0;

        char enc_cid[128];
        url_encode(g_cfg.callerid, enc_cid, sizeof enc_cid);

        char enc_msg[256];
        url_encode(g_cfg.msg, enc_msg, sizeof enc_msg);

        char body[512];
        snprintf(body, sizeof body, "action=mbox_cqbeacon&callerid=%s&msg=%s",
                 enc_cid, enc_msg);

        char resp[1024];
        if (http_post(g_cfg.prterm_url, body, resp, sizeof resp) != 0) {
            mailboxd_log_warn("[beacon] CQ POST failed");
            continue;
        }
        const char *rbody = http_body(resp);
        if (json_is_ok(rbody)) {
            mailboxd_log_info("[beacon] CQ transmitted");
        } else {
            mailboxd_log_debug("[beacon] CQ skipped: %.120s", rbody);
        }
    }

    mailboxd_log_info("[beacon] daemon stopped");
    return NULL;
}

mailboxd_result_t mailboxd_beacon_start(const mailboxd_config_t *config)
{
    beacon_config_load(&g_cfg, config);

    if (!g_cfg.enabled) {
        mailboxd_log_info("[beacon] disabled in INI");
        return MAILBOXD_OK;
    }
    if (g_cfg.callerid[0] == '\0') {
        mailboxd_log_warn("[beacon] callerid empty — not starting");
        return MAILBOXD_ERR_INVALID;
    }

    g_running = 1;
    if (pthread_create(&g_thread, NULL, beacon_thread, NULL) != 0) {
        mailboxd_log_warn("[beacon] pthread_create: %s", strerror(errno));
        g_running = 0;
        return MAILBOXD_ERR_IO;
    }
    return MAILBOXD_OK;
}

void mailboxd_beacon_stop(void)
{
    if (!g_running) return;
    g_running = 0;
    pthread_join(g_thread, NULL);
}
