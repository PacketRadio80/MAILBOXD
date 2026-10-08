/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_COMMAND_H
#define MAILBOXD_COMMAND_H

#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_service;
struct mailboxd_session;

/**
 * Input line routing scope.
 *
 * MailboxD accepts only lines starting with '/'. Valid forms:
 *   /              or /command /cmd /commands → /help
 *   /<verb>        or / <verb>              → command
 *   /command <verb> or /cmd <verb>          → same as / <verb>
 *
 * - COMMENT: starts with ';' or '#' — ignored (like empty line)
 * - LOCAL: anything else — not MailboxD; silently ignored
 */
typedef enum mailboxd_command_scope {
    MAILBOXD_CMD_SCOPE_MAILBOXD = 1,
    MAILBOXD_CMD_SCOPE_LOCAL = 2,
    MAILBOXD_CMD_SCOPE_COMMENT = 3
} mailboxd_command_scope_t;

typedef struct mailboxd_parsed_command {
    mailboxd_command_scope_t scope;
    char *line;
    char *verb;
    char **argv;
    size_t argc;
} mailboxd_parsed_command_t;

/**
 * Classify a single input line.
 * Returns MAILBOXD_CMD_SCOPE_MAILBOXD only when the first non-whitespace
 * character is '/'.
 */
mailboxd_command_scope_t mailboxd_command_classify(const char *line);

/** Non-zero when @p line is a MailboxD system command (starts with '/'). */
int mailboxd_command_is_mailboxd(const char *line);

/** Non-zero when @p line is a local comment (';' or '#' after whitespace). */
int mailboxd_command_is_comment(const char *line);

/**
 * Parse a trimmed command line into verb and arguments.
 * For MailboxD commands the leading '/' is stripped from the verb.
 */
mailboxd_result_t mailboxd_command_parse(const char *line,
                                   mailboxd_parsed_command_t *out);

void mailboxd_command_free(mailboxd_parsed_command_t *cmd);

/**
 * Dispatch a parsed MailboxD command (scope must be MAILBOXD_CMD_SCOPE_MAILBOXD).
 */
mailboxd_result_t mailboxd_command_dispatch(struct mailboxd_service *service,
                                      struct mailboxd_session *session,
                                      const mailboxd_parsed_command_t *cmd);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_COMMAND_H */
