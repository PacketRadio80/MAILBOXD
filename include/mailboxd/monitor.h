/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_MONITOR_H
#define MAILBOXD_MONITOR_H

#include "mailboxd/limits.h"
#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_config;
struct mailboxd_service;
struct mailboxd_session;

#define MAILBOXD_MONITOR_ALLOW_MAX 16u
#define MAILBOXD_MONITOR_ALLOW_NAME_MAX 64u

typedef struct mailboxd_monitor_config {
    int enabled;
    int follow_mailboxd;
    int follow_security;
    /** When set, Sysop sessions are always hidden from /who and /online. */
    int invisible_sysop;
    unsigned invite_timeout_sec;
    char allow[MAILBOXD_MONITOR_ALLOW_MAX][MAILBOXD_MONITOR_ALLOW_NAME_MAX];
    unsigned allow_count;
} mailboxd_monitor_config_t;

void mailboxd_monitor_config_defaults(mailboxd_monitor_config_t *cfg);
void mailboxd_monitor_config_apply(const struct mailboxd_config *config);
const mailboxd_monitor_config_t *mailboxd_monitor_config_get(void);

/** Non-zero when [monitor] enabled=yes. */
int mailboxd_monitor_enabled(void);

/** Non-zero when [monitor] invisible-sysop=yes (Sysop always /who-hidden). */
int mailboxd_monitor_invisible_sysop(void);

/** Non-zero when @p username is listed in [monitor] allow=. */
int mailboxd_monitor_user_allowed(const char *username);

/**
 * Non-zero when @p session may use /monitor (Sysop/registry OR allow list).
 * Caller still runs normal command access for registry min-level.
 */
int mailboxd_monitor_session_may_use(const struct mailboxd_session *session);

/** Enable/disable monitor mode on @p session. */
mailboxd_result_t mailboxd_monitor_set_active(struct mailboxd_session *session, int on);

int mailboxd_monitor_is_active(const struct mailboxd_session *session);

/**
 * Push a live line to every session with monitor mode on.
 * Prefixes with "[monitor] " for clarity.
 */
void mailboxd_monitor_broadcast(struct mailboxd_service *service, const char *line);

/** Format Username@plugin event and push live (always when any monitor on). */
void mailboxd_monitor_event(struct mailboxd_service *service,
                         const char *username,
                         const char *plugin,
                         const char *detail);

/** Poll log file tails (call from service loop). */
void mailboxd_monitor_tick(struct mailboxd_service *service);

void mailboxd_monitor_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_MONITOR_H */
