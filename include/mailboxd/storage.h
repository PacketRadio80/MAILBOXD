/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_STORAGE_H
#define MAILBOXD_STORAGE_H

#include "mailboxd/auth.h"
#include "mailboxd/limits.h"
#include "mailboxd/types.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mailboxd_storage_backend_kind {
    MAILBOXD_STORAGE_FLATFILE = 1,
    MAILBOXD_STORAGE_SQLITE = 2,
    MAILBOXD_STORAGE_MYSQL = 3,
    MAILBOXD_STORAGE_MARIADB = 4
} mailboxd_storage_backend_kind_t;

#define MAILBOXD_USER_NAME_MAX 64
#define MAILBOXD_USER_NICKNAME_MAX 64
#define MAILBOXD_USER_FULL_NAME_MAX 128
#define MAILBOXD_USER_COUNTRY_MAX 64
#define MAILBOXD_USER_LOCATION_MAX 128
#define MAILBOXD_USER_EMAIL_MAX 128
#define MAILBOXD_USER_PASSWORD_MAX MAILBOXD_PASSWORD_STORED_MAX
#define MAILBOXD_TRANSPORT_NAME_MAX 32
/** Peer address string (IPv4 or bracketed IPv6). */
#define MAILBOXD_REMOTE_ADDR_MAX 64

/** Maximum registered user records per INI user-file shard. */
#define MAILBOXD_USERS_PER_FILE 50u

/** INI section prefix for one user: [user.<id>]. */
#define MAILBOXD_USER_INI_SECTION_PREFIX "user."

typedef struct mailboxd_user_registration {
    char username[MAILBOXD_USER_NAME_MAX];
    char nickname[MAILBOXD_USER_NICKNAME_MAX];
    char full_name[MAILBOXD_USER_FULL_NAME_MAX];
    char country[MAILBOXD_USER_COUNTRY_MAX];
    char location[MAILBOXD_USER_LOCATION_MAX];
    char email[MAILBOXD_USER_EMAIL_MAX];
    char password[MAILBOXD_USER_PASSWORD_MAX];
} mailboxd_user_registration_t;

/** Validate profile fields only (full name, country, location, email). */
int mailboxd_user_profile_valid(const mailboxd_user_registration_t *reg);

/** Validate registration profile fields (username + full name, country, location, email). */
int mailboxd_registration_valid(const mailboxd_user_registration_t *reg,
                             const char *guest_prefix);

typedef struct mailboxd_user_record {
    uint64_t id;
    char username[MAILBOXD_USER_NAME_MAX];
    char nickname[MAILBOXD_USER_NICKNAME_MAX];
    mailboxd_user_level_t level;
    int active;
    time_t created_at;
    char full_name[MAILBOXD_USER_FULL_NAME_MAX];
    char country[MAILBOXD_USER_COUNTRY_MAX];
    char location[MAILBOXD_USER_LOCATION_MAX];
    char email[MAILBOXD_USER_EMAIL_MAX];
    char password[MAILBOXD_USER_PASSWORD_MAX];
    time_t last_login_at;
} mailboxd_user_record_t;

typedef struct mailboxd_session_record {
    uint64_t session_id;
    uint64_t user_id;
    char username[MAILBOXD_USER_NAME_MAX];
    char transport[MAILBOXD_TRANSPORT_NAME_MAX];
    char remote[MAILBOXD_REMOTE_ADDR_MAX];
    time_t connected_at;
    time_t disconnected_at;
    int active;
} mailboxd_session_record_t;

typedef struct mailboxd_storage mailboxd_storage_t;

/** Paths and backup settings for SQLite (`[storage]`). */
typedef struct mailboxd_storage_sql_config {
    char user_db[MAILBOXD_PATH_MAX];
    char mail_db[MAILBOXD_PATH_MAX];
    unsigned backup_interval_sec;
    char backup_path[MAILBOXD_PATH_MAX];
} mailboxd_storage_sql_config_t;

typedef struct mailboxd_storage_options {
    mailboxd_storage_backend_kind_t backend;
    const char *path;
    const char *guest_prefix;
    const mailboxd_storage_sql_config_t *sql_cfg;
} mailboxd_storage_options_t;

struct mailboxd_config;

void mailboxd_storage_sql_config_defaults(mailboxd_storage_sql_config_t *cfg);

/** Resolve `user_db` / `mail_db` under @p storage_path from INI `[storage]`. */
void mailboxd_storage_sql_config_apply(mailboxd_storage_sql_config_t *cfg,
                                    const struct mailboxd_config *config,
                                    const char *storage_path);

const mailboxd_storage_sql_config_t *mailboxd_storage_sql_config(
    const mailboxd_storage_t *storage);

/** Copy SQLite DB files to fallback paths (no-op when not SQLite). */
void mailboxd_storage_backup_tick(mailboxd_storage_t *storage);

mailboxd_storage_t *mailboxd_storage_open(const mailboxd_storage_options_t *options);
void mailboxd_storage_close(mailboxd_storage_t *storage);

mailboxd_storage_backend_kind_t mailboxd_storage_backend(const mailboxd_storage_t *storage);

/** Root data directory path, or NULL when unset. */
const char *mailboxd_storage_root_path(const mailboxd_storage_t *storage);

/**
 * Register a new user account (level user, inactive until Sysop/Admin activates).
 */
mailboxd_result_t mailboxd_storage_register_user(mailboxd_storage_t *storage,
                                           const mailboxd_user_registration_t *reg,
                                           mailboxd_user_record_t *out);

/** Look up a user by login name (case-insensitive; stored lowercase). */
mailboxd_result_t mailboxd_storage_find_user(mailboxd_storage_t *storage,
                                       const char *username,
                                       mailboxd_user_record_t *out);

/** Resolve by login name or nickname (case-insensitive). */
mailboxd_result_t mailboxd_storage_resolve_user(mailboxd_storage_t *storage,
                                          const char *name,
                                          mailboxd_user_record_t *out);

/** Count existing accounts at a given level (e.g. enforce one Sysop). */
mailboxd_result_t mailboxd_storage_count_level(mailboxd_storage_t *storage,
                                         mailboxd_user_level_t level,
                                         size_t *count);

/**
 * Iterate all user records. @p fn returns MAILBOXD_OK to continue; any other
 * result stops iteration and is returned from this function.
 */
typedef mailboxd_result_t (*mailboxd_storage_user_fn)(const mailboxd_user_record_t *user,
                                                void *ctx);

mailboxd_result_t mailboxd_storage_foreach_user(mailboxd_storage_t *storage,
                                          mailboxd_storage_user_fn fn,
                                          void *ctx);

mailboxd_result_t mailboxd_storage_session_begin(mailboxd_storage_t *storage,
                                           const mailboxd_user_record_t *user,
                                           const char *transport,
                                           mailboxd_session_record_t *out);

mailboxd_result_t mailboxd_storage_session_end(mailboxd_storage_t *storage,
                                         uint64_t session_id);

/**
 * Replace an existing user record (matched by @p user->id).
 */
mailboxd_result_t mailboxd_storage_update_user(mailboxd_storage_t *storage,
                                         const mailboxd_user_record_t *user);

/** Remove a user record from storage (matched by @p user_id). */
mailboxd_result_t mailboxd_storage_delete_user(mailboxd_storage_t *storage,
                                         uint64_t user_id);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_STORAGE_H */
