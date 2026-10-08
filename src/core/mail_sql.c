/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mail_sql.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/traffic.h"
#include "mailboxd/util.h"
#include "storage_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef MAILBOXD_HAVE_SQLITE
#include <sqlite3.h>

#define MAIL_FOLDER_INBOX 0
#define MAIL_FOLDER_RECYCLE 1

static int mail_entry_compare(const void *a, const void *b)
{
    const mailboxd_mail_entry_t *ea = (const mailboxd_mail_entry_t *)a;
    const mailboxd_mail_entry_t *eb = (const mailboxd_mail_entry_t *)b;

    if (ea->received_at > eb->received_at) {
        return -1;
    }
    if (ea->received_at < eb->received_at) {
        return 1;
    }
    if (ea->id > eb->id) {
        return -1;
    }
    if (ea->id < eb->id) {
        return 1;
    }
    return 0;
}

static mailboxd_result_t mail_sql_meta_bump(sqlite3 *db, const char *key,
                                         uint64_t *value)
{
    sqlite3_stmt *stmt;
    int rc;

    *value = 0;

    rc = sqlite3_prepare_v2(db,
                            "SELECT value FROM meta WHERE key=?1;",
                            -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            *value = (uint64_t)sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    (*value)++;

    rc = sqlite3_prepare_v2(db,
                            "INSERT INTO meta(key,value) VALUES(?1,?2) "
                            "ON CONFLICT(key) DO UPDATE SET value=?2;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)*value);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? MAILBOXD_OK : MAILBOXD_ERR_IO;
}

static void mail_sql_owner_norm(char *owner, size_t len, const char *username)
{
    mailboxd_strlcpy(owner, username, len);
    mailboxd_username_normalize(owner);
}

mailboxd_result_t mailboxd_mail_sql_load_inbox(mailboxd_storage_t *storage,
                                         const char *username,
                                         mailboxd_mail_entry_t *entries,
                                         size_t max_entries,
                                         size_t *out_count)
{
    sqlite3 *db;
    sqlite3_stmt *stmt;
    char owner[MAILBOXD_USER_NAME_MAX];
    int rc;
    size_t count = 0;

    if (storage == NULL || username == NULL || entries == NULL ||
        out_count == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    db = mailboxd_storage_sql_mail_db(storage);
    if (db == NULL) {
        return MAILBOXD_ERR_IO;
    }

    mail_sql_owner_norm(owner, sizeof(owner), username);
    *out_count = 0;

    rc = sqlite3_prepare_v2(db,
                            "SELECT id,from_user,subject,received_at,read_flag "
                            "FROM messages WHERE owner=?1 AND folder=?2 "
                            "ORDER BY received_at DESC, id DESC;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, MAIL_FOLDER_INBOX);

    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_entries) {
        mailboxd_mail_entry_t *entry = &entries[count];

        memset(entry, 0, sizeof(*entry));
        entry->id = (uint64_t)sqlite3_column_int64(stmt, 0);
        mailboxd_strlcpy(entry->from, (const char *)sqlite3_column_text(stmt, 1),
                      sizeof(entry->from));
        mailboxd_strlcpy(entry->subject,
                      (const char *)sqlite3_column_text(stmt, 2),
                      sizeof(entry->subject));
        entry->received_at = (time_t)sqlite3_column_int64(stmt, 3);
        entry->read = sqlite3_column_int(stmt, 4);
        count++;
    }

    sqlite3_finalize(stmt);

    if (count > 1) {
        qsort(entries, count, sizeof(entries[0]), mail_entry_compare);
    }

    *out_count = count;
    return MAILBOXD_OK;
}

static mailboxd_result_t mail_sql_trim_inbox(sqlite3 *db, const char *owner,
                                          unsigned max_messages)
{
    sqlite3_stmt *stmt;
    int rc;
    size_t count = 0;

    rc = sqlite3_prepare_v2(db,
                            "SELECT COUNT(*) FROM messages "
                            "WHERE owner=?1 AND folder=?2;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, MAIL_FOLDER_INBOX);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = (size_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (count <= max_messages) {
        return MAILBOXD_OK;
    }

    rc = sqlite3_prepare_v2(db,
                            "DELETE FROM messages WHERE id IN ("
                            "SELECT id FROM messages WHERE owner=?1 "
                            "AND folder=?2 ORDER BY received_at ASC, id ASC "
                            "LIMIT ?3);",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, MAIL_FOLDER_INBOX);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)(count - max_messages));
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? MAILBOXD_OK : MAILBOXD_ERR_IO;
}

mailboxd_result_t mailboxd_mail_sql_deliver(mailboxd_storage_t *storage,
                                      const mailboxd_mail_config_t *mail,
                                      const char *from_user,
                                      const char *to_user,
                                      const char *subject,
                                      const char *body)
{
    sqlite3 *db;
    sqlite3_stmt *stmt;
    char owner[MAILBOXD_USER_NAME_MAX];
    uint64_t id;
    int rc;
    mailboxd_result_t hres;
    time_t now = time(NULL);

    if (storage == NULL || mail == NULL || from_user == NULL ||
        to_user == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (subject == NULL) {
        subject = "";
    }
    if (body == NULL) {
        body = "";
    }

    db = mailboxd_storage_sql_mail_db(storage);
    if (db == NULL) {
        return MAILBOXD_ERR_IO;
    }

    mail_sql_owner_norm(owner, sizeof(owner), to_user);

    hres = mail_sql_meta_bump(db, "mail_next", &id);
    if (hres != MAILBOXD_OK) {
        return hres;
    }

    rc = sqlite3_prepare_v2(db,
                            "INSERT INTO messages(id,owner,from_user,subject,"
                            "body,received_at,read_flag,deleted_at,folder) "
                            "VALUES(?1,?2,?3,?4,?5,?6,0,0,?7);",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
    sqlite3_bind_text(stmt, 2, owner, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, from_user, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, subject, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, body, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)now);
    sqlite3_bind_int(stmt, 7, MAIL_FOLDER_INBOX);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return MAILBOXD_ERR_IO;
    }

    return mail_sql_trim_inbox(db, owner, mail->max_messages);
}

unsigned mailboxd_mail_sql_purge_recycle(mailboxd_storage_t *storage,
                                      const mailboxd_mail_config_t *mail,
                                      const char *username)
{
    sqlite3 *db;
    sqlite3_stmt *stmt;
    char owner[MAILBOXD_USER_NAME_MAX];
    time_t cutoff;
    int rc;

    if (storage == NULL || mail == NULL || username == NULL) {
        return 0;
    }

    db = mailboxd_storage_sql_mail_db(storage);
    if (db == NULL) {
        return 0;
    }

    mail_sql_owner_norm(owner, sizeof(owner), username);
    cutoff = time(NULL) - (time_t)mail->recycle_days * 86400;

    rc = sqlite3_prepare_v2(db,
                            "DELETE FROM messages WHERE owner=?1 AND folder=?2 "
                            "AND deleted_at > 0 AND deleted_at < ?3;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, MAIL_FOLDER_RECYCLE);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)cutoff);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? (unsigned)sqlite3_changes(db) : 0;
}

static mailboxd_result_t mail_sql_resolve_index(mailboxd_service_t *service,
                                             mailboxd_session_t *session,
                                             unsigned list_index,
                                             uint64_t *out_id)
{
    const mailboxd_mail_config_t *mail;
    mailboxd_mail_entry_t entries[MAILBOXD_MAIL_MAX_MESSAGES];
    size_t count;
    mailboxd_result_t rc;

    mail = mailboxd_service_get_mail(service);
    if (mail == NULL || !mail->enabled) {
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    rc = mailboxd_mail_sql_load_inbox(mailboxd_service_get_storage(service),
                                  mailboxd_session_username(session),
                                  entries, MAILBOXD_MAIL_MAX_MESSAGES, &count);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (list_index == 0 || list_index > count) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    *out_id = entries[list_index - 1].id;
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_mail_sql_read(mailboxd_service_t *service,
                                   mailboxd_session_t *session,
                                   unsigned list_index)
{
    mailboxd_storage_t *storage;
    sqlite3 *db;
    sqlite3_stmt *stmt;
    uint64_t id;
    char owner[MAILBOXD_USER_NAME_MAX];
    char line[MAILBOXD_LINE_MAX];
    int rc;
    mailboxd_result_t hres;
    const char *from_user;
    const char *subject;
    const char *body;

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    hres = mail_sql_resolve_index(service, session, list_index, &id);
    if (hres == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "No such message.");
        return MAILBOXD_OK;
    }
    if (hres != MAILBOXD_OK) {
        return hres;
    }

    db = mailboxd_storage_sql_mail_db(storage);
    if (db == NULL) {
        return MAILBOXD_ERR_IO;
    }

    mail_sql_owner_norm(owner, sizeof(owner), mailboxd_session_username(session));

    rc = sqlite3_prepare_v2(db,
                            "SELECT from_user,subject,body FROM messages "
                            "WHERE id=?1 AND owner=?2 AND folder=?3;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
    sqlite3_bind_text(stmt, 2, owner, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, MAIL_FOLDER_INBOX);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        mailboxd_session_write_line(session, "Cannot open message.");
        return MAILBOXD_ERR_IO;
    }

    from_user = (const char *)sqlite3_column_text(stmt, 0);
    subject = (const char *)sqlite3_column_text(stmt, 1);
    body = (const char *)sqlite3_column_text(stmt, 2);
    if (from_user == NULL) {
        from_user = "";
    }
    if (subject == NULL) {
        subject = "";
    }
    if (body == NULL) {
        body = "";
    }

    snprintf(line, sizeof(line), "From: %s", from_user);
    mailboxd_session_write_line(session, line);
    snprintf(line, sizeof(line), "Subject: %s", subject);
    mailboxd_session_write_line(session, line);

    {
        const char *p = body;
        const char *nl;

        while (*p != '\0') {
            nl = strchr(p, '\n');
            if (nl == NULL) {
                mailboxd_session_write_line(session, p);
                break;
            }
            if (nl > p) {
                size_t chunk = (size_t)(nl - p);

                if (chunk >= sizeof(line)) {
                    chunk = sizeof(line) - 1;
                }
                memcpy(line, p, chunk);
                line[chunk] = '\0';
                mailboxd_session_write_line(session, line);
            } else {
                mailboxd_session_write_line(session, "");
            }
            p = nl + 1;
        }
    }

    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db,
                            "UPDATE messages SET read_flag=1 "
                            "WHERE id=?1 AND owner=?2;",
                            -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
        sqlite3_bind_text(stmt, 2, owner, -1, SQLITE_STATIC);
        (void)sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_mail_sql_delete_range(mailboxd_service_t *service,
                                           mailboxd_session_t *session,
                                           unsigned from,
                                           unsigned to)
{
    const mailboxd_mail_config_t *mail;
    mailboxd_storage_t *storage;
    sqlite3 *db;
    sqlite3_stmt *stmt;
    mailboxd_mail_entry_t entries[MAILBOXD_MAIL_MAX_MESSAGES];
    char owner[MAILBOXD_USER_NAME_MAX];
    size_t count;
    size_t i;
    unsigned moved = 0;
    time_t now = time(NULL);
    int rc;
    mailboxd_result_t hres;

    mail = mailboxd_service_get_mail(service);
    storage = mailboxd_service_get_storage(service);
    if (mail == NULL || !mail->enabled || storage == NULL) {
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    if (to == 0) {
        to = from;
    }

    (void)mailboxd_mail_sql_purge_recycle(storage, mail,
                                       mailboxd_session_username(session));

    hres = mailboxd_mail_sql_load_inbox(storage, mailboxd_session_username(session),
                                      entries, MAILBOXD_MAIL_MAX_MESSAGES, &count);
    if (hres != MAILBOXD_OK) {
        return hres;
    }

    if (from > count || to > count) {
        mailboxd_session_write_line(session, "No such message.");
        return MAILBOXD_OK;
    }

    db = mailboxd_storage_sql_mail_db(storage);
    if (db == NULL) {
        return MAILBOXD_ERR_IO;
    }

    mail_sql_owner_norm(owner, sizeof(owner), mailboxd_session_username(session));

    rc = sqlite3_prepare_v2(db,
                            "UPDATE messages SET folder=?4, deleted_at=?3 "
                            "WHERE id=?1 AND owner=?2 AND folder=?5;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return MAILBOXD_ERR_IO;
    }

    for (i = (size_t)(from - 1); i < (size_t)to; i++) {
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)entries[i].id);
        sqlite3_bind_text(stmt, 2, owner, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)now);
        sqlite3_bind_int(stmt, 4, MAIL_FOLDER_RECYCLE);
        sqlite3_bind_int(stmt, 5, MAIL_FOLDER_INBOX);
        rc = sqlite3_step(stmt);
        sqlite3_reset(stmt);
        if (rc == SQLITE_DONE && sqlite3_changes(db) > 0) {
            moved++;
        }
    }

    sqlite3_finalize(stmt);

    if (moved == 0) {
        mailboxd_session_write_line(session, "Delete failed.");
        return MAILBOXD_ERR_IO;
    }

    if (moved == 1) {
        mailboxd_session_write_line(session,
            "Message moved to recycle (auto-purge after configured days).");
    } else {
        char buf[64];

        snprintf(buf, sizeof(buf),
                 "%u messages moved to recycle (auto-purge after %u days).",
                 moved, mail->recycle_days);
        mailboxd_session_write_line(session, buf);
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_mail_sql_recycle_empty(mailboxd_service_t *service,
                                            mailboxd_session_t *session)
{
    const mailboxd_mail_config_t *mail;
    mailboxd_storage_t *storage;
    sqlite3 *db;
    sqlite3_stmt *stmt;
    char owner[MAILBOXD_USER_NAME_MAX];
    int rc;
    char buf[64];

    mail = mailboxd_service_get_mail(service);
    storage = mailboxd_service_get_storage(service);
    if (mail == NULL || !mail->enabled || storage == NULL) {
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    db = mailboxd_storage_sql_mail_db(storage);
    if (db == NULL) {
        return MAILBOXD_ERR_IO;
    }

    mail_sql_owner_norm(owner, sizeof(owner), mailboxd_session_username(session));
    (void)mailboxd_mail_sql_purge_recycle(storage, mail, owner);

    rc = sqlite3_prepare_v2(db,
                            "DELETE FROM messages WHERE owner=?1 AND folder=?2;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        mailboxd_session_write_line(session, "Cannot read recycle bin.");
        return MAILBOXD_ERR_IO;
    }

    sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, MAIL_FOLDER_RECYCLE);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        mailboxd_session_write_line(session, "Cannot read recycle bin.");
        return MAILBOXD_ERR_IO;
    }

    if (sqlite3_changes(db) == 0) {
        mailboxd_session_write_line(session, "Recycle bin empty.");
    } else {
        snprintf(buf, sizeof(buf), "Recycle bin emptied (%d message(s)).",
                 sqlite3_changes(db));
        mailboxd_session_write_line(session, buf);
    }

    return MAILBOXD_OK;
}

#else /* MAILBOXD_HAVE_SQLITE */

mailboxd_result_t mailboxd_mail_sql_load_inbox(mailboxd_storage_t *storage,
                                         const char *username,
                                         mailboxd_mail_entry_t *entries,
                                         size_t max_entries,
                                         size_t *out_count)
{
    (void)storage;
    (void)username;
    (void)entries;
    (void)max_entries;
    (void)out_count;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_mail_sql_deliver(mailboxd_storage_t *storage,
                                      const mailboxd_mail_config_t *mail,
                                      const char *from_user,
                                      const char *to_user,
                                      const char *subject,
                                      const char *body)
{
    (void)storage;
    (void)mail;
    (void)from_user;
    (void)to_user;
    (void)subject;
    (void)body;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_mail_sql_read(mailboxd_service_t *service,
                                   mailboxd_session_t *session,
                                   unsigned list_index)
{
    (void)service;
    (void)session;
    (void)list_index;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_mail_sql_delete_range(mailboxd_service_t *service,
                                           mailboxd_session_t *session,
                                           unsigned from,
                                           unsigned to)
{
    (void)service;
    (void)session;
    (void)from;
    (void)to;
    return MAILBOXD_ERR_UNSUPPORTED;
}

mailboxd_result_t mailboxd_mail_sql_recycle_empty(mailboxd_service_t *service,
                                            mailboxd_session_t *session)
{
    (void)service;
    (void)session;
    return MAILBOXD_ERR_UNSUPPORTED;
}

unsigned mailboxd_mail_sql_purge_recycle(mailboxd_storage_t *storage,
                                      const mailboxd_mail_config_t *mail,
                                      const char *username)
{
    (void)storage;
    (void)mail;
    (void)username;
    return 0;
}

#endif /* MAILBOXD_HAVE_SQLITE */
