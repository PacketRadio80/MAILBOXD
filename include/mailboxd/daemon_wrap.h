/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_DAEMON_WRAP_H
#define MAILBOXD_DAEMON_WRAP_H

#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAILBOXD_DAEMON_DEFAULT_SESSION "mailboxd"
#define MAILBOXD_DAEMON_WRAP_ENV "MAILBOXDD_WRAPPED"

typedef struct mailboxd_daemon_launch_opts {
    int foreground;
    int attach;
    int use_screen;
    int use_tmux;
    char session[32];
} mailboxd_daemon_launch_opts_t;

void mailboxd_daemon_launch_opts_defaults(mailboxd_daemon_launch_opts_t *opts);

/**
 * Handle --screen / --tmux / --attach before the service loop.
 * Spawns or attaches via GNU screen or tmux when requested.
 * Returns MAILBOXD_OK to continue a foreground run; otherwise exits the process.
 */
mailboxd_result_t mailboxd_daemon_apply_launch_opts(const mailboxd_daemon_launch_opts_t *opts,
                                              const char *binary_path,
                                              int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_DAEMON_WRAP_H */
