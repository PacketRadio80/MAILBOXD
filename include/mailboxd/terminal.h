/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_TERMINAL_H
#define MAILBOXD_TERMINAL_H

#include "mailboxd/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_session;

/**
 * MailboxD uses a single terminal palette: light gray text on a black background.
 * ANSI SGR: foreground 37, background 40.
 */
#define MAILBOXD_TERM_SGR_LIGHTGRAY_ON_BLACK "\033[37;40m"

/** Clear screen and move cursor home (requires `traffic.ansi = yes`). */
#define MAILBOXD_TERM_CLEAR_SCREEN "\033[2J\033[H"

/** Apply the MailboxD terminal colors on a session (sent once at connect). */
mailboxd_result_t mailboxd_term_init_session(struct mailboxd_session *session);

/** Clear the client screen (ANSI or plain newlines). */
mailboxd_result_t mailboxd_term_clear_screen(struct mailboxd_session *session);

/**
 * Copy @p src to @p dst without ANSI escape sequences (colors, cursor, etc.).
 * @return length of written string excluding the terminator.
 */
size_t mailboxd_term_copy_plain(const char *src, char *dst, size_t dst_size);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_TERMINAL_H */
