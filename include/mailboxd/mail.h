/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_MAIL_H
#define MAILBOXD_MAIL_H

#include "mailboxd/config.h"
#include "mailboxd/storage.h"
#include "mailboxd/types.h"

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_service;
struct mailboxd_session;

/** Maximum stored messages per user inbox. */
#define MAILBOXD_MAIL_MAX_MESSAGES 50u

/** Subject line limit (80-column profile). */
#define MAILBOXD_MAIL_SUBJECT_MAX 72u

/** Total message body size (all lines). */
#define MAILBOXD_MAIL_BODY_MAX 2048u

/** Default days before recycled mail is permanently removed. */
#define MAILBOXD_MAIL_DEFAULT_RECYCLE_DAYS 10u

typedef struct mailboxd_mail_config {
    int enabled;
    unsigned max_messages;
    unsigned subject_max;
    unsigned body_max;
    unsigned recycle_days;
    /** Root mail directory (typically <storage>/mail). */
    char root[512];
} mailboxd_mail_config_t;

typedef struct mailboxd_mail_entry {
    uint64_t id;
    char from[64];
    char subject[MAILBOXD_MAIL_SUBJECT_MAX];
    time_t received_at;
    int read;
} mailboxd_mail_entry_t;

void mailboxd_mail_config_defaults(mailboxd_mail_config_t *mail);

/** Load `[mail]` and set @p mail->root under @p storage_path. */
void mailboxd_mail_config_apply(mailboxd_mail_config_t *mail,
                             const mailboxd_config_t *config,
                             const char *storage_path);

const mailboxd_mail_config_t *mailboxd_service_get_mail(const struct mailboxd_service *service);

/** List inbox on @p session (registered users only). */
void mailboxd_mail_list_inbox(struct mailboxd_service *service,
                           struct mailboxd_session *session);

/**
 * List inbox rows @p from..@p to (1-based, newest first).
 * @p to = 0 means through the last message.
 */
void mailboxd_mail_list_inbox_range(struct mailboxd_service *service,
                                 struct mailboxd_session *session,
                                 unsigned from, unsigned to);

/**
 * After login: if mail arrived since @p since_login, announce the count.
 * No output when mail is disabled, the user is a guest, or the count is zero.
 * @p since_login 0 means first login (all inbox messages count as new).
 */
void mailboxd_mail_announce_since_last_login(struct mailboxd_service *service,
                                          struct mailboxd_session *session,
                                          time_t since_login);

/**
 * Parse @p spec as @c from-to (e.g. @c 1-15 , @c 5-20 ) or a single index.
 * Returns 0 on invalid input.
 */
int mailboxd_mail_parse_list_range(const char *spec,
                                unsigned *from, unsigned *to);

/**
 * Read message by 1-based list index (newest first).
 * Marks the message as read.
 */
mailboxd_result_t mailboxd_mail_read(struct mailboxd_service *service,
                               struct mailboxd_session *session,
                               unsigned list_index);

/** Move inbox message(s) to recycle (1-based index or range). */
mailboxd_result_t mailboxd_mail_delete(struct mailboxd_service *service,
                                 struct mailboxd_session *session,
                                 unsigned list_index);

mailboxd_result_t mailboxd_mail_delete_range(struct mailboxd_service *service,
                                       struct mailboxd_session *session,
                                       unsigned from, unsigned to);

/** Permanently remove all messages in the user's recycle bin. */
mailboxd_result_t mailboxd_mail_recycle_empty(struct mailboxd_service *service,
                                        struct mailboxd_session *session);

/**
 * Deliver a message from @p from_user to @p to_user.
 * @p to_user must exist and be an active registered account.
 */
mailboxd_result_t mailboxd_mail_deliver(struct mailboxd_service *service,
                                  const char *from_user,
                                  const char *to_user,
                                  const char *subject,
                                  const char *body);

/**
 * Mail active Sysop and Admin when a guest self-registers.
 * Body lists every field from `/register` plus assigned account metadata.
 * No-op when mail is disabled. Individual delivery failures are ignored.
 */
mailboxd_result_t mailboxd_mail_notify_staff_registration(
    struct mailboxd_service *service,
    const mailboxd_user_registration_t *reg,
    const mailboxd_user_record_t *user);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_MAIL_H */
