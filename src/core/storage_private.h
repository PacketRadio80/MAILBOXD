/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_STORAGE_PRIVATE_H
#define MAILBOXD_STORAGE_PRIVATE_H

#include "mailboxd/auth.h"
#include "mailboxd/storage.h"

#include <stddef.h>

struct mailboxd_storage {
    mailboxd_storage_backend_kind_t backend;
    char *path;
    char guest_prefix[MAILBOXD_AUTH_GUEST_PREFIX_MAX];
    mailboxd_storage_sql_config_t sql_cfg;
    void *backend_data;
};

mailboxd_result_t mailboxd_storage_flatfile_open(mailboxd_storage_t *storage);
void mailboxd_storage_flatfile_close(mailboxd_storage_t *storage);
mailboxd_result_t mailboxd_storage_flatfile_session_begin(mailboxd_storage_t *storage,
                                                    const mailboxd_user_record_t *user,
                                                    const char *transport,
                                                    mailboxd_session_record_t *out);
mailboxd_result_t mailboxd_storage_flatfile_session_end(mailboxd_storage_t *storage,
                                                  uint64_t session_id);
mailboxd_result_t mailboxd_storage_flatfile_find_user(mailboxd_storage_t *storage,
                                               const char *username,
                                               mailboxd_user_record_t *out);
mailboxd_result_t mailboxd_storage_flatfile_resolve_user(mailboxd_storage_t *storage,
                                                   const char *name,
                                                   mailboxd_user_record_t *out);
mailboxd_result_t mailboxd_storage_flatfile_count_level(mailboxd_storage_t *storage,
                                                 mailboxd_user_level_t level,
                                                 size_t *count);
mailboxd_result_t mailboxd_storage_flatfile_foreach_user(
    mailboxd_storage_t *storage,
    mailboxd_storage_user_fn fn,
    void *ctx);
mailboxd_result_t mailboxd_storage_flatfile_register_user(mailboxd_storage_t *storage,
                                                    const mailboxd_user_registration_t *reg,
                                                    mailboxd_user_record_t *out);
mailboxd_result_t mailboxd_storage_flatfile_update_user(mailboxd_storage_t *storage,
                                                  const mailboxd_user_record_t *user);
mailboxd_result_t mailboxd_storage_flatfile_delete_user(mailboxd_storage_t *storage,
                                                  uint64_t user_id);

mailboxd_result_t mailboxd_storage_sql_open(mailboxd_storage_t *storage);
void mailboxd_storage_sql_close(mailboxd_storage_t *storage);
mailboxd_result_t mailboxd_storage_sql_register_user(mailboxd_storage_t *storage,
                                               const mailboxd_user_registration_t *reg,
                                               mailboxd_user_record_t *out);
mailboxd_result_t mailboxd_storage_sql_find_user(mailboxd_storage_t *storage,
                                           const char *username,
                                           mailboxd_user_record_t *out);
mailboxd_result_t mailboxd_storage_sql_resolve_user(mailboxd_storage_t *storage,
                                              const char *name,
                                              mailboxd_user_record_t *out);
mailboxd_result_t mailboxd_storage_sql_count_level(mailboxd_storage_t *storage,
                                             mailboxd_user_level_t level,
                                             size_t *count);
mailboxd_result_t mailboxd_storage_sql_foreach_user(mailboxd_storage_t *storage,
                                              mailboxd_storage_user_fn fn,
                                              void *ctx);
mailboxd_result_t mailboxd_storage_sql_update_user(mailboxd_storage_t *storage,
                                             const mailboxd_user_record_t *user);
mailboxd_result_t mailboxd_storage_sql_delete_user(mailboxd_storage_t *storage,
                                             uint64_t user_id);
mailboxd_result_t mailboxd_storage_sql_session_begin(mailboxd_storage_t *storage,
                                               const mailboxd_user_record_t *user,
                                               const char *transport,
                                               mailboxd_session_record_t *out);
mailboxd_result_t mailboxd_storage_sql_session_end(mailboxd_storage_t *storage,
                                             uint64_t session_id);
void mailboxd_storage_sql_backup_files(const mailboxd_storage_t *storage);
void mailboxd_storage_sql_backup_tick(mailboxd_storage_t *storage);

#ifdef MAILBOXD_HAVE_SQLITE
struct sqlite3;
struct sqlite3 *mailboxd_storage_sql_mail_db(mailboxd_storage_t *storage);
#endif

#endif /* MAILBOXD_STORAGE_PRIVATE_H */
