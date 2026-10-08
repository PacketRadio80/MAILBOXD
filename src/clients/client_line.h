/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_CLIENT_LINE_H
#define MAILBOXD_CLIENT_LINE_H

#include "mailboxd/limits.h"
#include "mailboxd/types.h"

#include <stddef.h>
#include <termios.h>

typedef struct mailboxd_client_line_editor {
    char line[MAILBOXD_LINE_MAX];
    size_t line_len;
    size_t line_cursor;
    char history[MAILBOXD_HISTORY_MAX][MAILBOXD_LINE_MAX];
    unsigned history_count;
    unsigned history_next;
    int history_view;
    char history_saved_line[MAILBOXD_LINE_MAX];
    int raw_enabled;
    int stdin_fd;
    unsigned char esc_state;
    char csi_buf[24];
    size_t csi_len;
    struct termios termios_saved;
} mailboxd_client_line_editor_t;

int mailboxd_client_line_editor_start(mailboxd_client_line_editor_t *ed, int stdin_fd);
void mailboxd_client_line_editor_stop(mailboxd_client_line_editor_t *ed);
mailboxd_result_t mailboxd_client_line_editor_poll(mailboxd_client_line_editor_t *ed,
                                             char *out_line, size_t out_len,
                                             int *have_line, int *eof_seen);

#endif /* MAILBOXD_CLIENT_LINE_H */
