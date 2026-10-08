/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/traffic.h"
#if !defined(MAILBOXD_CLIENT_BUILD)
#include "mailboxd/config.h"
#endif
#include "mailboxd/session.h"
#include "mailboxd/terminal.h"
#include "mailboxd/util.h"
#include "mailboxd/log.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/select.h>
#include <sys/time.h>
#endif

static mailboxd_traffic_config_t g_traffic_config;
static int g_traffic_config_ready;

void mailboxd_traffic_config_defaults(mailboxd_traffic_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    cfg->baud = MAILBOXD_BAUD2400;
    cfg->line_width = MAILBOXD_LINE_WIDTH;
    cfg->pace_output = 1;
    cfg->ansi = 0;
    cfg->input_echo = 0;
}

#if !defined(MAILBOXD_CLIENT_BUILD)
static unsigned parse_uint_default(const mailboxd_config_t *config,
                                   const char *section,
                                   const char *key,
                                   unsigned default_value,
                                   unsigned min_value,
                                   unsigned max_value)
{
    unsigned value;

    value = mailboxd_config_get_uint(config, section, key, default_value,
                                  min_value, max_value);
    return value;
}
#endif

void mailboxd_traffic_config_apply(const struct mailboxd_config *config)
{
    mailboxd_traffic_config_defaults(&g_traffic_config);

#if !defined(MAILBOXD_CLIENT_BUILD)
    if (config != NULL) {
        g_traffic_config.baud = parse_uint_default(
            config, "traffic", "baud", MAILBOXD_BAUD2400, 300u, 38400u);
        g_traffic_config.line_width = parse_uint_default(
            config, "traffic", "line_width", MAILBOXD_LINE_WIDTH, 20u,
            MAILBOXD_LINE_WIDTH_MAX);
        g_traffic_config.pace_output =
            mailboxd_config_get_bool(config, "traffic", "pace_output", 1);
        g_traffic_config.ansi =
            mailboxd_config_get_bool(config, "traffic", "ansi", 0);
        g_traffic_config.input_echo =
            mailboxd_config_get_bool(config, "traffic", "input_echo", 0);
    }

    mailboxd_log_info("[traffic] baud=%u line_width=%u pace=%s ansi=%s echo=%s",
           g_traffic_config.baud, g_traffic_config.line_width,
           mailboxd_bool_to_string(g_traffic_config.pace_output),
           mailboxd_bool_to_string(g_traffic_config.ansi),
           mailboxd_bool_to_string(g_traffic_config.input_echo));
#else
    (void)config;
#endif

    g_traffic_config_ready = 1;
}

const mailboxd_traffic_config_t *mailboxd_traffic_config_get(void)
{
    if (!g_traffic_config_ready) {
        mailboxd_traffic_config_defaults(&g_traffic_config);
        g_traffic_config_ready = 1;
    }

    return &g_traffic_config;
}

unsigned mailboxd_traffic_byte_delay_us(unsigned baud)
{
    if (baud == 0) {
        return 0;
    }

    return 10000000u / baud;
}

static mailboxd_result_t emit_raw(struct mailboxd_session *session, char ch)
{
    const mailboxd_traffic_config_t *cfg = mailboxd_traffic_config_get();
    char byte = ch;
    mailboxd_result_t rc;

    if (session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (session->transport != NULL && session->transport->write != NULL) {
        rc = session->transport->write(session, &byte, 1);
    } else {
        if (fputc((unsigned char)byte, stdout) == EOF) {
            return MAILBOXD_ERR_IO;
        }
        fflush(stdout);
        rc = MAILBOXD_OK;
    }

    if (rc == MAILBOXD_OK && cfg->pace_output && cfg->baud > 0) {
        unsigned delay = mailboxd_traffic_byte_delay_us(cfg->baud);

        if (delay > 0) {
#if defined(_WIN32)
            Sleep((DWORD)((delay + 999u) / 1000u));
#else
            struct timeval tv;

            tv.tv_sec = (time_t)(delay / 1000000u);
#if defined(__AMIGA__)
            tv.tv_usec = (unsigned long)(delay % 1000000u);
#else
            tv.tv_usec = (suseconds_t)(delay % 1000000u);
#endif
            (void)select(0, NULL, NULL, NULL, &tv);
#endif
        }
    }

    return rc;
}

static mailboxd_result_t emit_newline(struct mailboxd_session *session,
                                   unsigned *out_col)
{
    mailboxd_result_t rc;

    rc = emit_raw(session, '\r');
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = emit_raw(session, '\n');
    if (rc == MAILBOXD_OK && out_col != NULL) {
        *out_col = 0;
    }

    return rc;
}

static int traffic_plain_char(const char *src, size_t len, size_t *consumed,
                              char *out)
{
    unsigned char ch;

    if (src == NULL || len == 0 || consumed == NULL || out == NULL) {
        return 0;
    }

    ch = (unsigned char)src[0];

    if (ch == '\r') {
        *consumed = 1;
        *out = '\n';
        return 1;
    }

    if (ch == '\n') {
        *consumed = 1;
        *out = '\n';
        return 1;
    }

    if (ch == '\t') {
        *consumed = 1;
        *out = ' ';
        return 1;
    }

    if (ch < 0x20 || ch == 0x7f) {
        *consumed = 1;
        return 0;
    }

    if (ch == 0x1b && len >= 2 && src[1] == '[') {
        size_t i = 2;

        while (i < len && (src[i] < 0x40 || src[i] > 0x7e)) {
            i++;
        }
        if (i < len) {
            *consumed = i + 1;
        } else {
            *consumed = len;
        }
        return 0;
    }

    if (ch == 0x1b) {
        *consumed = 1;
        return 0;
    }

    *consumed = 1;
    *out = (char)ch;
    return 1;
}

mailboxd_result_t mailboxd_traffic_emit(struct mailboxd_session *session,
                                  unsigned *out_col,
                                  const char *data, size_t len)
{
    const mailboxd_traffic_config_t *cfg = mailboxd_traffic_config_get();
    size_t pos = 0;
    unsigned col = out_col != NULL ? *out_col : 0;

    if (session == NULL || data == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (len == 0) {
        return MAILBOXD_OK;
    }

    if (cfg->ansi && strcmp(data, MAILBOXD_TERM_SGR_LIGHTGRAY_ON_BLACK) == 0 &&
        len == strlen(MAILBOXD_TERM_SGR_LIGHTGRAY_ON_BLACK)) {
        size_t i;

        for (i = 0; i < len; i++) {
            mailboxd_result_t rc = emit_raw(session, data[i]);

            if (rc != MAILBOXD_OK) {
                return rc;
            }
        }

        return MAILBOXD_OK;
    }

    while (pos < len) {
        char ch;
        size_t step = 0;
        mailboxd_result_t rc;

        if (!traffic_plain_char(data + pos, len - pos, &step, &ch)) {
            pos += step > 0 ? step : 1;
            continue;
        }

        pos += step;

        if (ch == '\n') {
            rc = emit_newline(session, out_col);
            if (rc != MAILBOXD_OK) {
                return rc;
            }
            col = 0;
            continue;
        }

        if (cfg->line_width > 0 && col >= cfg->line_width) {
            rc = emit_newline(session, out_col);
            if (rc != MAILBOXD_OK) {
                return rc;
            }
            col = 0;
        }

        rc = emit_raw(session, ch);
        if (rc != MAILBOXD_OK) {
            return rc;
        }

        col++;
    }

    if (out_col != NULL) {
        *out_col = col;
    }

    return MAILBOXD_OK;
}
