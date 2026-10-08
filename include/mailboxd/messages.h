/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_MESSAGES_H
#define MAILBOXD_MESSAGES_H

#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_session;

/** Automatic system notices (login, mail, timeouts, conference status, …). */
#define MAILBOXD_MSG_PREFIX_SYSTEM "*** "

/** Sysop / granted manual system-wide notices (`/broadcast`, `/announce`). */
#define MAILBOXD_MSG_PREFIX_SYSOP "+++ "

/** Private directed messages (system or Sysop+granted); v1 half-stub. */
#define MAILBOXD_MSG_PREFIX_PRIVATE "### "

/**
 * Format a system line: `*** <body>`
 */
mailboxd_result_t mailboxd_msg_format_system(char *out, size_t out_len,
                                     const char *body);

/**
 * Format a Sysop/granted system-wide line: `+++ <from>: <body>`
 * @p from_label may be NULL → `+++ <body>`
 */
mailboxd_result_t mailboxd_msg_format_sysop(char *out, size_t out_len,
                                      const char *from_label,
                                      const char *body);

/**
 * Format a private line: `### <from>: <body>` or `### <body>` when @p from_label is NULL.
 */
mailboxd_result_t mailboxd_msg_format_private(char *out, size_t out_len,
                                        const char *from_label,
                                        const char *body);

mailboxd_result_t mailboxd_msg_send_system(struct mailboxd_session *session,
                                     const char *body);

mailboxd_result_t mailboxd_msg_send_sysop(struct mailboxd_session *session,
                                    const char *from_label,
                                    const char *body);

/** Half-stub: directed private message to one session. */
mailboxd_result_t mailboxd_msg_send_private(struct mailboxd_session *session,
                                      const char *from_label,
                                      const char *body);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_MESSAGES_H */
