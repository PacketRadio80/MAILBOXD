/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_CONFERENCE_H
#define MAILBOXD_CONFERENCE_H

#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_service;
struct mailboxd_session;

#define MAILBOXD_CONFERENCE_TOPIC_MAX 32

/** Invite rate limit: max invites per target user within the window. */
#define MAILBOXD_CONFERENCE_INVITE_MAX_PER_TARGET 2u

/** Invite rate limit window (seconds). */
#define MAILBOXD_CONFERENCE_INVITE_WINDOW_SEC 1800u

/**
 * Send a conference invite: @p topic to @p partner (login or nickname).
 * Partner must accept with y/n or yes/no. Max two invites per target per 30 min.
 */
mailboxd_result_t mailboxd_conference_start(struct mailboxd_service *service,
                                      struct mailboxd_session *initiator,
                                      const char *topic,
                                      const char *partner);

/**
 * Monitor/sysop meet: invite @p partner with timeout, or @p force join immediately.
 * Topic defaults to "monitor". Status lines go to initiator.
 */
mailboxd_result_t mailboxd_conference_monitor_meet(struct mailboxd_service *service,
                                             struct mailboxd_session *initiator,
                                             const char *partner,
                                             int force);

/** Expire timed-out invites (monitor path). Safe to call every session tick. */
void mailboxd_conference_invite_tick(struct mailboxd_service *service,
                                  struct mailboxd_session *session);

/** Non-zero when @p session has a pending conference invite to answer. */
int mailboxd_conference_invite_pending(const struct mailboxd_session *session);

/**
 * Handle invite reply line (y/n, yes/no). Call for local input while invite pending.
 * Returns MAILBOXD_OK when consumed.
 */
mailboxd_result_t mailboxd_conference_reply_invite(struct mailboxd_service *service,
                                           struct mailboxd_session *session,
                                           const char *line);

/** Clear pending invites involving @p session (disconnect). */
void mailboxd_conference_session_closed(struct mailboxd_session *session);

/** Deliver a line within an active conference (sender must be in conference area). */
mailboxd_result_t mailboxd_conference_post(struct mailboxd_service *service,
                                     struct mailboxd_session *from,
                                     const char *message);

/** Tear down conference state when leaving the conference area. */
void mailboxd_conference_area_leaving(struct mailboxd_session *session);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_CONFERENCE_H */
