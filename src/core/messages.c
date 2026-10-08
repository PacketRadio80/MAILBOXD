/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/messages.h"
#include "mailboxd/session.h"
#include "mailboxd/traffic.h"

#include <stdio.h>
#include <string.h>

static mailboxd_result_t msg_format_prefixed(char *out, size_t out_len,
                                          const char *prefix,
                                          const char *from_label,
                                          const char *body)
{
    int n;

    if (out == NULL || out_len == 0 || prefix == NULL || body == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (from_label != NULL && from_label[0] != '\0') {
        n = snprintf(out, out_len, "%s%s: %s", prefix, from_label, body);
    } else {
        n = snprintf(out, out_len, "%s%s", prefix, body);
    }

    if (n < 0 || (size_t)n >= out_len) {
        return MAILBOXD_ERR_INVALID;
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_msg_format_system(char *out, size_t out_len,
                                       const char *body)
{
    return msg_format_prefixed(out, out_len, MAILBOXD_MSG_PREFIX_SYSTEM,
                               NULL, body);
}

mailboxd_result_t mailboxd_msg_format_sysop(char *out, size_t out_len,
                                      const char *from_label,
                                      const char *body)
{
    return msg_format_prefixed(out, out_len, MAILBOXD_MSG_PREFIX_SYSOP,
                               from_label, body);
}

mailboxd_result_t mailboxd_msg_format_private(char *out, size_t out_len,
                                        const char *from_label,
                                        const char *body)
{
    return msg_format_prefixed(out, out_len, MAILBOXD_MSG_PREFIX_PRIVATE,
                               from_label, body);
}

static mailboxd_result_t msg_send_formatted(struct mailboxd_session *session,
                                         mailboxd_result_t (*format_fn)(char *,
                                                                     size_t,
                                                                     const char *,
                                                                     const char *),
                                         const char *from_label,
                                         const char *body)
{
    char line[MAILBOXD_LINE_MAX];
    mailboxd_result_t rc;

    if (session == NULL || body == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = format_fn(line, sizeof(line), from_label, body);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    return mailboxd_session_write_line(session, line);
}

mailboxd_result_t mailboxd_msg_send_system(struct mailboxd_session *session,
                                     const char *body)
{
    char line[MAILBOXD_LINE_MAX];
    mailboxd_result_t rc;

    if (session == NULL || body == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_msg_format_system(line, sizeof(line), body);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    return mailboxd_session_write_line(session, line);
}

mailboxd_result_t mailboxd_msg_send_sysop(struct mailboxd_session *session,
                                    const char *from_label,
                                    const char *body)
{
    return msg_send_formatted(session, mailboxd_msg_format_sysop,
                              from_label, body);
}

mailboxd_result_t mailboxd_msg_send_private(struct mailboxd_session *session,
                                      const char *from_label,
                                      const char *body)
{
    return msg_send_formatted(session, mailboxd_msg_format_private,
                              from_label, body);
}
