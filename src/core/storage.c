/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/storage.h"
#include "mailboxd/auth.h"
#include "mailboxd/config.h"
#include "mailboxd/limits.h"
#include "mailboxd/util.h"
#include "storage_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAILBOXD_SQL_DEFAULT_USER_DB "users.db"
#define MAILBOXD_SQL_DEFAULT_MAIL_DB "mail.db"

static char *mailboxd_strdup(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }

    len = strlen(s) + 1;
    copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

void mailboxd_storage_sql_config_defaults(mailboxd_storage_sql_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    cfg->user_db[0] = '\0';
    cfg->mail_db[0] = '\0';
    cfg->backup_interval_sec = MAILBOXD_STORAGE_BACKUP_INTERVAL_DEFAULT_SEC;
    cfg->backup_path[0] = '\0';
}

void mailboxd_storage_sql_config_apply(mailboxd_storage_sql_config_t *cfg,
                                    const mailboxd_config_t *config,
                                    const char *storage_path)
{
    const char *user_db;
    const char *mail_db;
    const char *backup_path;

    mailboxd_storage_sql_config_defaults(cfg);
    if (storage_path == NULL || storage_path[0] == '\0') {
        return;
    }

    user_db = MAILBOXD_SQL_DEFAULT_USER_DB;
    mail_db = MAILBOXD_SQL_DEFAULT_MAIL_DB;
    backup_path = "";

    if (config != NULL) {
        user_db = mailboxd_config_get(config, "storage", "user_db",
                                   MAILBOXD_SQL_DEFAULT_USER_DB);
        mail_db = mailboxd_config_get(config, "storage", "mail_db",
                                   MAILBOXD_SQL_DEFAULT_MAIL_DB);
        backup_path = mailboxd_config_get(config, "storage", "backup_path", "");
        cfg->backup_interval_sec = mailboxd_config_get_uint(
            config, "storage", "backup_interval",
            MAILBOXD_STORAGE_BACKUP_INTERVAL_DEFAULT_SEC, 60u, 86400u);
    }

    (void)mailboxd_path_join(cfg->user_db, sizeof(cfg->user_db), storage_path,
                          user_db);
    (void)mailboxd_path_join(cfg->mail_db, sizeof(cfg->mail_db), storage_path,
                          mail_db);

    if (backup_path != NULL && backup_path[0] != '\0') {
        char resolved[MAILBOXD_PATH_MAX];

        if (mailboxd_path_resolve(resolved, sizeof(resolved), backup_path) ==
            MAILBOXD_OK) {
            mailboxd_strlcpy(cfg->backup_path, resolved, sizeof(cfg->backup_path));
        } else {
            (void)mailboxd_path_join(cfg->backup_path, sizeof(cfg->backup_path),
                                  storage_path, backup_path);
        }
    }
}

const mailboxd_storage_sql_config_t *mailboxd_storage_sql_config(
    const mailboxd_storage_t *storage)
{
    if (storage == NULL) {
        return NULL;
    }

    return &storage->sql_cfg;
}

void mailboxd_storage_backup_tick(mailboxd_storage_t *storage)
{
    mailboxd_storage_sql_backup_tick(storage);
}

static void storage_dispatch_sqlite(mailboxd_storage_t *storage,
                                  const mailboxd_storage_sql_config_t *sql_cfg)
{
    mailboxd_storage_sql_config_defaults(&storage->sql_cfg);
    if (sql_cfg != NULL) {
        storage->sql_cfg = *sql_cfg;
    } else if (storage->path != NULL) {
        char user_db[MAILBOXD_PATH_MAX];
        char mail_db[MAILBOXD_PATH_MAX];

        (void)mailboxd_path_join(user_db, sizeof(user_db), storage->path,
                              MAILBOXD_SQL_DEFAULT_USER_DB);
        (void)mailboxd_path_join(mail_db, sizeof(mail_db), storage->path,
                              MAILBOXD_SQL_DEFAULT_MAIL_DB);
        mailboxd_strlcpy(storage->sql_cfg.user_db, user_db,
                      sizeof(storage->sql_cfg.user_db));
        mailboxd_strlcpy(storage->sql_cfg.mail_db, mail_db,
                      sizeof(storage->sql_cfg.mail_db));
    }
}

mailboxd_storage_t *mailboxd_storage_open(const mailboxd_storage_options_t *options)
{
    mailboxd_storage_t *storage;
    mailboxd_result_t rc;

    if (options == NULL || options->path == NULL) {
        return NULL;
    }

    storage = calloc(1, sizeof(*storage));
    if (storage == NULL) {
        return NULL;
    }

    storage->backend = options->backend;
    storage->path = mailboxd_strdup(options->path);
    if (storage->path == NULL) {
        free(storage);
        return NULL;
    }

    if (options->guest_prefix != NULL && options->guest_prefix[0] != '\0') {
        mailboxd_strlcpy(storage->guest_prefix, options->guest_prefix,
                      sizeof(storage->guest_prefix));
    } else {
        mailboxd_strlcpy(storage->guest_prefix, MAILBOXD_AUTH_DEFAULT_GUEST_PREFIX,
                      sizeof(storage->guest_prefix));
    }

    if (storage->backend == MAILBOXD_STORAGE_SQLITE) {
        storage_dispatch_sqlite(storage, options->sql_cfg);
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        rc = mailboxd_storage_flatfile_open(storage);
        break;
    case MAILBOXD_STORAGE_SQLITE:
    case MAILBOXD_STORAGE_MYSQL:
    case MAILBOXD_STORAGE_MARIADB:
        rc = mailboxd_storage_sql_open(storage);
        break;
    default:
        rc = MAILBOXD_ERR_UNSUPPORTED;
        break;
    }

    if (rc != MAILBOXD_OK) {
        mailboxd_storage_close(storage);
        return NULL;
    }

    return storage;
}

void mailboxd_storage_close(mailboxd_storage_t *storage)
{
    if (storage == NULL) {
        return;
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        mailboxd_storage_flatfile_close(storage);
        break;
    case MAILBOXD_STORAGE_SQLITE:
    case MAILBOXD_STORAGE_MYSQL:
    case MAILBOXD_STORAGE_MARIADB:
        mailboxd_storage_sql_close(storage);
        break;
    default:
        break;
    }

    free(storage->path);
    free(storage);
}

mailboxd_storage_backend_kind_t mailboxd_storage_backend(const mailboxd_storage_t *storage)
{
    if (storage == NULL) {
        return MAILBOXD_STORAGE_FLATFILE;
    }
    return storage->backend;
}

const char *mailboxd_storage_root_path(const mailboxd_storage_t *storage)
{
    if (storage == NULL || storage->path == NULL || storage->path[0] == '\0') {
        return NULL;
    }
    return storage->path;
}

mailboxd_result_t mailboxd_storage_register_user(mailboxd_storage_t *storage,
                                           const mailboxd_user_registration_t *reg,
                                           mailboxd_user_record_t *out)
{
    if (storage == NULL || reg == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        return mailboxd_storage_flatfile_register_user(storage, reg, out);
    case MAILBOXD_STORAGE_SQLITE:
        return mailboxd_storage_sql_register_user(storage, reg, out);
    default:
        return MAILBOXD_ERR_UNSUPPORTED;
    }
}

mailboxd_result_t mailboxd_storage_find_user(mailboxd_storage_t *storage,
                                       const char *username,
                                       mailboxd_user_record_t *out)
{
    if (storage == NULL || username == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        return mailboxd_storage_flatfile_find_user(storage, username, out);
    case MAILBOXD_STORAGE_SQLITE:
        return mailboxd_storage_sql_find_user(storage, username, out);
    default:
        return MAILBOXD_ERR_UNSUPPORTED;
    }
}

mailboxd_result_t mailboxd_storage_resolve_user(mailboxd_storage_t *storage,
                                          const char *name,
                                          mailboxd_user_record_t *out)
{
    if (storage == NULL || name == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        return mailboxd_storage_flatfile_resolve_user(storage, name, out);
    case MAILBOXD_STORAGE_SQLITE:
        return mailboxd_storage_sql_resolve_user(storage, name, out);
    default:
        return MAILBOXD_ERR_UNSUPPORTED;
    }
}

mailboxd_result_t mailboxd_storage_count_level(mailboxd_storage_t *storage,
                                         mailboxd_user_level_t level,
                                         size_t *count)
{
    if (storage == NULL || count == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        return mailboxd_storage_flatfile_count_level(storage, level, count);
    case MAILBOXD_STORAGE_SQLITE:
        return mailboxd_storage_sql_count_level(storage, level, count);
    default:
        return MAILBOXD_ERR_UNSUPPORTED;
    }
}

mailboxd_result_t mailboxd_storage_foreach_user(mailboxd_storage_t *storage,
                                          mailboxd_storage_user_fn fn,
                                          void *ctx)
{
    if (storage == NULL || fn == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        return mailboxd_storage_flatfile_foreach_user(storage, fn, ctx);
    case MAILBOXD_STORAGE_SQLITE:
        return mailboxd_storage_sql_foreach_user(storage, fn, ctx);
    default:
        return MAILBOXD_ERR_UNSUPPORTED;
    }
}

mailboxd_result_t mailboxd_storage_session_begin(mailboxd_storage_t *storage,
                                           const mailboxd_user_record_t *user,
                                           const char *transport,
                                           mailboxd_session_record_t *out)
{
    if (storage == NULL || user == NULL || transport == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        return mailboxd_storage_flatfile_session_begin(storage, user, transport, out);
    case MAILBOXD_STORAGE_SQLITE:
        return mailboxd_storage_sql_session_begin(storage, user, transport, out);
    default:
        return MAILBOXD_ERR_UNSUPPORTED;
    }
}

mailboxd_result_t mailboxd_storage_session_end(mailboxd_storage_t *storage,
                                         uint64_t session_id)
{
    if (storage == NULL || session_id == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        return mailboxd_storage_flatfile_session_end(storage, session_id);
    case MAILBOXD_STORAGE_SQLITE:
        return mailboxd_storage_sql_session_end(storage, session_id);
    default:
        return MAILBOXD_ERR_UNSUPPORTED;
    }
}

mailboxd_result_t mailboxd_storage_update_user(mailboxd_storage_t *storage,
                                         const mailboxd_user_record_t *user)
{
    if (storage == NULL || user == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        return mailboxd_storage_flatfile_update_user(storage, user);
    case MAILBOXD_STORAGE_SQLITE:
        return mailboxd_storage_sql_update_user(storage, user);
    default:
        return MAILBOXD_ERR_UNSUPPORTED;
    }
}

mailboxd_result_t mailboxd_storage_delete_user(mailboxd_storage_t *storage,
                                         uint64_t user_id)
{
    if (storage == NULL || user_id == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    switch (storage->backend) {
    case MAILBOXD_STORAGE_FLATFILE:
        return mailboxd_storage_flatfile_delete_user(storage, user_id);
    case MAILBOXD_STORAGE_SQLITE:
        return mailboxd_storage_sql_delete_user(storage, user_id);
    default:
        return MAILBOXD_ERR_UNSUPPORTED;
    }
}
