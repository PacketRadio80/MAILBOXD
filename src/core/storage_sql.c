/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/storage.h"
#include "mailboxd/auth.h"
#include "mailboxd/config.h"
#include "mailboxd/password.h"
#include "mailboxd/security.h"
#include "mailboxd/util.h"
#include "mailboxd/log.h"
#include "storage_private.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef MAILBOXD_HAVE_SQLITE
#include <sqlite3.h>
#endif

#define MAILBOXD_SQL_DEFAULT_USER_DB "users.db"
#define MAILBOXD_SQL_DEFAULT_MAIL_DB "mail.db"

#ifndef MAILBOXD_HAVE_SQLITE

mailboxd_result_t mailboxd_storage_sql_open(mailboxd_storage_t *storage)
{
    (void)storage;
    mailboxd_log_warn("[storage] SQLite backend requested but mailboxd was built without "
                   "libsqlite3 — use backend=flatfile or rebuild with sqlite3");
    return MAILBOXD_ERR_UNSUPPORTED;
}

void mailboxd_storage_sql_close(mailboxd_storage_t *storage)
{
    (void)storage;
}

mailboxd_result_t mailboxd_storage_sql_register_user(mailboxd_storage_t *storage,
                                               const mailboxd_user_registration_t *reg,
                                               mailboxd_user_record_t *out)
{
    (void)storage;
    (void)reg;
    (void)out;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_storage_sql_find_user(mailboxd_storage_t *storage,
                                           const char *username,
                                           mailboxd_user_record_t *out)
{
    (void)storage;
    (void)username;
    (void)out;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_storage_sql_resolve_user(mailboxd_storage_t *storage,
                                              const char *name,
                                              mailboxd_user_record_t *out)
{
    (void)storage;
    (void)name;
    (void)out;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_storage_sql_count_level(mailboxd_storage_t *storage,
                                             mailboxd_user_level_t level,
                                             size_t *count)
{
    (void)storage;
    (void)level;
    (void)count;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_storage_sql_foreach_user(mailboxd_storage_t *storage,
                                            mailboxd_storage_user_fn fn,
                                            void *ctx)
{
    (void)storage;
    (void)fn;
    (void)ctx;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_storage_sql_update_user(mailboxd_storage_t *storage,
                                             const mailboxd_user_record_t *user)
{
    (void)storage;
    (void)user;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_storage_sql_delete_user(mailboxd_storage_t *storage,
                                             uint64_t user_id)
{
    (void)storage;
    (void)user_id;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_storage_sql_session_begin(mailboxd_storage_t *storage,
                                               const mailboxd_user_record_t *user,
                                               const char *transport,
                                               mailboxd_session_record_t *out)
{
    (void)storage;
    (void)user;
    (void)transport;
    (void)out;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_storage_sql_session_end(mailboxd_storage_t *storage,
                                             uint64_t session_id)
{
    (void)storage;
    (void)session_id;
    return MAILBOXD_ERR_UNSUPPORTED;
}

void mailboxd_storage_sql_backup_files(const mailboxd_storage_t *storage)
{
    (void)storage;
}

void mailboxd_storage_sql_backup_tick(mailboxd_storage_t *storage)
{
    (void)storage;
}

#else /* MAILBOXD_HAVE_SQLITE */

struct sql_state {
    sqlite3 *users_db;
    sqlite3 *mail_db;
    unsigned backup_tick;
};

static int sql_str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }

    while (*a != '\0' && *b != '\0') {
        char ca = (char)(*a >= 'A' && *a <= 'Z' ? *a + 32 : *a);
        char cb = (char)(*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);

        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static int mkdir_p(const char *path)
{
    char buf[MAILBOXD_PATH_MAX];
    size_t len;
    size_t i;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    len = strlen(path);
    if (len >= sizeof(buf)) {
        return -1;
    }

    memcpy(buf, path, len + 1);

    for (i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            buf[i] = '/';
        }
    }

    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static mailboxd_result_t sql_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc;

    if (db == NULL || sql == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        mailboxd_log_warn("[storage] sqlite: %s",
                err != NULL ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return MAILBOXD_ERR_IO;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t sql_meta_get(sqlite3 *db, const char *key, uint64_t *value)
{
    sqlite3_stmt *stmt;
    int rc;

    *value = 0;

    rc = sqlite3_prepare_v2(db,
                            "SELECT value FROM meta WHERE key = ?1;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *value = (uint64_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return MAILBOXD_OK;
}

static mailboxd_result_t sql_meta_set(sqlite3 *db, const char *key, uint64_t value)
{
    sqlite3_stmt *stmt;
    int rc;

    rc = sqlite3_prepare_v2(db,
                            "INSERT INTO meta(key,value) VALUES(?1,?2) "
                            "ON CONFLICT(key) DO UPDATE SET value=?2;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)value);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? MAILBOXD_OK : MAILBOXD_ERR_IO;
}

static mailboxd_result_t sql_meta_bump(sqlite3 *db, const char *key, uint64_t *value)
{
    mailboxd_result_t rc;

    rc = sql_meta_get(db, key, value);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    (*value)++;
    return sql_meta_set(db, key, *value);
}

static mailboxd_result_t sql_open_db(const char *path, sqlite3 **out_db)
{
    int rc;
    int existed = (access(path, F_OK) == 0);

    if (out_db == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    *out_db = NULL;
    rc = sqlite3_open(path, out_db);
    if (rc != SQLITE_OK) {
        mailboxd_log_warn("[storage] cannot open '%s': %s",
                path, sqlite3_errmsg(*out_db));
        if (*out_db != NULL) {
            sqlite3_close(*out_db);
            *out_db = NULL;
        }
        return MAILBOXD_ERR_IO;
    }

    sqlite3_busy_timeout(*out_db, 5000);
    if (!existed) {
        mailboxd_log_info("[storage] created new database %s", path);
    } else {
        mailboxd_log_info("[storage] opened existing database %s", path);
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t sql_init_users_schema(sqlite3 *db)
{
    static const char *schema =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY,"
        "  username TEXT NOT NULL UNIQUE,"
        "  nickname TEXT,"
        "  level INTEGER NOT NULL,"
        "  active INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  full_name TEXT,"
        "  country TEXT,"
        "  location TEXT,"
        "  email TEXT,"
        "  password TEXT NOT NULL,"
        "  last_login_at INTEGER"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_users_nickname ON users(nickname);"
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  session_id INTEGER PRIMARY KEY,"
        "  user_id INTEGER NOT NULL,"
        "  username TEXT,"
        "  transport TEXT,"
        "  remote TEXT,"
        "  connected_at INTEGER,"
        "  disconnected_at INTEGER,"
        "  active INTEGER"
        ");"
        "CREATE TABLE IF NOT EXISTS meta ("
        "  key TEXT PRIMARY KEY,"
        "  value INTEGER NOT NULL"
        ");";

    return sql_exec(db, schema);
}

static mailboxd_result_t sql_init_mail_schema(sqlite3 *db)
{
    static const char *schema =
        "CREATE TABLE IF NOT EXISTS messages ("
        "  id INTEGER PRIMARY KEY,"
        "  owner TEXT NOT NULL,"
        "  from_user TEXT NOT NULL,"
        "  subject TEXT,"
        "  body TEXT,"
        "  received_at INTEGER NOT NULL,"
        "  read_flag INTEGER NOT NULL DEFAULT 0,"
        "  deleted_at INTEGER,"
        "  folder INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_mail_owner ON messages(owner, folder, received_at DESC);"
        "CREATE TABLE IF NOT EXISTS meta ("
        "  key TEXT PRIMARY KEY,"
        "  value INTEGER NOT NULL"
        ");";

    return sql_exec(db, schema);
}

static void sql_row_to_user(sqlite3_stmt *stmt, mailboxd_user_record_t *user)
{
    memset(user, 0, sizeof(*user));
    user->id = (uint64_t)sqlite3_column_int64(stmt, 0);
    mailboxd_strlcpy(user->username, (const char *)sqlite3_column_text(stmt, 1),
                  sizeof(user->username));
    if (sqlite3_column_text(stmt, 2) != NULL) {
        mailboxd_strlcpy(user->nickname, (const char *)sqlite3_column_text(stmt, 2),
                      sizeof(user->nickname));
    }
    user->level = (mailboxd_user_level_t)sqlite3_column_int(stmt, 3);
    user->active = sqlite3_column_int(stmt, 4);
    user->created_at = (time_t)sqlite3_column_int64(stmt, 5);
    if (sqlite3_column_text(stmt, 6) != NULL) {
        mailboxd_strlcpy(user->full_name, (const char *)sqlite3_column_text(stmt, 6),
                      sizeof(user->full_name));
    }
    if (sqlite3_column_text(stmt, 7) != NULL) {
        mailboxd_strlcpy(user->country, (const char *)sqlite3_column_text(stmt, 7),
                      sizeof(user->country));
    }
    if (sqlite3_column_text(stmt, 8) != NULL) {
        mailboxd_strlcpy(user->location, (const char *)sqlite3_column_text(stmt, 8),
                      sizeof(user->location));
    }
    if (sqlite3_column_text(stmt, 9) != NULL) {
        mailboxd_strlcpy(user->email, (const char *)sqlite3_column_text(stmt, 9),
                      sizeof(user->email));
    }
    if (sqlite3_column_text(stmt, 10) != NULL) {
        mailboxd_strlcpy(user->password, (const char *)sqlite3_column_text(stmt, 10),
                      sizeof(user->password));
    }
    user->last_login_at = (time_t)sqlite3_column_int64(stmt, 11);
}

static mailboxd_result_t sql_ensure_default_sysop(mailboxd_storage_t *storage)
{
    struct sql_state *state = storage->backend_data;
    size_t sysop_count = 0;
    mailboxd_user_record_t sysop;
    char plain_password[MAILBOXD_PASSWORD_MAX_LEN + 1];
    uint64_t user_id;
    sqlite3_stmt *stmt;
    int rc;
    mailboxd_result_t hres;

    hres = mailboxd_storage_sql_count_level(storage, MAILBOXD_LEVEL_SYSOP, &sysop_count);
    if (hres != MAILBOXD_OK) {
        return hres;
    }
    if (sysop_count > 0) {
        return MAILBOXD_OK;
    }

    hres = sql_meta_bump(state->users_db, "user_next", &user_id);
    if (hres != MAILBOXD_OK) {
        return hres;
    }

    memset(&sysop, 0, sizeof(sysop));
    sysop.id = user_id;
    mailboxd_strlcpy(sysop.username, "sysop", sizeof(sysop.username));
    mailboxd_strlcpy(sysop.nickname, MAILBOXD_DEFAULT_SYSOP_USERNAME,
                  sizeof(sysop.nickname));
    sysop.level = MAILBOXD_LEVEL_SYSOP;
    sysop.active = 1;
    sysop.created_at = time(NULL);
    mailboxd_strlcpy(sysop.full_name, "System Operator", sizeof(sysop.full_name));

    /* Fixed initial Sysop password, matching PRTERM's built-in admin
     * default ("PRTerm") — change it on first login via /passwd. */
    mailboxd_strlcpy(plain_password, MAILBOXD_SYSOP_INIT_PASSWORD,
                  sizeof(plain_password));
    if (mailboxd_password_hash(plain_password, sysop.password,
                            sizeof(sysop.password)) != MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    rc = sqlite3_prepare_v2(state->users_db,
                            "INSERT INTO users(id,username,nickname,level,active,"
                            "created_at,full_name,country,location,email,password,"
                            "last_login_at) VALUES(?1,?2,?3,?4,?5,?6,?7,'','','',?8,0);",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)sysop.id);
    sqlite3_bind_text(stmt, 2, sysop.username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, sysop.nickname, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, (int)sysop.level);
    sqlite3_bind_int(stmt, 5, sysop.active);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)sysop.created_at);
    sqlite3_bind_text(stmt, 7, sysop.full_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, sysop.password, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return MAILBOXD_ERR_IO;
    }

    mailboxd_log_info("[storage] created default Sysop in %s (login: %s / %s)",
           storage->sql_cfg.user_db, sysop.nickname, plain_password);
    mailboxd_security_log_write("sysop_created user=%s", sysop.nickname);
    return MAILBOXD_OK;
}

static mailboxd_result_t sql_copy_file(const char *src, const char *dst)
{
    FILE *in_fp;
    FILE *out_fp;
    unsigned char buf[4096];
    size_t n;

    in_fp = fopen(src, "rb");
    if (in_fp == NULL) {
        return MAILBOXD_ERR_IO;
    }

    out_fp = fopen(dst, "wb");
    if (out_fp == NULL) {
        fclose(in_fp);
        return MAILBOXD_ERR_IO;
    }

    while ((n = fread(buf, 1, sizeof(buf), in_fp)) > 0) {
        if (fwrite(buf, 1, n, out_fp) != n) {
            fclose(in_fp);
            fclose(out_fp);
            return MAILBOXD_ERR_IO;
        }
    }

    fclose(in_fp);
    if (fclose(out_fp) != 0) {
        return MAILBOXD_ERR_IO;
    }

    return MAILBOXD_OK;
}

static void sql_backup_one(sqlite3 *db, const char *src_path,
                           const char *backup_dir)
{
    char dst[MAILBOXD_PATH_MAX];
    const char *base;
    mailboxd_result_t rc;

    if (db == NULL || src_path == NULL || src_path[0] == '\0') {
        return;
    }

    (void)sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);

    base = strrchr(src_path, '/');
    base = (base != NULL) ? base + 1 : src_path;

    if (backup_dir != NULL && backup_dir[0] != '\0') {
        char name[MAILBOXD_PATH_MAX];

        if (mkdir_p(backup_dir) != 0) {
            return;
        }
        if (strlen(base) + strlen(MAILBOXD_STORAGE_BACKUP_SUFFIX) + 1 >=
            sizeof(name)) {
            return;
        }
        snprintf(name, sizeof(name), "%s%s", base, MAILBOXD_STORAGE_BACKUP_SUFFIX);
        if (mailboxd_path_join(dst, sizeof(dst), backup_dir, name) != MAILBOXD_OK) {
            return;
        }
    } else {
        if (strlen(src_path) + strlen(MAILBOXD_STORAGE_BACKUP_SUFFIX) + 1 >=
            sizeof(dst)) {
            return;
        }
        snprintf(dst, sizeof(dst), "%s%s", src_path, MAILBOXD_STORAGE_BACKUP_SUFFIX);
    }

    rc = sql_copy_file(src_path, dst);
    if (rc != MAILBOXD_OK) {
        mailboxd_log_warn("[storage] backup failed %s -> %s", src_path, dst);
    }
}

mailboxd_result_t mailboxd_storage_sql_open(mailboxd_storage_t *storage)
{
    struct sql_state *state;
    mailboxd_result_t rc;

    if (storage == NULL || storage->path == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (storage->backend == MAILBOXD_STORAGE_MYSQL ||
        storage->backend == MAILBOXD_STORAGE_MARIADB) {
        mailboxd_log_warn("[storage] MySQL/MariaDB backends are not supported — "
                       "use flatfile or sqlite");
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    if (mkdir_p(storage->path) != 0) {
        mailboxd_log_warn("[storage] cannot create data path '%s'",
                storage->path);
        return MAILBOXD_ERR_IO;
    }

    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return MAILBOXD_ERR_NOMEM;
    }

    rc = sql_open_db(storage->sql_cfg.user_db, &state->users_db);
    if (rc != MAILBOXD_OK) {
        free(state);
        return rc;
    }

    rc = sql_init_users_schema(state->users_db);
    if (rc != MAILBOXD_OK) {
        mailboxd_storage_sql_close(storage);
        free(state);
        return rc;
    }

    rc = sql_open_db(storage->sql_cfg.mail_db, &state->mail_db);
    if (rc != MAILBOXD_OK) {
        sqlite3_close(state->users_db);
        free(state);
        return rc;
    }

    rc = sql_init_mail_schema(state->mail_db);
    if (rc != MAILBOXD_OK) {
        sqlite3_close(state->users_db);
        sqlite3_close(state->mail_db);
        free(state);
        return rc;
    }

    storage->backend_data = state;

    rc = sql_ensure_default_sysop(storage);
    if (rc != MAILBOXD_OK) {
        mailboxd_storage_sql_close(storage);
        return rc;
    }

    mailboxd_log_info("[storage] sqlite user_db=%s mail_db=%s backup_interval=%us",
           storage->sql_cfg.user_db, storage->sql_cfg.mail_db,
           storage->sql_cfg.backup_interval_sec);

    return MAILBOXD_OK;
}

void mailboxd_storage_sql_close(mailboxd_storage_t *storage)
{
    struct sql_state *state;

    if (storage == NULL) {
        return;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return;
    }

    if (state->users_db != NULL) {
        sqlite3_close(state->users_db);
    }
    if (state->mail_db != NULL) {
        sqlite3_close(state->mail_db);
    }

    free(state);
    storage->backend_data = NULL;
}

sqlite3 *mailboxd_storage_sql_mail_db(mailboxd_storage_t *storage)
{
    struct sql_state *state;

    if (storage == NULL) {
        return NULL;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return NULL;
    }

    return state->mail_db;
}

static mailboxd_result_t sql_find_user_by_column(sqlite3 *db, const char *column,
                                              const char *name,
                                              mailboxd_user_record_t *out)
{
    sqlite3_stmt *stmt;
    int rc;
    const char *sql_username =
        "SELECT id,username,nickname,level,active,created_at,full_name,"
        "country,location,email,password,last_login_at "
        "FROM users WHERE lower(username)=lower(?1) LIMIT 1;";
    const char *sql_nickname =
        "SELECT id,username,nickname,level,active,created_at,full_name,"
        "country,location,email,password,last_login_at "
        "FROM users WHERE lower(nickname)=lower(?1) LIMIT 1;";
    const char *sql = sql_str_ieq(column, "nickname") ? sql_nickname :
                                                        sql_username;

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        sql_row_to_user(stmt, out);
        sqlite3_finalize(stmt);
        return MAILBOXD_OK;
    }

    sqlite3_finalize(stmt);
    return MAILBOXD_ERR_NOT_FOUND;
}

mailboxd_result_t mailboxd_storage_sql_find_user(mailboxd_storage_t *storage,
                                          const char *username,
                                          mailboxd_user_record_t *out)
{
    struct sql_state *state;
    char normalized[MAILBOXD_USER_NAME_MAX];

    if (storage == NULL || username == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL || state->users_db == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    mailboxd_strlcpy(normalized, username, sizeof(normalized));
    mailboxd_username_normalize(normalized);
    return sql_find_user_by_column(state->users_db, "username", normalized, out);
}

mailboxd_result_t mailboxd_storage_sql_resolve_user(mailboxd_storage_t *storage,
                                            const char *name,
                                            mailboxd_user_record_t *out)
{
    mailboxd_result_t rc;

    if (storage == NULL || name == NULL || out == NULL || name[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_sql_find_user(storage, name, out);
    if (rc == MAILBOXD_OK) {
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_ERR_NOT_FOUND) {
        return rc;
    }

    return sql_find_user_by_column(
        ((struct sql_state *)storage->backend_data)->users_db,
        "nickname", name, out);
}

typedef struct foreach_ctx {
    mailboxd_storage_user_fn fn;
    void *user_ctx;
} foreach_ctx_t;

static int sql_foreach_cb(void *ctx, int n_cols, char **values, char **cols)
{
    mailboxd_user_record_t user;
    foreach_ctx_t *fctx = (foreach_ctx_t *)ctx;
    mailboxd_result_t rc;
    int i;

    (void)n_cols;
    (void)cols;
    memset(&user, 0, sizeof(user));

    for (i = 0; values[i] != NULL; i++) {
        switch (i) {
        case 0: user.id = (uint64_t)strtoull(values[i], NULL, 10); break;
        case 1: mailboxd_strlcpy(user.username, values[i], sizeof(user.username)); break;
        case 2: mailboxd_strlcpy(user.nickname, values[i], sizeof(user.nickname)); break;
        case 3: user.level = (mailboxd_user_level_t)atoi(values[i]); break;
        case 4: user.active = atoi(values[i]); break;
        case 5: user.created_at = (time_t)strtoll(values[i], NULL, 10); break;
        case 6: mailboxd_strlcpy(user.full_name, values[i], sizeof(user.full_name)); break;
        case 7: mailboxd_strlcpy(user.country, values[i], sizeof(user.country)); break;
        case 8: mailboxd_strlcpy(user.location, values[i], sizeof(user.location)); break;
        case 9: mailboxd_strlcpy(user.email, values[i], sizeof(user.email)); break;
        case 10: mailboxd_strlcpy(user.password, values[i], sizeof(user.password)); break;
        case 11: user.last_login_at = (time_t)strtoll(values[i], NULL, 10); break;
        default: break;
        }
    }

    rc = fctx->fn(&user, fctx->user_ctx);
    return (rc == MAILBOXD_OK) ? 0 : 1;
}

mailboxd_result_t mailboxd_storage_sql_foreach_user(mailboxd_storage_t *storage,
                                              mailboxd_storage_user_fn fn,
                                              void *ctx)
{
    struct sql_state *state;
    foreach_ctx_t fctx;
    char *err = NULL;
    int rc;

    if (storage == NULL || fn == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL || state->users_db == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    fctx.fn = fn;
    fctx.user_ctx = ctx;

    rc = sqlite3_exec(state->users_db,
                      "SELECT id,username,nickname,level,active,created_at,"
                      "full_name,country,location,email,password,last_login_at "
                      "FROM users ORDER BY id;",
                      sql_foreach_cb, &fctx, &err);
    if (rc == SQLITE_ABORT) {
        sqlite3_free(err);
        return MAILBOXD_ERR_BUSY;
    }
    if (rc != SQLITE_OK) {
        mailboxd_log_warn("[storage] foreach: %s", err != NULL ? err : "error");
        sqlite3_free(err);
        return MAILBOXD_ERR_IO;
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_storage_sql_count_level(mailboxd_storage_t *storage,
                                             mailboxd_user_level_t level,
                                             size_t *count)
{
    struct sql_state *state;
    sqlite3_stmt *stmt;
    int rc;

    if (storage == NULL || count == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    *count = 0;
    state = storage->backend_data;
    if (state == NULL || state->users_db == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = sqlite3_prepare_v2(state->users_db,
                            "SELECT COUNT(*) FROM users WHERE level=?1;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_int(stmt, 1, (int)level);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *count = (size_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return MAILBOXD_OK;
}

typedef struct identity_taken_ctx {
    const char *username;
    const char *nickname;
    int taken;
} identity_taken_ctx_t;

static mailboxd_result_t identity_taken_cb(const mailboxd_user_record_t *user, void *ctx)
{
    identity_taken_ctx_t *ictx = (identity_taken_ctx_t *)ctx;

    if (sql_str_ieq(user->username, ictx->username) ||
        sql_str_ieq(user->username, ictx->nickname) ||
        (user->nickname[0] != '\0' &&
         (sql_str_ieq(user->nickname, ictx->nickname) ||
          sql_str_ieq(user->nickname, ictx->username)))) {
        ictx->taken = 1;
        return MAILBOXD_ERR_BUSY;
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_storage_sql_register_user(mailboxd_storage_t *storage,
                                               const mailboxd_user_registration_t *reg,
                                               mailboxd_user_record_t *out)
{
    struct sql_state *state;
    mailboxd_user_record_t existing;
    identity_taken_ctx_t taken;
    uint64_t user_id;
    sqlite3_stmt *stmt;
    int rc;
    mailboxd_result_t hres;

    if (storage == NULL || reg == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (!mailboxd_registration_valid(reg, storage->guest_prefix)) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL || state->users_db == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    hres = mailboxd_storage_sql_find_user(storage, reg->username, &existing);
    if (hres == MAILBOXD_OK) {
        return MAILBOXD_ERR_BUSY;
    }
    if (hres != MAILBOXD_ERR_NOT_FOUND) {
        return hres;
    }

    memset(&taken, 0, sizeof(taken));
    taken.username = reg->username;
    taken.nickname = reg->nickname;
    hres = mailboxd_storage_sql_foreach_user(storage, identity_taken_cb, &taken);
    if (hres == MAILBOXD_ERR_BUSY || taken.taken) {
        return MAILBOXD_ERR_BUSY;
    }
    if (hres != MAILBOXD_OK) {
        return hres;
    }

    hres = sql_meta_bump(state->users_db, "user_next", &user_id);
    if (hres != MAILBOXD_OK) {
        return hres;
    }

    memset(out, 0, sizeof(*out));
    out->id = user_id;
    mailboxd_strlcpy(out->username, reg->username, sizeof(out->username));
    mailboxd_username_normalize(out->username);
    mailboxd_strlcpy(out->nickname, reg->nickname, sizeof(out->nickname));
    out->level = MAILBOXD_LEVEL_USER;
    out->active = 0;
    out->created_at = time(NULL);
    mailboxd_strlcpy(out->full_name, reg->full_name, sizeof(out->full_name));
    mailboxd_strlcpy(out->country, reg->country, sizeof(out->country));
    mailboxd_strlcpy(out->location, reg->location, sizeof(out->location));
    mailboxd_strlcpy(out->email, reg->email, sizeof(out->email));

    if (reg->password[0] != '\0') {
        if (!mailboxd_password_plain_valid(reg->password)) {
            return MAILBOXD_ERR_INVALID;
        }
        hres = mailboxd_password_hash(reg->password, out->password,
                                   sizeof(out->password));
        if (hres != MAILBOXD_OK) {
            return hres;
        }
    }

    rc = sqlite3_prepare_v2(state->users_db,
                            "INSERT INTO users(id,username,nickname,level,active,"
                            "created_at,full_name,country,location,email,password,"
                            "last_login_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,0);",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)out->id);
    sqlite3_bind_text(stmt, 2, out->username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, out->nickname, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, (int)out->level);
    sqlite3_bind_int(stmt, 5, out->active);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)out->created_at);
    sqlite3_bind_text(stmt, 7, out->full_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, out->country, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, out->location, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, out->email, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, out->password, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? MAILBOXD_OK : MAILBOXD_ERR_IO;
}

mailboxd_result_t mailboxd_storage_sql_update_user(mailboxd_storage_t *storage,
                                             const mailboxd_user_record_t *user)
{
    struct sql_state *state;
    sqlite3_stmt *stmt;
    int rc;

    if (storage == NULL || user == NULL || user->id == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL || state->users_db == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = sqlite3_prepare_v2(state->users_db,
                            "UPDATE users SET username=?2,nickname=?3,level=?4,"
                            "active=?5,created_at=?6,full_name=?7,country=?8,"
                            "location=?9,email=?10,password=?11,last_login_at=?12 "
                            "WHERE id=?1;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user->id);
    sqlite3_bind_text(stmt, 2, user->username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, user->nickname, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, (int)user->level);
    sqlite3_bind_int(stmt, 5, user->active);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)user->created_at);
    sqlite3_bind_text(stmt, 7, user->full_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, user->country, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, user->location, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, user->email, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, user->password, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 12, (sqlite3_int64)user->last_login_at);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return MAILBOXD_ERR_IO;
    }
    if (sqlite3_changes(state->users_db) == 0) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_storage_sql_delete_user(mailboxd_storage_t *storage,
                                             uint64_t user_id)
{
    struct sql_state *state;
    sqlite3_stmt *stmt;
    int rc;

    if (storage == NULL || user_id == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL || state->users_db == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = sqlite3_prepare_v2(state->users_db,
                            "DELETE FROM users WHERE id=?1;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return MAILBOXD_ERR_IO;
    }
    if (sqlite3_changes(state->users_db) == 0) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_storage_sql_session_begin(mailboxd_storage_t *storage,
                                               const mailboxd_user_record_t *user,
                                               const char *transport,
                                               mailboxd_session_record_t *out)
{
    struct sql_state *state;
    sqlite3_stmt *stmt;
    int rc;
    mailboxd_result_t hres;

    if (storage == NULL || user == NULL || transport == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL || state->users_db == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    hres = sql_meta_bump(state->users_db, "session_next", &out->session_id);
    if (hres != MAILBOXD_OK) {
        return hres;
    }

    out->user_id = user->id;
    mailboxd_strlcpy(out->username, user->username, sizeof(out->username));
    mailboxd_strlcpy(out->transport, transport, sizeof(out->transport));
    out->connected_at = time(NULL);
    out->active = 1;

    rc = sqlite3_prepare_v2(state->users_db,
                            "INSERT INTO sessions(session_id,user_id,username,"
                            "transport,remote,connected_at,disconnected_at,active) "
                            "VALUES(?1,?2,?3,?4,'',?5,0,1);",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)out->session_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)out->user_id);
    sqlite3_bind_text(stmt, 3, out->username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, out->transport, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)out->connected_at);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? MAILBOXD_OK : MAILBOXD_ERR_IO;
}

mailboxd_result_t mailboxd_storage_sql_session_end(mailboxd_storage_t *storage,
                                             uint64_t session_id)
{
    struct sql_state *state;
    sqlite3_stmt *stmt;
    int rc;
    time_t now = time(NULL);

    if (storage == NULL || session_id == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL || state->users_db == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = sqlite3_prepare_v2(state->users_db,
                            "UPDATE sessions SET disconnected_at=?2, active=0 "
                            "WHERE session_id=?1;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)session_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)now);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return MAILBOXD_ERR_IO;
    }

    if (sqlite3_changes(state->users_db) == 0) {
        rc = sqlite3_prepare_v2(state->users_db,
                                "INSERT INTO sessions(session_id,user_id,username,"
                                "transport,remote,connected_at,disconnected_at,"
                                "active) VALUES(?1,0,'','','',0,?2,0);",
                                -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            return MAILBOXD_ERR_IO;
        }
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)session_id);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)now);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            return MAILBOXD_ERR_IO;
        }
    }

    return MAILBOXD_OK;
}

void mailboxd_storage_sql_backup_files(const mailboxd_storage_t *storage)
{
    struct sql_state *state;
    const char *backup_dir;

    if (storage == NULL || storage->backend != MAILBOXD_STORAGE_SQLITE) {
        return;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return;
    }

    backup_dir = storage->sql_cfg.backup_path;
    if (backup_dir != NULL && backup_dir[0] == '\0') {
        backup_dir = NULL;
    }

    sql_backup_one(state->users_db, storage->sql_cfg.user_db, backup_dir);
    sql_backup_one(state->mail_db, storage->sql_cfg.mail_db, backup_dir);
}

void mailboxd_storage_sql_backup_tick(mailboxd_storage_t *storage)
{
    struct sql_state *state;

    if (storage == NULL || storage->backend != MAILBOXD_STORAGE_SQLITE) {
        return;
    }

    state = storage->backend_data;
    if (state == NULL || storage->sql_cfg.backup_interval_sec == 0) {
        return;
    }

    state->backup_tick++;
    if (state->backup_tick >= storage->sql_cfg.backup_interval_sec) {
        state->backup_tick = 0;
        mailboxd_storage_sql_backup_files(storage);
    }
}

#endif /* MAILBOXD_HAVE_SQLITE */
