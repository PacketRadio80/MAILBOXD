/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_BANDWIDTH_POLICY_H
#define MAILBOXD_BANDWIDTH_POLICY_H

/**
 * Per-user bandwidth limits for telnet sessions under pressure. Among
 * interactive users the newest sessions are sacrificed first.
 */

#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAILBOXD_BANDWIDTH_DISCONNECT_MSG "You are disconnected by bandwidth limits."

typedef enum mailboxd_bandwidth_action {
    MAILBOXD_BANDWIDTH_NONE = 0,
    MAILBOXD_BANDWIDTH_PAUSE = 1,
    MAILBOXD_BANDWIDTH_BREAK = 2,
    MAILBOXD_BANDWIDTH_CANCEL = 3,
    MAILBOXD_BANDWIDTH_RESUME = 4
} mailboxd_bandwidth_action_t;

struct mailboxd_service;

/** Logged-in telnet users subject to limits. */
unsigned mailboxd_bandwidth_policy_user_count(struct mailboxd_service *service);

/**
 * Apply pause / break / cancel / resume to online users (newest first).
 * Returns the number of users affected.
 */
unsigned mailboxd_bandwidth_policy_apply(struct mailboxd_service *service,
                                      mailboxd_bandwidth_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_BANDWIDTH_POLICY_H */
