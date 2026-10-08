/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/mail.h"
#include "mailboxd/messages.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/storage.h"
#include "mailboxd/auth.h"
#include "mailboxd/util.h"
#include "mailboxd/limits.h"
#include "mailboxd/log.h"
#include "mail_sql.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAILBOXD_MAIL_DIR_NAME "mail"
#define MAILBOXD_MAIL_INBOX_NAME "inbox"
#define MAILBOXD_MAIL_RECYCLE_NAME "recycle"
#define MAILBOXD_MAIL_NEXT_FILE "mail.next"

static int str_ieq(const char *a, const char *b)
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

static mailboxd_result_t read_counter(const char *path, uint64_t *value)
{
    FILE *fp;
    unsigned long long n = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        *value = 0;
        return MAILBOXD_OK;
    }

    if (fscanf(fp, "%llu", &n) != 1) {
        fclose(fp);
        return MAILBOXD_ERR_IO;
    }

    fclose(fp);
    *value = (uint64_t)n;
    return MAILBOXD_OK;
}

static mailboxd_result_t write_counter(const char *path, uint64_t value)
{
    FILE *fp;

    fp = fopen(path, "w");
    if (fp == NULL) {
        return MAILBOXD_ERR_IO;
    }

    fprintf(fp, "%llu\n", (unsigned long long)value);
    fclose(fp);
    return MAILBOXD_OK;
}

static mailboxd_result_t mail_user_inbox_path(const mailboxd_mail_config_t *mail,
                                           const char *username,
                                           char *out, size_t out_len)
{
    char user_dir[MAILBOXD_PATH_MAX];
    char user_norm[MAILBOXD_USER_NAME_MAX];

    if (mail == NULL || username == NULL || username[0] == '\0' ||
        out == NULL || out_len == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    mailboxd_strlcpy(user_norm, username, sizeof(user_norm));
    mailboxd_username_normalize(user_norm);

    if (mailboxd_path_join(user_dir, sizeof(user_dir), mail->root, user_norm) !=
        MAILBOXD_OK) {
        return MAILBOXD_ERR_INVALID;
    }

    return mailboxd_path_join(out, out_len, user_dir, MAILBOXD_MAIL_INBOX_NAME);
}

static mailboxd_result_t mail_user_recycle_path(const mailboxd_mail_config_t *mail,
                                             const char *username,
                                             char *out, size_t out_len)
{
    char user_dir[MAILBOXD_PATH_MAX];
    char user_norm[MAILBOXD_USER_NAME_MAX];

    if (mail == NULL || username == NULL || username[0] == '\0' ||
        out == NULL || out_len == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    mailboxd_strlcpy(user_norm, username, sizeof(user_norm));
    mailboxd_username_normalize(user_norm);

    if (mailboxd_path_join(user_dir, sizeof(user_dir), mail->root, user_norm) !=
        MAILBOXD_OK) {
        return MAILBOXD_ERR_INVALID;
    }

    return mailboxd_path_join(out, out_len, user_dir, MAILBOXD_MAIL_RECYCLE_NAME);
}

static int parse_msg_filename(const char *name, uint64_t *out_id)
{
    char *end;
    unsigned long long id;

    if (name == NULL || out_id == NULL) {
        return 0;
    }

    if (strlen(name) < 5 || strcmp(name + strlen(name) - 4, ".msg") != 0) {
        return 0;
    }

    id = strtoull(name, &end, 10);
    if (end == name || strcmp(end, ".msg") != 0) {
        return 0;
    }

    *out_id = (uint64_t)id;
    return 1;
}

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

static mailboxd_result_t load_inbox(const char *inbox_path,
                                 mailboxd_mail_entry_t *entries,
                                 size_t max_entries,
                                 size_t *out_count)
{
    DIR *dir;
    struct dirent *ent;
    size_t count = 0;

    if (inbox_path == NULL || entries == NULL || out_count == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    *out_count = 0;

    dir = opendir(inbox_path);
    if (dir == NULL) {
        if (errno == ENOENT) {
            return MAILBOXD_OK;
        }
        return MAILBOXD_ERR_IO;
    }

    while ((ent = readdir(dir)) != NULL) {
        uint64_t id;
        char path[MAILBOXD_PATH_MAX];
        FILE *fp;
        char line[MAILBOXD_LINE_MAX];
        mailboxd_mail_entry_t entry;

        if (!parse_msg_filename(ent->d_name, &id)) {
            continue;
        }

        if (count >= max_entries) {
            break;
        }

        memset(&entry, 0, sizeof(entry));
        entry.id = id;

        if (mailboxd_path_join(path, sizeof(path), inbox_path, ent->d_name) !=
            MAILBOXD_OK) {
            continue;
        }

        fp = fopen(path, "r");
        if (fp == NULL) {
            continue;
        }

        while (fgets(line, sizeof(line), fp) != NULL) {
            char *eq;
            char *key;
            char *value;

            if (line[0] == '-' && line[1] == '-' && line[2] == '-') {
                break;
            }

            eq = strchr(line, '=');
            if (eq == NULL) {
                continue;
            }

            *eq = '\0';
            key = line;
            value = eq + 1;

            while (*value == ' ' || *value == '\t') {
                value++;
            }

            {
                size_t vlen = strlen(value);

                while (vlen > 0 && (value[vlen - 1] == '\n' ||
                                    value[vlen - 1] == '\r')) {
                    value[--vlen] = '\0';
                }
            }

            if (str_ieq(key, "from")) {
                mailboxd_strlcpy(entry.from, value, sizeof(entry.from));
            } else if (str_ieq(key, "subject")) {
                mailboxd_strlcpy(entry.subject, value, sizeof(entry.subject));
            } else if (str_ieq(key, "time")) {
                entry.received_at = (time_t)strtol(value, NULL, 10);
            } else if (str_ieq(key, "read")) {
                entry.read = mailboxd_bool_is_true(value);
            }
        }

        fclose(fp);
        entries[count++] = entry;
    }

    closedir(dir);

    if (count > 1) {
        qsort(entries, count, sizeof(entries[0]), mail_entry_compare);
    }

    *out_count = count;
    return MAILBOXD_OK;
}

static int mail_uses_sqlite(mailboxd_service_t *service)
{
    mailboxd_storage_t *storage;

    if (service == NULL) {
        return 0;
    }

    storage = mailboxd_service_get_storage(service);
    return storage != NULL &&
           mailboxd_storage_backend(storage) == MAILBOXD_STORAGE_SQLITE;
}

static mailboxd_result_t mail_load_inbox(mailboxd_service_t *service,
                                      const mailboxd_mail_config_t *mail,
                                      const char *username,
                                      mailboxd_mail_entry_t *entries,
                                      size_t max_entries,
                                      size_t *out_count)
{
    mailboxd_storage_t *storage;
    char inbox[MAILBOXD_PATH_MAX];
    mailboxd_result_t rc;

    if (mail_uses_sqlite(service)) {
        storage = mailboxd_service_get_storage(service);
        return mailboxd_mail_sql_load_inbox(storage, username, entries,
                                         max_entries, out_count);
    }

    rc = mail_user_inbox_path(mail, username, inbox, sizeof(inbox));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    return load_inbox(inbox, entries, max_entries, out_count);
}

static mailboxd_result_t msg_file_path(const char *inbox_path, uint64_t id,
                                    char *out, size_t out_len)
{
    char name[32];

    snprintf(name, sizeof(name), "%06llu.msg",
             (unsigned long long)id);
    return mailboxd_path_join(out, out_len, inbox_path, name);
}

void mailboxd_mail_config_defaults(mailboxd_mail_config_t *mail)
{
    if (mail == NULL) {
        return;
    }

    mail->enabled = 1;
    mail->max_messages = MAILBOXD_MAIL_MAX_MESSAGES;
    mail->subject_max = MAILBOXD_MAIL_SUBJECT_MAX;
    mail->body_max = MAILBOXD_MAIL_BODY_MAX;
    mail->recycle_days = MAILBOXD_MAIL_DEFAULT_RECYCLE_DAYS;
    mail->root[0] = '\0';
}

void mailboxd_mail_config_apply(mailboxd_mail_config_t *mail,
                             const mailboxd_config_t *config,
                             const char *storage_path)
{
    if (mail == NULL) {
        return;
    }

    mailboxd_mail_config_defaults(mail);

    if (config != NULL) {
        mail->enabled = mailboxd_config_get_bool(config, "mail", "enabled", 1);
        mail->max_messages = mailboxd_config_get_uint(
            config, "mail", "max_messages", MAILBOXD_MAIL_MAX_MESSAGES, 1u,
            MAILBOXD_MAIL_MAX_MESSAGES);
        mail->subject_max = mailboxd_config_get_uint(
            config, "mail", "subject_max", MAILBOXD_MAIL_SUBJECT_MAX, 8u,
            MAILBOXD_MAIL_SUBJECT_MAX);
        mail->body_max = mailboxd_config_get_uint(
            config, "mail", "body_max", MAILBOXD_MAIL_BODY_MAX, 64u,
            MAILBOXD_MAIL_BODY_MAX);
        mail->recycle_days = mailboxd_config_get_uint(
            config, "mail", "recycle_days", MAILBOXD_MAIL_DEFAULT_RECYCLE_DAYS,
            1u, 365u);
    }

    mail->root[0] = '\0';
    if (storage_path != NULL && storage_path[0] != '\0') {
        const char *subdir = "mail";
        const char *custom;

        if (config != NULL) {
            custom = mailboxd_config_get(config, "mail", "path", NULL);
            if (custom != NULL && custom[0] != '\0') {
                subdir = custom;
            }
        }

        if (mailboxd_path_join(mail->root, sizeof(mail->root), storage_path,
                            subdir) != MAILBOXD_OK) {
            mail->root[0] = '\0';
        }
    }

    mailboxd_log_info("[mail] enabled=%s max_messages=%u recycle_days=%u root=%s",
           mailboxd_bool_to_string(mail->enabled), mail->max_messages,
           mail->recycle_days,
           mail->root[0] != '\0' ? mail->root : "(unset)");
}

static mailboxd_result_t mail_ensure_root(const mailboxd_mail_config_t *mail)
{
    char next_path[MAILBOXD_PATH_MAX];

    if (mail == NULL || !mail->enabled) {
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    if (mail->root[0] == '\0') {
        return MAILBOXD_ERR_IO;
    }

    if (mkdir_p(mail->root) != 0) {
        return MAILBOXD_ERR_IO;
    }

    if (mailboxd_path_join(next_path, sizeof(next_path), mail->root,
                        MAILBOXD_MAIL_NEXT_FILE) != MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    {
        FILE *fp = fopen(next_path, "r");

        if (fp == NULL) {
            return write_counter(next_path, 0);
        }
        fclose(fp);
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t mail_next_id(const mailboxd_mail_config_t *mail,
                                   uint64_t *out_id)
{
    char next_path[MAILBOXD_PATH_MAX];
    uint64_t id;
    mailboxd_result_t rc;

    if (mailboxd_path_join(next_path, sizeof(next_path), mail->root,
                        MAILBOXD_MAIL_NEXT_FILE) != MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    rc = read_counter(next_path, &id);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    id++;
    rc = write_counter(next_path, id);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    *out_id = id;
    return MAILBOXD_OK;
}

static mailboxd_result_t mail_trim_inbox(mailboxd_service_t *service,
                                      const mailboxd_mail_config_t *mail,
                                      const char *username,
                                      unsigned max_messages)
{
    mailboxd_mail_entry_t entries[MAILBOXD_MAIL_MAX_MESSAGES];
    size_t count;
    size_t i;
    mailboxd_result_t rc;
    char inbox[MAILBOXD_PATH_MAX];

    if (mail_uses_sqlite(service)) {
        return MAILBOXD_OK;
    }

    rc = mail_user_inbox_path(mail, username, inbox, sizeof(inbox));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = load_inbox(inbox, entries, MAILBOXD_MAIL_MAX_MESSAGES, &count);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (count <= max_messages) {
        return MAILBOXD_OK;
    }

    for (i = max_messages; i < count; i++) {
        char path[MAILBOXD_PATH_MAX];

        if (msg_file_path(inbox, entries[i].id, path, sizeof(path)) ==
            MAILBOXD_OK) {
            remove(path);
        }
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t mail_ensure_storage(mailboxd_service_t *service,
                                          const mailboxd_mail_config_t *mail)
{
    if (mail == NULL || !mail->enabled) {
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    if (mail_uses_sqlite(service)) {
        return MAILBOXD_OK;
    }

    return mail_ensure_root(mail);
}

void mailboxd_mail_list_inbox(mailboxd_service_t *service, mailboxd_session_t *session)
{
    mailboxd_mail_list_inbox_range(service, session, 1, 0);
}

static const char *mail_display_name(mailboxd_service_t *service,
                                     const char *name,
                                     char *buf, size_t buf_len)
{
    mailboxd_storage_t *storage;
    mailboxd_user_record_t user;

    if (name == NULL || name[0] == '\0') {
        return "";
    }

    storage = mailboxd_service_get_storage(service);
    if (storage != NULL &&
        mailboxd_storage_resolve_user(storage, name, &user) == MAILBOXD_OK) {
        return mailboxd_user_display_name(&user);
    }

    mailboxd_strlcpy(buf, name, buf_len);
    return buf;
}

static void mail_list_print(mailboxd_service_t *service,
                            mailboxd_session_t *session,
                            const mailboxd_mail_entry_t *entries,
                            size_t count,
                            unsigned from,
                            unsigned to)
{
    size_t i;
    size_t start;
    size_t end;
    char line[MAILBOXD_MAIL_SUBJECT_MAX + 64];
    char header[48];
    char from_name[MAILBOXD_USER_NAME_MAX];

    if (to == 0 || to > count) {
        to = (unsigned)count;
    }

    if (count == 0) {
        mailboxd_session_write_line(session, "Inbox empty.");
        mailboxd_session_write_line(session,
            "Use /mail send <user> <subject> to compose.");
        return;
    }

    if (from == 0 || from > count || from > to) {
        mailboxd_session_write_line(session, "No messages in that range.");
        return;
    }

    start = (size_t)(from - 1);
    end = (size_t)to;

    if (from == 1 && to == (unsigned)count) {
        mailboxd_session_write_line(session, "Inbox (newest first):");
    } else {
        snprintf(header, sizeof(header), "Inbox %u-%u (newest first):",
                 from, to);
        mailboxd_session_write_line(session, header);
    }

    for (i = start; i < end; i++) {
        snprintf(line, sizeof(line), "  %zu  %s%s  %s",
                 i + 1,
                 entries[i].read ? " " : "* ",
                 mail_display_name(service, entries[i].from, from_name,
                                   sizeof(from_name)),
                 entries[i].subject[0] != '\0' ? entries[i].subject
                                               : "(no subject)");
        mailboxd_session_write_line(session, line);
    }

    mailboxd_session_write_line(session,
        "  /mail read <n>  delete <n|from-to>  list <from-to>  recycle");
}

static unsigned mail_recycle_purge_expired(const mailboxd_mail_config_t *mail,
                                           const char *recycle_path)
{
    DIR *dir;
    struct dirent *ent;
    time_t now = time(NULL);
    time_t max_age;
    unsigned removed = 0;

    if (mail == NULL || recycle_path == NULL || recycle_path[0] == '\0') {
        return 0;
    }

    max_age = (time_t)mail->recycle_days * 86400;

    dir = opendir(recycle_path);
    if (dir == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        return 0;
    }

    while ((ent = readdir(dir)) != NULL) {
        uint64_t id;
        char path[MAILBOXD_PATH_MAX];
        struct stat st;

        if (!parse_msg_filename(ent->d_name, &id)) {
            continue;
        }

        if (mailboxd_path_join(path, sizeof(path), recycle_path, ent->d_name) !=
            MAILBOXD_OK) {
            continue;
        }

        if (stat(path, &st) != 0) {
            continue;
        }

        if (now - st.st_mtime >= max_age) {
            if (remove(path) == 0) {
                removed++;
            }
        }
    }

    closedir(dir);
    return removed;
}

static mailboxd_result_t mail_move_id_to_recycle(const char *inbox_path,
                                              const char *recycle_path,
                                              uint64_t id)
{
    char src[MAILBOXD_PATH_MAX];
    char dst[MAILBOXD_PATH_MAX];
    FILE *in_fp;
    FILE *out_fp;
    char line[MAILBOXD_LINE_MAX];
    char tmp[MAILBOXD_PATH_MAX + 8];
    int has_deleted = 0;

    if (msg_file_path(inbox_path, id, src, sizeof(src)) != MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    if (mkdir_p(recycle_path) != 0) {
        return MAILBOXD_ERR_IO;
    }

    if (msg_file_path(recycle_path, id, dst, sizeof(dst)) != MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    if (strlen(dst) + 4 >= sizeof(tmp)) {
        return MAILBOXD_ERR_IO;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);

    in_fp = fopen(src, "r");
    out_fp = fopen(tmp, "w");
    if (in_fp == NULL || out_fp == NULL) {
        if (in_fp != NULL) {
            fclose(in_fp);
        }
        if (out_fp != NULL) {
            fclose(out_fp);
        }
        remove(tmp);
        return MAILBOXD_ERR_IO;
    }

    while (fgets(line, sizeof(line), in_fp) != NULL) {
        if (strncmp(line, "deleted=", 8) == 0) {
            continue;
        }
        if (!has_deleted && line[0] == '-' && line[1] == '-' && line[2] == '-') {
            fprintf(out_fp, "deleted=%ld\n", (long)time(NULL));
            has_deleted = 1;
        }
        fputs(line, out_fp);
    }

    if (!has_deleted) {
        fprintf(out_fp, "deleted=%ld\n", (long)time(NULL));
    }

    fclose(in_fp);
    fclose(out_fp);

    if (remove(src) != 0) {
        remove(tmp);
        return MAILBOXD_ERR_IO;
    }

    if (rename(tmp, dst) != 0) {
        remove(tmp);
        return MAILBOXD_ERR_IO;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t mail_purge_user_recycle(mailboxd_service_t *service,
                                              const mailboxd_mail_config_t *mail,
                                              const char *username)
{
    char recycle[MAILBOXD_PATH_MAX];

    if (mail_uses_sqlite(service)) {
        (void)mailboxd_mail_sql_purge_recycle(mailboxd_service_get_storage(service),
                                           mail, username);
        return MAILBOXD_OK;
    }

    if (mail_user_recycle_path(mail, username, recycle, sizeof(recycle)) !=
        MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    (void)mail_recycle_purge_expired(mail, recycle);
    return MAILBOXD_OK;
}

void mailboxd_mail_list_inbox_range(mailboxd_service_t *service,
                                 mailboxd_session_t *session,
                                 unsigned from,
                                 unsigned to)
{
    const mailboxd_mail_config_t *mail;
    mailboxd_mail_entry_t entries[MAILBOXD_MAIL_MAX_MESSAGES];
    char inbox[MAILBOXD_PATH_MAX];
    size_t count;
    mailboxd_result_t rc;

    if (service == NULL || session == NULL) {
        return;
    }

    if (mailboxd_session_is_guest(session)) {
        mailboxd_session_write_line(session, "Guests cannot use mail.");
        return;
    }

    mail = mailboxd_service_get_mail(service);
    if (mail == NULL || !mail->enabled) {
        mailboxd_session_write_line(session, "Mail is disabled.");
        return;
    }

    rc = mail_ensure_storage(service, mail);
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Mail storage unavailable.");
        return;
    }

    if (!mail_uses_sqlite(service)) {
        rc = mail_user_inbox_path(mail, mailboxd_session_username(session),
                                  inbox, sizeof(inbox));
        if (rc != MAILBOXD_OK) {
            mailboxd_session_write_line(session, "Mail path error.");
            return;
        }
    }

    (void)mail_purge_user_recycle(service, mail, mailboxd_session_username(session));

    rc = mail_load_inbox(service, mail, mailboxd_session_username(session),
                         entries, MAILBOXD_MAIL_MAX_MESSAGES, &count);
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Cannot read inbox.");
        return;
    }

    mail_list_print(service, session, entries, count, from, to);
}

void mailboxd_mail_announce_since_last_login(mailboxd_service_t *service,
                                            mailboxd_session_t *session,
                                            time_t since_login)
{
    const mailboxd_mail_config_t *mail;
    mailboxd_mail_entry_t entries[MAILBOXD_MAIL_MAX_MESSAGES];
    char inbox[MAILBOXD_PATH_MAX];
    size_t count;
    size_t i;
    size_t new_count = 0;
    mailboxd_result_t rc;

    if (service == NULL || session == NULL) {
        return;
    }

    if (mailboxd_session_is_guest(session)) {
        return;
    }

    mail = mailboxd_service_get_mail(service);
    if (mail == NULL || !mail->enabled) {
        return;
    }

    rc = mail_ensure_storage(service, mail);
    if (rc != MAILBOXD_OK) {
        return;
    }

    if (!mail_uses_sqlite(service)) {
        rc = mail_user_inbox_path(mail, mailboxd_session_username(session),
                                  inbox, sizeof(inbox));
        if (rc != MAILBOXD_OK) {
            return;
        }
    }

    (void)mail_purge_user_recycle(service, mail, mailboxd_session_username(session));

    rc = mail_load_inbox(service, mail, mailboxd_session_username(session),
                         entries, MAILBOXD_MAIL_MAX_MESSAGES, &count);
    if (rc != MAILBOXD_OK || count == 0) {
        return;
    }

    for (i = 0; i < count; i++) {
        if (since_login == 0 || entries[i].received_at > since_login) {
            new_count++;
        }
    }

    if (new_count == 0) {
        return;
    }

    if (new_count == 1) {
        (void)mailboxd_msg_send_system(session,
            "You have 1 new message since your last login. (/mail)");
    } else {
        char body[96];
        snprintf(body, sizeof(body),
                 "You have %zu new messages since your last login. (/mail)",
                 new_count);
        (void)mailboxd_msg_send_system(session, body);
    }
}

int mailboxd_mail_parse_list_range(const char *spec,
                                unsigned *from, unsigned *to)
{
    const char *dash;
    char *end;
    unsigned long start;
    unsigned long end_num;

    if (from == NULL || to == NULL) {
        return 0;
    }

    if (spec == NULL || spec[0] == '\0') {
        *from = 1;
        *to = 0;
        return 1;
    }

    dash = strchr(spec, '-');
    if (dash == NULL) {
        start = strtoul(spec, &end, 10);
        if (end == spec || *end != '\0' || start == 0) {
            return 0;
        }
        *from = (unsigned)start;
        *to = (unsigned)start;
        return 1;
    }

    start = strtoul(spec, &end, 10);
    if (end != dash || start == 0) {
        return 0;
    }

    end_num = strtoul(dash + 1, &end, 10);
    if (end == dash + 1 || *end != '\0' || end_num == 0) {
        return 0;
    }

    if (start > end_num) {
        return 0;
    }

    *from = (unsigned)start;
    *to = (unsigned)end_num;
    return 1;
}

static mailboxd_result_t mail_resolve_index(mailboxd_service_t *service,
                                         mailboxd_session_t *session,
                                         unsigned list_index,
                                         uint64_t *out_id,
                                         char *inbox_path, size_t inbox_len)
{
    const mailboxd_mail_config_t *mail;
    mailboxd_mail_entry_t entries[MAILBOXD_MAIL_MAX_MESSAGES];
    size_t count;
    mailboxd_result_t rc;

    mail = mailboxd_service_get_mail(service);
    if (mail == NULL || !mail->enabled) {
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    rc = mail_ensure_storage(service, mail);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (!mail_uses_sqlite(service)) {
        rc = mail_user_inbox_path(mail, mailboxd_session_username(session),
                                  inbox_path, inbox_len);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    rc = mail_load_inbox(service, mail, mailboxd_session_username(session),
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

static void mail_print_header_line(mailboxd_service_t *service,
                                   mailboxd_session_t *session,
                                   const char *line)
{
    char out_line[MAILBOXD_LINE_MAX];
    char name_buf[MAILBOXD_USER_NAME_MAX];
    char fallback[MAILBOXD_USER_NAME_MAX];
    const char *value;
    const char *display;
    size_t vlen;

    if (line == NULL || session == NULL) {
        return;
    }

    if (strncmp(line, "from=", 5) == 0) {
        value = line + 5;
        vlen = strlen(value);
        while (vlen > 0 && (value[vlen - 1] == '\n' || value[vlen - 1] == '\r')) {
            vlen--;
        }
        memcpy(name_buf, value, vlen);
        name_buf[vlen] = '\0';
        display = mail_display_name(service, name_buf, fallback, sizeof(fallback));
        snprintf(out_line, sizeof(out_line), "from=%s", display);
        mailboxd_session_write_line(session, out_line);
        return;
    }

    if (strncmp(line, "to=", 3) == 0) {
        value = line + 3;
        vlen = strlen(value);
        while (vlen > 0 && (value[vlen - 1] == '\n' || value[vlen - 1] == '\r')) {
            vlen--;
        }
        memcpy(name_buf, value, vlen);
        name_buf[vlen] = '\0';
        display = mail_display_name(service, name_buf, fallback, sizeof(fallback));
        snprintf(out_line, sizeof(out_line), "to=%s", display);
        mailboxd_session_write_line(session, out_line);
        return;
    }

    mailboxd_session_write_line(session, line);
}

mailboxd_result_t mailboxd_mail_read(mailboxd_service_t *service,
                               mailboxd_session_t *session,
                               unsigned list_index)
{
    char inbox[MAILBOXD_PATH_MAX];
    char path[MAILBOXD_PATH_MAX];
    uint64_t id;
    FILE *fp;
    char line[MAILBOXD_LINE_MAX];
    int in_body = 0;
    mailboxd_result_t rc;

    if (mailboxd_session_is_guest(session)) {
        mailboxd_session_write_line(session, "Guests cannot use mail.");
        return MAILBOXD_ERR_DENIED;
    }

    if (mail_uses_sqlite(service)) {
        (void)mailboxd_mail_sql_purge_recycle(mailboxd_service_get_storage(service),
                                           mailboxd_service_get_mail(service),
                                           mailboxd_session_username(session));
        return mailboxd_mail_sql_read(service, session, list_index);
    }

    (void)mail_purge_user_recycle(service, mailboxd_service_get_mail(service),
                                  mailboxd_session_username(session));

    rc = mail_resolve_index(service, session, list_index, &id,
                            inbox, sizeof(inbox));
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "No such message.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (msg_file_path(inbox, id, path, sizeof(path)) != MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        mailboxd_session_write_line(session, "Cannot open message.");
        return MAILBOXD_ERR_IO;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (!in_body) {
            if (line[0] == '-' && line[1] == '-' && line[2] == '-') {
                in_body = 1;
                continue;
            }
            if (strncmp(line, "read=", 5) == 0) {
                continue;
            }
            if (strncmp(line, "from=", 5) == 0 ||
                strncmp(line, "to=", 3) == 0) {
                mail_print_header_line(service, session, line);
                continue;
            }
        }
        mailboxd_session_write_line(session, line);
    }

    fclose(fp);

    {
        char tmp_path[MAILBOXD_PATH_MAX + 8];
        FILE *in_fp;
        FILE *out_fp;
        char buf[MAILBOXD_LINE_MAX];

        if (strlen(path) + 4 >= sizeof(tmp_path)) {
            return MAILBOXD_OK;
        }
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
        in_fp = fopen(path, "r");
        out_fp = fopen(tmp_path, "w");
        if (in_fp != NULL && out_fp != NULL) {
            while (fgets(buf, sizeof(buf), in_fp) != NULL) {
                if (strncmp(buf, "read=", 5) == 0) {
                    fputs("read=yes\n", out_fp);
                } else {
                    fputs(buf, out_fp);
                }
            }
            fclose(in_fp);
            fclose(out_fp);
            remove(path);
            rename(tmp_path, path);
        } else {
            if (in_fp != NULL) {
                fclose(in_fp);
            }
            if (out_fp != NULL) {
                fclose(out_fp);
            }
            remove(tmp_path);
        }
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_mail_delete_range(mailboxd_service_t *service,
                                       mailboxd_session_t *session,
                                       unsigned from,
                                       unsigned to)
{
    const mailboxd_mail_config_t *mail;
    mailboxd_mail_entry_t entries[MAILBOXD_MAIL_MAX_MESSAGES];
    char inbox[MAILBOXD_PATH_MAX];
    char recycle[MAILBOXD_PATH_MAX];
    size_t count;
    size_t i;
    unsigned moved = 0;
    mailboxd_result_t rc;

    if (mailboxd_session_is_guest(session)) {
        mailboxd_session_write_line(session, "Guests cannot use mail.");
        return MAILBOXD_ERR_DENIED;
    }

    mail = mailboxd_service_get_mail(service);
    if (mail == NULL || !mail->enabled) {
        mailboxd_session_write_line(session, "Mail is disabled.");
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    if (to == 0) {
        to = from;
    }
    if (from == 0 || from > to) {
        mailboxd_session_write_line(session, "Usage: /mail delete <n|from-to>");
        return MAILBOXD_OK;
    }

    if (mail_uses_sqlite(service)) {
        return mailboxd_mail_sql_delete_range(service, session, from, to);
    }

    rc = mail_ensure_root(mail);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = mail_user_inbox_path(mail, mailboxd_session_username(session),
                              inbox, sizeof(inbox));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = mail_user_recycle_path(mail, mailboxd_session_username(session),
                                recycle, sizeof(recycle));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    (void)mail_purge_user_recycle(service, mail, mailboxd_session_username(session));

    rc = mail_load_inbox(service, mail, mailboxd_session_username(session),
                         entries, MAILBOXD_MAIL_MAX_MESSAGES, &count);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (from > count || to > count) {
        mailboxd_session_write_line(session, "No such message.");
        return MAILBOXD_OK;
    }

    for (i = (size_t)(from - 1); i < (size_t)to; i++) {
        rc = mail_move_id_to_recycle(inbox, recycle, entries[i].id);
        if (rc != MAILBOXD_OK) {
            mailboxd_session_write_line(session, "Delete failed.");
            return rc;
        }
        moved++;
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

mailboxd_result_t mailboxd_mail_delete(mailboxd_service_t *service,
                               mailboxd_session_t *session,
                               unsigned list_index)
{
    return mailboxd_mail_delete_range(service, session, list_index, list_index);
}

mailboxd_result_t mailboxd_mail_recycle_empty(mailboxd_service_t *service,
                                        mailboxd_session_t *session)
{
    const mailboxd_mail_config_t *mail;
    char recycle[MAILBOXD_PATH_MAX];
    DIR *dir;
    struct dirent *ent;
    unsigned removed = 0;
    mailboxd_result_t rc;
    char buf[64];

    if (mailboxd_session_is_guest(session)) {
        mailboxd_session_write_line(session, "Guests cannot use mail.");
        return MAILBOXD_ERR_DENIED;
    }

    mail = mailboxd_service_get_mail(service);
    if (mail == NULL || !mail->enabled) {
        mailboxd_session_write_line(session, "Mail is disabled.");
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    if (mail_uses_sqlite(service)) {
        return mailboxd_mail_sql_recycle_empty(service, session);
    }

    rc = mail_ensure_root(mail);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = mail_user_recycle_path(mail, mailboxd_session_username(session),
                                recycle, sizeof(recycle));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    (void)mail_recycle_purge_expired(mail, recycle);

    dir = opendir(recycle);
    if (dir == NULL) {
        if (errno == ENOENT) {
            mailboxd_session_write_line(session, "Recycle bin empty.");
            return MAILBOXD_OK;
        }
        mailboxd_session_write_line(session, "Cannot read recycle bin.");
        return MAILBOXD_ERR_IO;
    }

    while ((ent = readdir(dir)) != NULL) {
        uint64_t id;
        char path[MAILBOXD_PATH_MAX];

        if (!parse_msg_filename(ent->d_name, &id)) {
            continue;
        }

        if (mailboxd_path_join(path, sizeof(path), recycle, ent->d_name) !=
            MAILBOXD_OK) {
            continue;
        }

        if (remove(path) == 0) {
            removed++;
        }
    }

    closedir(dir);

    if (removed == 0) {
        mailboxd_session_write_line(session, "Recycle bin empty.");
    } else {
        snprintf(buf, sizeof(buf), "Recycle bin emptied (%u message(s)).",
                 removed);
        mailboxd_session_write_line(session, buf);
    }

    return MAILBOXD_OK;
}

#define MAILBOXD_MAIL_SYSTEM_FROM "system"

static void mail_format_utc_time(time_t when, char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }

    buf[0] = '\0';
    if (when <= 0) {
        return;
    }

    {
        const struct tm *tm = gmtime(&when);

        if (tm != NULL) {
            (void)strftime(buf, len, "%Y-%m-%d %H:%M:%S UTC", tm);
        }
    }
}

typedef struct mail_notify_staff_ctx {
    mailboxd_service_t *service;
    char subject[MAILBOXD_MAIL_SUBJECT_MAX];
    char body[MAILBOXD_MAIL_BODY_MAX];
} mail_notify_staff_ctx_t;

static mailboxd_result_t mail_notify_staff_cb(const mailboxd_user_record_t *user,
                                           void *ctx)
{
    mail_notify_staff_ctx_t *nctx = (mail_notify_staff_ctx_t *)ctx;

    if (user == NULL || nctx == NULL) {
        return MAILBOXD_OK;
    }

    if (!user->active || !mailboxd_user_level_is_sysop_or_admin(user->level)) {
        return MAILBOXD_OK;
    }

    (void)mailboxd_mail_deliver(nctx->service, MAILBOXD_MAIL_SYSTEM_FROM,
                             user->username, nctx->subject, nctx->body);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_mail_notify_staff_registration(mailboxd_service_t *service,
                                                    const mailboxd_user_registration_t *reg,
                                                    const mailboxd_user_record_t *user)
{
    const mailboxd_mail_config_t *mail;
    mailboxd_storage_t *storage;
    mail_notify_staff_ctx_t ctx;
    char created[48];
    int n;

    if (service == NULL || reg == NULL || user == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    mail = mailboxd_service_get_mail(service);
    if (mail == NULL || !mail->enabled) {
        return MAILBOXD_OK;
    }

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    n = snprintf(ctx.subject, sizeof(ctx.subject),
                 "Registration pending: %s", reg->nickname);
    if (n < 0 || (size_t)n >= sizeof(ctx.subject)) {
        return MAILBOXD_ERR_INVALID;
    }

    mail_format_utc_time(user->created_at, created, sizeof(created));
    if (created[0] == '\0') {
        mailboxd_strlcpy(created, "(unknown)", sizeof(created));
    }

    n = snprintf(ctx.body, sizeof(ctx.body),
                 "Guest self-registration pending approval.\n"
                 "/register data submitted:\n\n"
                 "  Nickname:   %s\n"
                 "  Login:      %s\n"
                 "  Full name:  %s\n"
                 "  Country:    %s\n"
                 "  Location:   %s\n"
                 "  Email:      %s\n\n"
                 "Account record:\n\n"
                 "  User ID:    %llu\n"
                 "  Level:      %s\n"
                 "  Active:     %s\n"
                 "  Created:    %s\n\n"
                 "Review all fields. If correct, activate with:\n"
                 "  /activate %s\n",
                 reg->nickname,
                 reg->username,
                 reg->full_name,
                 reg->country,
                 reg->location,
                 reg->email,
                 (unsigned long long)user->id,
                 mailboxd_user_level_name(user->level),
                 mailboxd_bool_to_string(user->active),
                 created,
                 reg->username);
    if (n < 0 || (size_t)n >= sizeof(ctx.body)) {
        return MAILBOXD_ERR_INVALID;
    }

    ctx.service = service;

    (void)mailboxd_storage_foreach_user(storage, mail_notify_staff_cb, &ctx);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_mail_deliver(mailboxd_service_t *service,
                                  const char *from_user,
                                  const char *to_user,
                                  const char *subject,
                                  const char *body)
{
    const mailboxd_mail_config_t *mail;
    mailboxd_storage_t *storage;
    mailboxd_user_record_t recipient;
    char inbox[MAILBOXD_PATH_MAX];
    char path[MAILBOXD_PATH_MAX];
    uint64_t id;
    FILE *fp;
    mailboxd_result_t rc;
    char to_norm[MAILBOXD_USER_NAME_MAX];
    size_t body_len;

    if (service == NULL || from_user == NULL || to_user == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    mail = mailboxd_service_get_mail(service);
    if (mail == NULL || !mail->enabled) {
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    if (subject == NULL) {
        subject = "";
    }
    if (body == NULL) {
        body = "";
    }

    body_len = strlen(body);
    if (strlen(subject) > mail->subject_max || body_len > mail->body_max) {
        return MAILBOXD_ERR_INVALID;
    }

    mailboxd_strlcpy(to_norm, to_user, sizeof(to_norm));
    mailboxd_username_normalize(to_norm);

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_resolve_user(storage, to_user, &recipient);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        return MAILBOXD_ERR_NOT_FOUND;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    mailboxd_strlcpy(to_norm, recipient.username, sizeof(to_norm));

    if (mailboxd_user_level_is_guest(recipient.level) || !recipient.active) {
        return MAILBOXD_ERR_DENIED;
    }

    if (mail_uses_sqlite(service)) {
        return mailboxd_mail_sql_deliver(storage, mail, from_user, to_norm,
                                      subject, body);
    }

    rc = mail_ensure_root(mail);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = mail_user_inbox_path(mail, to_norm, inbox, sizeof(inbox));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (mkdir_p(inbox) != 0) {
        return MAILBOXD_ERR_IO;
    }

    rc = mail_next_id(mail, &id);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (msg_file_path(inbox, id, path, sizeof(path)) != MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    fp = fopen(path, "w");
    if (fp == NULL) {
        return MAILBOXD_ERR_IO;
    }

    fprintf(fp, "id=%llu\n", (unsigned long long)id);
    fprintf(fp, "from=%s\n", from_user);
    fprintf(fp, "to=%s\n", to_norm);
    fprintf(fp, "subject=%s\n", subject);
    fprintf(fp, "time=%ld\n", (long)time(NULL));
    fprintf(fp, "read=no\n");
    fprintf(fp, "---\n");
    fputs(body, fp);
    if (body_len == 0 || body[body_len - 1] != '\n') {
        fputc('\n', fp);
    }
    fclose(fp);

    (void)mail_trim_inbox(service, mail, to_norm, mail->max_messages);
    return MAILBOXD_OK;
}
