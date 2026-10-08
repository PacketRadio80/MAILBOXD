/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_MAIL_SQL_H
#define MAILBOXD_MAIL_SQL_H

#include "mailboxd/mail.h"
#include "mailboxd/storage.h"
#include "mailboxd/types.h"

struct mailboxd_service;
struct mailboxd_session;

mailboxd_result_t mailboxd_mail_sql_load_inbox(mailboxd_storage_t *storage,
                                         const char *username,
                                         mailboxd_mail_entry_t *entries,
                                         size_t max_entries,
                                         size_t *out_count);

mailboxd_result_t mailboxd_mail_sql_deliver(mailboxd_storage_t *storage,
                                      const mailboxd_mail_config_t *mail,
                                      const char *from_user,
                                      const char *to_user,
                                      const char *subject,
                                      const char *body);

mailboxd_result_t mailboxd_mail_sql_read(struct mailboxd_service *service,
                                   struct mailboxd_session *session,
                                   unsigned list_index);

mailboxd_result_t mailboxd_mail_sql_delete_range(struct mailboxd_service *service,
                                           struct mailboxd_session *session,
                                           unsigned from,
                                           unsigned to);

mailboxd_result_t mailboxd_mail_sql_recycle_empty(struct mailboxd_service *service,
                                            struct mailboxd_session *session);

unsigned mailboxd_mail_sql_purge_recycle(mailboxd_storage_t *storage,
                                      const mailboxd_mail_config_t *mail,
                                      const char *username);

#endif /* MAILBOXD_MAIL_SQL_H */
