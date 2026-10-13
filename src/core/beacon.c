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
 *    from [beacon] callerid in mailboxd.ini).  New frames are
 *    dispatched as commands through the mailboxd_prterm bridge.
 *
 * INI: [beacon]
 *   enabled    = yes
 *   callerid   = CALLERID
 *   interval   = 150          ; CQ beacon cycle (seconds)
 *   prterm_url = <URL of the prterm.cgi endpoint on this host>
 *                          ; REQUIRED — there is NO built-in default URL.
 *                          ; The route depends on the web server setup
 *                          ; (/prterm.cgi, ScriptAlias, ...), so the
 *                          ; operator must set it.  Beacon refuses to
 *                          ; start when unset or not http://.
 *   stations   = tnc2c,pktn2c,bc0
 *                          ; comma-separated PRTERM station names.
 *                          ; Each gets its own HTTP request with station=
 *                          ; and its own 150 s band-free check in PRTERM.
 *                          ; Empty = single request, PRTERM default station.
 *   tx_delay   = 10        ; seconds between station TXes (min 1)
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
#include <time.h>
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
#define BEACON_STATIONS_MAX  256
#define BEACON_INTERVAL_MIN  150
#define BEACON_TX_DELAY_DEF  10
#define RX_POLL_SEC           5

typedef struct beacon_config {
    int        enabled;
    char       callerid[BEACON_CALLERID_MAX];
    char       msg[BEACON_MSG_MAX];
    unsigned   interval;
    char       prterm_url[BEACON_URL_MAX];
    char       stations[BEACON_STATIONS_MAX]; /* comma-separated */
    unsigned   tx_delay;                       /* seconds between TXes */
} beacon_config_t;

static void beacon_config_defaults(beacon_config_t *bc)
{
    memset(bc, 0, sizeof *bc);
    bc->enabled   = 0;
    bc->interval  = BEACON_INTERVAL_MIN;
    bc->tx_delay  = BEACON_TX_DELAY_DEF;
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
        /* Default: "<callerid> = online" (from [beacon] callerid). */
        snprintf(bc->msg, sizeof bc->msg, "%s = online", bc->callerid);
    }

    bc->interval = mailboxd_config_get_uint(config, "beacon",
                                            "interval", BEACON_INTERVAL_MIN,
                                            BEACON_INTERVAL_MIN, 3600);

    v = mailboxd_config_get(config, "beacon", "stations", "");
    if (v[0] != '\0')
        mailboxd_strlcpy(bc->stations, v, sizeof bc->stations);

    v = mailboxd_config_get(config, "beacon", "prterm_url", "");
    if (v[0] != '\0')
        mailboxd_strlcpy(bc->prterm_url, v, sizeof bc->prterm_url);

    bc->tx_delay = mailboxd_config_get_uint(config, "beacon",
                                            "tx_delay", BEACON_TX_DELAY_DEF,
                                            1, 300);
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

/* POST `body` to `url`, read the full response into `resp`.
 * out: http_status  HTTP status code from the status line (0 if the
 *                   transaction failed before a response arrived)
 *      elapsed_ms   wall time of the whole request
 * Returns 0 when a response was received (any status), -1 on a
 * transport error (errno set). */
static int http_post(const char *url, const char *body,
                     char *resp, size_t resp_len,
                     int *http_status, long *elapsed_ms)
{
    char host[128];
    int  port;
    char path[256];
    struct timespec t0, t1;

    if (http_status != NULL) *http_status = 0;
    if (elapsed_ms != NULL) *elapsed_ms = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (parse_url(url, host, sizeof host, &port, path, sizeof path) != 0) {
        mailboxd_log_warn("[beacon] event=http result=error reason=bad_url url=%s", url);
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

    if (http_status != NULL) {
        int code = 0;
        if (strncmp(resp, "HTTP/", 5) == 0) {
            const char *sp = strchr(resp, ' ');
            if (sp != NULL) code = atoi(sp + 1);
        }
        *http_status = code;
    }
    if (elapsed_ms != NULL) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        *elapsed_ms = (long)((t1.tv_sec - t0.tv_sec) * 1000L +
                             (t1.tv_nsec - t0.tv_nsec) / 1000000L);
    }
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
    p += strlen(needle); /* value starts right after "key":" */
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

    mailboxd_log_info("[beacon] event=start callerid=%s msg='%s' interval=%us rxpoll=%us tx_delay=%us stations='%s' url=%s",
                      g_cfg.callerid, g_cfg.msg, g_cfg.interval, RX_POLL_SEC,
                      g_cfg.tx_delay,
                      g_cfg.stations[0] ? g_cfg.stations : "(default)",
                      g_cfg.prterm_url);

    /* PRTERM socket path: derived from the standard layout. */
    char sock_path[256];
    snprintf(sock_path, sizeof sock_path,
             "/var/mailboxd/prterm.sock");

    long long rx_last_ts = 0;          /* epoch of last processed RX frame */
    unsigned  beacon_tick = g_cfg.interval; /* fires immediately on first cycle */
    unsigned  rx_tick = 0;
    unsigned  cq_cycle = 0;            /* CQ cycle counter for log correlation */

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
            int  poll_status = 0;
            long poll_ms = 0;
            if (http_post(g_cfg.prterm_url, body, resp, sizeof resp,
                          &poll_status, &poll_ms) != 0) {
                mailboxd_log_debug("[beacon] event=rx_poll result=error reason=transport errno=%d ms=%ld",
                                   errno, poll_ms);
            } else if (json_is_ok(http_body(resp))) {
                const char *rbody = http_body(resp);
                int nframes = 0;
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
                        nframes++;
                        mailboxd_log_info("[beacon] event=rx_frame from=%s ts=%lld text='%.120s'",
                                          from, ts, text);

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
                                mailboxd_log_info("[beacon] event=rx_dispatch from=%s result=ok out='%.120s'",
                                                  from, run_out);
                            } else {
                                mailboxd_log_warn("[beacon] event=rx_dispatch from=%s result=error reason=bridge_hello_failed sock=%s",
                                                  from, sock_path);
                            }
                            close(bfd);
                        } else {
                            mailboxd_log_warn("[beacon] event=rx_dispatch from=%s result=error reason=bridge_connect_failed sock=%s errno=%d",
                                              from, sock_path, errno);
                        }
                    }
                    p += 8; /* advance past "from":" */
                }
                mailboxd_log_debug("[beacon] event=rx_poll result=ok http=%d ms=%ld frames=%d since=%lld",
                                   poll_status, poll_ms, nframes, rx_last_ts);
            } else {
                mailboxd_log_debug("[beacon] event=rx_poll result=skip http=%d ms=%ld response='%.80s'",
                                   poll_status, poll_ms, http_body(resp));
            }
        }

        /* ── CQ beacon (every `interval` seconds) ────────────────── */
        beacon_tick++;
        if (beacon_tick < g_cfg.interval) continue;
        beacon_tick = 0;
        cq_cycle++;

        char enc_cid[128];
        url_encode(g_cfg.callerid, enc_cid, sizeof enc_cid);

        char enc_msg[256];
        url_encode(g_cfg.msg, enc_msg, sizeof enc_msg);

        /* Build the base POST body (without station=). */
        char base_body[512];
        snprintf(base_body, sizeof base_body,
                 "action=mbox_cqbeacon&callerid=%s&msg=%s",
                 enc_cid, enc_msg);

        /* If stations are configured, iterate each one with its own
         * request.  Each station gets a `station=` param so PRTERM
         * binds that rig and checks its band-free independently. */
        if (g_cfg.stations[0] != '\0') {
            char buf[BEACON_STATIONS_MAX];
            mailboxd_strlcpy(buf, g_cfg.stations, sizeof buf);
            char *save = NULL;
            char *tok = strtok_r(buf, ",", &save);
            mailboxd_log_debug("[beacon] event=cq_cycle cycle=%u stations='%s'",
                               cq_cycle, g_cfg.stations);
            while (tok && g_running) {
                /* Trim leading/trailing whitespace. */
                while (*tok == ' ' || *tok == '\t') tok++;
                char *end = tok + strlen(tok) - 1;
                while (end > tok && (*end == ' ' || *end == '\t')) *end-- = '\0';
                if (tok[0] == '\0') { tok = strtok_r(NULL, ",", &save); continue; }

                char enc_sta[64];
                url_encode(tok, enc_sta, sizeof enc_sta);
                char body[640];
                snprintf(body, sizeof body, "%s&station=%s",
                         base_body, enc_sta);

                char resp[1024];
                int  tx_status = 0;
                long tx_ms = 0;
                if (http_post(g_cfg.prterm_url, body, resp, sizeof resp,
                              &tx_status, &tx_ms) != 0) {
                    mailboxd_log_warn("[beacon] event=cq_tx cycle=%u station=%s result=error reason=transport errno=%d ms=%ld",
                                      cq_cycle, tok, errno, tx_ms);
                } else {
                    const char *rbody = http_body(resp);
                    if (json_is_ok(rbody)) {
                        mailboxd_log_info("[beacon] event=cq_tx cycle=%u station=%s result=ok http=%d ms=%ld",
                                          cq_cycle, tok, tx_status, tx_ms);
                    } else {
                        char why[160] = "";
                        json_str(rbody, "error", why, sizeof why);
                        mailboxd_log_warn("[beacon] event=cq_tx cycle=%u station=%s result=skip http=%d ms=%ld reason='%s' response='%.100s'",
                                          cq_cycle, tok, tx_status, tx_ms,
                                          why[0] ? why : "unknown", rbody);
                    }
                }

                /* Delay before next station (skip sleep on last).
                 * sleep() returns the UNSLEPT seconds, so the remaining
                 * time is the return value itself (not rem - retval). */
                tok = strtok_r(NULL, ",", &save);
                if (tok && g_running && g_cfg.tx_delay > 0) {
                    unsigned rem = g_cfg.tx_delay;
                    while (rem > 0 && g_running) rem = sleep(rem);
                }
            }
        } else {
            /* No station list — single request, PRTERM default station. */
            char resp[1024];
            int  tx_status = 0;
            long tx_ms = 0;
            mailboxd_log_debug("[beacon] event=cq_cycle cycle=%u stations='(default)'",
                               cq_cycle);
            if (http_post(g_cfg.prterm_url, base_body, resp, sizeof resp,
                          &tx_status, &tx_ms) != 0) {
                mailboxd_log_warn("[beacon] event=cq_tx cycle=%u station=default result=error reason=transport errno=%d ms=%ld",
                                  cq_cycle, errno, tx_ms);
            } else {
                const char *rbody = http_body(resp);
                if (json_is_ok(rbody)) {
                    mailboxd_log_info("[beacon] event=cq_tx cycle=%u station=default result=ok http=%d ms=%ld",
                                      cq_cycle, tx_status, tx_ms);
                } else {
                    char why[160] = "";
                    json_str(rbody, "error", why, sizeof why);
                    mailboxd_log_warn("[beacon] event=cq_tx cycle=%u station=default result=skip http=%d ms=%ld reason='%s' response='%.100s'",
                                      cq_cycle, tx_status, tx_ms,
                                      why[0] ? why : "unknown", rbody);
                }
            }
        }
    }

    mailboxd_log_info("[beacon] event=stop");
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
    if (g_cfg.prterm_url[0] == '\0') {
        mailboxd_log_warn("[beacon] event=config_error reason=prterm_url_missing "
                          "detail='[beacon] prterm_url must point to the "
                          "prterm.cgi endpoint of this host — no default is built in'");
        return MAILBOXD_ERR_INVALID;
    }
    {
        char host[128], path[256];
        int  port = 0;
        if (parse_url(g_cfg.prterm_url, host, sizeof host, &port,
                      path, sizeof path) != 0) {
            mailboxd_log_warn("[beacon] event=config_error reason=prterm_url_invalid url=%s",
                              g_cfg.prterm_url);
            return MAILBOXD_ERR_INVALID;
        }
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
