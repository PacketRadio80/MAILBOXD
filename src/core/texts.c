/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/texts.h"
#include "mailboxd/session.h"
#include "mailboxd/service.h"
#include "mailboxd/auth.h"
#include "mailboxd/util.h"
#include "mailboxd/limits.h"
#include "mailboxd/mailboxd.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

void mailboxd_texts_config_defaults(mailboxd_texts_config_t *texts)
{
    if (texts == NULL) {
        return;
    }

    mailboxd_strlcpy(texts->path, MAILBOXD_DEFAULT_TEXTS_PATH, sizeof(texts->path));
}

mailboxd_result_t mailboxd_texts_resolve(const mailboxd_texts_config_t *texts,
                                   const char *filename,
                                   char *out, size_t out_len)
{
    if (texts == NULL || filename == NULL || out == NULL || out_len == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    return mailboxd_path_join(out, out_len, texts->path, filename);
}

static size_t append_token_expanded(char *out, size_t out_len, size_t pos,
                                    const char *value)
{
    size_t value_len;

    if (out == NULL || out_len == 0 || value == NULL) {
        return pos;
    }

    value_len = strlen(value);

    if (pos + value_len >= out_len) {
        return out_len - 1;
    }

    memcpy(out + pos, value, value_len);
    return pos + value_len;
}

static void expand_text_line(char *out, size_t out_len,
                             const char *line,
                             const char *version,
                             const char *service_name,
                             const char *username,
                             const char *os_name,
                             const char *time_text,
                             const char *date_text)
{
    size_t pos = 0;
    size_t i = 0;

    if (out == NULL || out_len == 0 || line == NULL) {
        return;
    }

    out[0] = '\0';

    if (version == NULL) {
        version = MAILBOXD_VERSION_STRING;
    }
    if (service_name == NULL || service_name[0] == '\0') {
        service_name = MAILBOXD_DEFAULT_SERVICE_NAME;
    }
    if (username == NULL) {
        username = "";
    }
    if (os_name == NULL) {
        os_name = "";
    }
    if (time_text == NULL) {
        time_text = "";
    }
    if (date_text == NULL) {
        date_text = "";
    }

    while (line[i] != '\0' && pos + 1 < out_len) {
        if (strncmp(line + i, MAILBOXD_BANNER_TOKEN_VERSION,
                    strlen(MAILBOXD_BANNER_TOKEN_VERSION)) == 0) {
            pos = append_token_expanded(out, out_len, pos, version);
            i += strlen(MAILBOXD_BANNER_TOKEN_VERSION);
            continue;
        }

        if (strncmp(line + i, MAILBOXD_BANNER_TOKEN_SERVICE,
                    strlen(MAILBOXD_BANNER_TOKEN_SERVICE)) == 0) {
            pos = append_token_expanded(out, out_len, pos, service_name);
            i += strlen(MAILBOXD_BANNER_TOKEN_SERVICE);
            continue;
        }

        if (strncmp(line + i, MAILBOXD_TEXT_TOKEN_USERNAME,
                    strlen(MAILBOXD_TEXT_TOKEN_USERNAME)) == 0) {
            pos = append_token_expanded(out, out_len, pos, username);
            i += strlen(MAILBOXD_TEXT_TOKEN_USERNAME);
            continue;
        }

        if (strncmp(line + i, MAILBOXD_TEXT_TOKEN_OS,
                    strlen(MAILBOXD_TEXT_TOKEN_OS)) == 0) {
            pos = append_token_expanded(out, out_len, pos, os_name);
            i += strlen(MAILBOXD_TEXT_TOKEN_OS);
            continue;
        }

        if (strncmp(line + i, MAILBOXD_TEXT_TOKEN_TIME,
                    strlen(MAILBOXD_TEXT_TOKEN_TIME)) == 0) {
            pos = append_token_expanded(out, out_len, pos, time_text);
            i += strlen(MAILBOXD_TEXT_TOKEN_TIME);
            continue;
        }

        if (strncmp(line + i, MAILBOXD_TEXT_TOKEN_DATE,
                    strlen(MAILBOXD_TEXT_TOKEN_DATE)) == 0) {
            pos = append_token_expanded(out, out_len, pos, date_text);
            i += strlen(MAILBOXD_TEXT_TOKEN_DATE);
            continue;
        }

        out[pos++] = line[i++];
    }

    out[pos] = '\0';
}

static void format_text_tokens(char *time_text, size_t time_len,
                               char *date_text, size_t date_len,
                               const struct tm *tm)
{
    if (time_text != NULL && time_len > 0) {
        time_text[0] = '\0';
        if (tm != NULL) {
            (void)mailboxd_time_format_time(time_text, time_len, tm, NULL);
        }
    }

    if (date_text != NULL && date_len > 0) {
        date_text[0] = '\0';
        if (tm != NULL) {
            (void)mailboxd_time_format_date(date_text, date_len, tm, NULL);
        }
    }
}

static void fill_os_name(char *os_name, size_t os_len)
{
    if (os_name == NULL || os_len == 0) {
        return;
    }

    if (mailboxd_platform_os_name(os_name, os_len) != MAILBOXD_OK) {
        mailboxd_strlcpy(os_name, "unknown", os_len);
    }
}

static void expand_banner_line(char *out, size_t out_len,
                               const char *line,
                               const char *version,
                               const char *service_name,
                               const char *time_text,
                               const char *date_text)
{
    expand_text_line(out, out_len, line, version, service_name, NULL, NULL,
                     time_text, date_text);
}

typedef struct texts_line_emitter {
    mailboxd_session_t *session;
    unsigned pending_blank;
} texts_line_emitter_t;

static void texts_emit_expanded_line(texts_line_emitter_t *emitter,
                                     const char *expanded)
{
    unsigned i;

    if (emitter == NULL || emitter->session == NULL || expanded == NULL) {
        return;
    }

    if (expanded[0] == '\0') {
        emitter->pending_blank++;
        return;
    }

    for (i = 0; i < emitter->pending_blank; i++) {
        mailboxd_session_write(emitter->session, "\n");
    }
    emitter->pending_blank = 0;
    mailboxd_session_write_line(emitter->session, expanded);
}

static void texts_trim_trailing_cr(char *expanded)
{
    size_t n;

    if (expanded == NULL) {
        return;
    }

    n = strlen(expanded);
    while (n > 0 && (expanded[n - 1] == '\n' || expanded[n - 1] == '\r')) {
        expanded[--n] = '\0';
    }
}

static mailboxd_result_t send_banner_fallback(mailboxd_session_t *session,
                                           const char *version,
                                           const char *service_name)
{
    char line[MAILBOXD_LINE_MAX];

    if (version == NULL) {
        version = MAILBOXD_VERSION_STRING;
    }
    if (service_name == NULL || service_name[0] == '\0') {
        service_name = MAILBOXD_DEFAULT_SERVICE_NAME;
    }

    snprintf(line, sizeof(line), "MailboxD %s", version);
    mailboxd_session_write_line(session, line);
    snprintf(line, sizeof(line), "Online at %s", service_name);
    mailboxd_session_write_line(session, line);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_texts_send_banner(const mailboxd_texts_config_t *texts,
                                       mailboxd_session_t *session,
                                       const char *version,
                                       const char *service_name)
{
    char path[MAILBOXD_PATH_MAX];
    char expanded[MAILBOXD_LINE_MAX];
    FILE *fp;
    char line[MAILBOXD_LINE_MAX];
    char time_text[32];
    char date_text[32];
    struct tm now_tm;
    const struct tm *tm_ptr = NULL;
    mailboxd_result_t rc;

    if (texts == NULL || session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_time_local_now(&now_tm) == MAILBOXD_OK) {
        tm_ptr = &now_tm;
    }
    format_text_tokens(time_text, sizeof(time_text),
                       date_text, sizeof(date_text), tm_ptr);

    rc = mailboxd_texts_resolve(texts, MAILBOXD_TEXT_BANNER, path, sizeof(path));
    if (rc != MAILBOXD_OK) {
        return send_banner_fallback(session, version, service_name);
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return send_banner_fallback(session, version, service_name);
    }

    {
        texts_line_emitter_t emitter;

        emitter.session = session;
        emitter.pending_blank = 0;

        while (fgets(line, sizeof(line), fp) != NULL) {
            expand_banner_line(expanded, sizeof(expanded), line, version,
                              service_name, time_text, date_text);
            texts_trim_trailing_cr(expanded);
            texts_emit_expanded_line(&emitter, expanded);
        }
    }

    fclose(fp);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_texts_send_motd(const mailboxd_texts_config_t *texts,
                                     mailboxd_session_t *session)
{
    char path[MAILBOXD_PATH_MAX];
    char expanded[MAILBOXD_LINE_MAX];
    FILE *fp;
    char line[MAILBOXD_LINE_MAX];
    char time_text[32];
    char date_text[32];
    struct tm now_tm;
    const struct tm *tm_ptr = NULL;
    const char *username;
    mailboxd_result_t rc;

    if (texts == NULL || session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    username = mailboxd_session_display_name(session);
    if (username[0] == '\0') {
        username = "visitor";
    }

    if (mailboxd_time_local_now(&now_tm) == MAILBOXD_OK) {
        tm_ptr = &now_tm;
    }
    format_text_tokens(time_text, sizeof(time_text),
                       date_text, sizeof(date_text), tm_ptr);

    rc = mailboxd_texts_resolve(texts, MAILBOXD_TEXT_MOTD, path, sizeof(path));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    {
        texts_line_emitter_t emitter;

        emitter.session = session;
        emitter.pending_blank = 0;

        while (fgets(line, sizeof(line), fp) != NULL) {
            expand_text_line(expanded, sizeof(expanded), line, NULL, NULL,
                             username, NULL, time_text, date_text);
            texts_trim_trailing_cr(expanded);
            texts_emit_expanded_line(&emitter, expanded);
        }
    }

    fclose(fp);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_texts_send_file(const mailboxd_texts_config_t *texts,
                                     mailboxd_session_t *session,
                                     const char *filename)
{
    char path[MAILBOXD_PATH_MAX];
    char expanded[MAILBOXD_LINE_MAX];
    FILE *fp;
    char line[MAILBOXD_LINE_MAX];
    char time_text[32];
    char date_text[32];
    char os_name[64];
    struct tm now_tm;
    const struct tm *tm_ptr = NULL;
    mailboxd_result_t rc;

    if (texts == NULL || session == NULL || filename == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_time_local_now(&now_tm) == MAILBOXD_OK) {
        tm_ptr = &now_tm;
    }
    format_text_tokens(time_text, sizeof(time_text),
                       date_text, sizeof(date_text), tm_ptr);
    fill_os_name(os_name, sizeof(os_name));

    rc = mailboxd_texts_resolve(texts, filename, path, sizeof(path));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    {
        texts_line_emitter_t emitter;

        emitter.session = session;
        emitter.pending_blank = 0;

        while (fgets(line, sizeof(line), fp) != NULL) {
            expand_text_line(expanded, sizeof(expanded), line,
                             MAILBOXD_VERSION_STRING, NULL, NULL, os_name,
                             time_text, date_text);
            texts_trim_trailing_cr(expanded);
            texts_emit_expanded_line(&emitter, expanded);
        }
    }

    fclose(fp);
    return MAILBOXD_OK;
}

static mailboxd_result_t send_version_fallback(mailboxd_session_t *session,
                                            const char *os_name)
{
    char line[MAILBOXD_LINE_MAX];

    snprintf(line, sizeof(line), "MailboxD %s", MAILBOXD_VERSION_STRING);
    mailboxd_session_write_line(session, line);
    snprintf(line, sizeof(line), "Operating system: %s",
             (os_name != NULL && os_name[0] != '\0') ? os_name : "unknown");
    mailboxd_session_write_line(session, line);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_texts_send_version(const mailboxd_texts_config_t *texts,
                                        mailboxd_session_t *session)
{
    char path[MAILBOXD_PATH_MAX];
    char expanded[MAILBOXD_LINE_MAX];
    FILE *fp;
    char line[MAILBOXD_LINE_MAX];
    char time_text[32];
    char date_text[32];
    char os_name[64];
    struct tm now_tm;
    const struct tm *tm_ptr = NULL;
    mailboxd_result_t rc;

    if (texts == NULL || session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    fill_os_name(os_name, sizeof(os_name));

    if (mailboxd_time_local_now(&now_tm) == MAILBOXD_OK) {
        tm_ptr = &now_tm;
    }
    format_text_tokens(time_text, sizeof(time_text),
                       date_text, sizeof(date_text), tm_ptr);

    rc = mailboxd_texts_resolve(texts, MAILBOXD_TEXT_VERSION, path, sizeof(path));
    if (rc != MAILBOXD_OK) {
        return send_version_fallback(session, os_name);
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return send_version_fallback(session, os_name);
    }

    {
        texts_line_emitter_t emitter;

        emitter.session = session;
        emitter.pending_blank = 0;

        while (fgets(line, sizeof(line), fp) != NULL) {
            expand_text_line(expanded, sizeof(expanded), line,
                             MAILBOXD_VERSION_STRING, NULL, NULL, os_name,
                             time_text, date_text);
            texts_trim_trailing_cr(expanded);
            texts_emit_expanded_line(&emitter, expanded);
        }
    }

    fclose(fp);
    return MAILBOXD_OK;
}
