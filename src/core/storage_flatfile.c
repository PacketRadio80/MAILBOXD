/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/storage.h"
#include "mailboxd/auth.h"
#include "mailboxd/password.h"
#include "mailboxd/security.h"
#include "mailboxd/config.h"
#include "mailboxd/util.h"
#include "mailboxd/limits.h"
#include "mailboxd/log.h"
#include "storage_private.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

struct flatfile_state {
    char users_dir[MAILBOXD_PATH_MAX];
    char legacy_users_path[MAILBOXD_PATH_MAX];
    char sessions_path[MAILBOXD_PATH_MAX];
    char session_next_path[MAILBOXD_PATH_MAX];
    char user_next_path[MAILBOXD_PATH_MAX];
};

typedef struct user_shard {
    mailboxd_user_record_t users[MAILBOXD_USERS_PER_FILE];
    size_t count;
} user_shard_t;

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

static mailboxd_result_t bump_counter(const char *path, uint64_t *value)
{
    mailboxd_result_t rc;

    rc = read_counter(path, value);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    (*value)++;
    return write_counter(path, *value);
}

static mailboxd_result_t append_line(const char *path, const char *line)
{
    FILE *fp;

    fp = fopen(path, "a");
    if (fp == NULL) {
        return MAILBOXD_ERR_IO;
    }

    fprintf(fp, "%s\n", line);
    fclose(fp);
    return MAILBOXD_OK;
}

#define MAILBOXD_USER_NICK_SUFFIX ".nick"

static mailboxd_result_t user_nickname_path(const struct flatfile_state *state,
                                           const char *username,
                                           char *out, size_t out_len)
{
    char name[MAILBOXD_USER_NAME_MAX + 8];
    int n;

    if (state == NULL || username == NULL || username[0] == '\0' ||
        out == NULL || out_len == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    n = snprintf(name, sizeof(name), "%s%s", username, MAILBOXD_USER_NICK_SUFFIX);
    if (n < 0 || (size_t)n >= sizeof(name)) {
        return MAILBOXD_ERR_INVALID;
    }

    return mailboxd_path_join(out, out_len, state->users_dir, name);
}

static mailboxd_result_t read_nickname_file(const struct flatfile_state *state,
                                         const char *username,
                                         char *nickname,
                                         size_t nickname_len)
{
    char path[MAILBOXD_PATH_MAX];
    FILE *fp;
    char line[MAILBOXD_USER_NICKNAME_MAX];
    char *nl;
    mailboxd_result_t rc;

    if (nickname == NULL || nickname_len == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    nickname[0] = '\0';

    rc = user_nickname_path(state, username, path, sizeof(path));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return MAILBOXD_ERR_IO;
    }

    fclose(fp);

    nl = strchr(line, '\n');
    if (nl != NULL) {
        *nl = '\0';
    }
    nl = strchr(line, '\r');
    if (nl != NULL) {
        *nl = '\0';
    }

    if (line[0] == '\0') {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    mailboxd_strlcpy(nickname, line, nickname_len);
    return MAILBOXD_OK;
}

static mailboxd_result_t write_nickname_file(const struct flatfile_state *state,
                                            const char *username,
                                            const char *nickname)
{
    char path[MAILBOXD_PATH_MAX];
    FILE *fp;
    mailboxd_result_t rc;

    if (state == NULL || username == NULL || username[0] == '\0' ||
        nickname == NULL || nickname[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    rc = user_nickname_path(state, username, path, sizeof(path));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    fp = fopen(path, "w");
    if (fp == NULL) {
        return MAILBOXD_ERR_IO;
    }

    fprintf(fp, "%s\n", nickname);
    if (fclose(fp) != 0) {
        return MAILBOXD_ERR_IO;
    }

    return MAILBOXD_OK;
}

static void remove_nickname_file(const struct flatfile_state *state,
                                 const char *username)
{
    char path[MAILBOXD_PATH_MAX];

    if (state == NULL || username == NULL || username[0] == '\0') {
        return;
    }

    if (user_nickname_path(state, username, path, sizeof(path)) == MAILBOXD_OK) {
        (void)remove(path);
    }
}

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

static int file_exists(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static mailboxd_result_t user_shard_path(const struct flatfile_state *state,
                                      unsigned shard_index,
                                      char *out, size_t out_len)
{
    char name[32];
    int n;

    if (state == NULL || out == NULL || out_len == 0 || shard_index == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    if (shard_index == 1) {
        n = snprintf(name, sizeof(name), "users.ini");
    } else {
        n = snprintf(name, sizeof(name), "users%u.ini", shard_index);
    }

    if (n < 0 || (size_t)n >= sizeof(name)) {
        return MAILBOXD_ERR_INVALID;
    }

    return mailboxd_path_join(out, out_len, state->users_dir, name);
}

static unsigned last_user_shard_index(const struct flatfile_state *state)
{
    char path[MAILBOXD_PATH_MAX];
    unsigned index;

    for (index = 1; index < 10000u; index++) {
        if (user_shard_path(state, index, path, sizeof(path)) != MAILBOXD_OK) {
            break;
        }
        if (!file_exists(path)) {
            break;
        }
    }

    if (index == 1) {
        return 0;
    }

    return index - 1;
}

static int parse_legacy_user_line(const char *line, mailboxd_user_record_t *out)
{
    unsigned long long id = 0;
    unsigned long long active = 0;
    long long created = 0;
    char username[MAILBOXD_USER_NAME_MAX];
    char level_name[32];
    char full_name[MAILBOXD_USER_FULL_NAME_MAX];
    char country[MAILBOXD_USER_COUNTRY_MAX];
    char location[MAILBOXD_USER_LOCATION_MAX];
    char email[MAILBOXD_USER_EMAIL_MAX];
    char password[MAILBOXD_USER_PASSWORD_MAX];
    int fields;

    if (line == NULL || out == NULL) {
        return 0;
    }

    full_name[0] = '\0';
    country[0] = '\0';
    location[0] = '\0';
    email[0] = '\0';
    password[0] = '\0';

    fields = sscanf(line,
                    "%llu|%63[^|]|%31[^|]|%llu|%lld|"
                    "%127[^|]|%63[^|]|%127[^|]|%127[^|]|%95[^|]",
                    &id, username, level_name, &active, &created,
                    full_name, country, location, email, password);
    if (fields < 5) {
        return 0;
    }

    memset(out, 0, sizeof(*out));
    out->id = (uint64_t)id;
    mailboxd_strlcpy(out->username, username, sizeof(out->username));
    out->level = mailboxd_user_level_parse(level_name);
    out->active = (int)active;
    out->created_at = (time_t)created;

    if (fields >= 9) {
        mailboxd_strlcpy(out->full_name, full_name, sizeof(out->full_name));
        mailboxd_strlcpy(out->country, country, sizeof(out->country));
        mailboxd_strlcpy(out->location, location, sizeof(out->location));
        mailboxd_strlcpy(out->email, email, sizeof(out->email));
    }

    if (fields >= 10) {
        mailboxd_strlcpy(out->password, password, sizeof(out->password));
    }

    return 1;
}

static int parse_user_section(const mailboxd_config_t *cfg, const char *section,
                              mailboxd_user_record_t *out)
{
    const char *value;
    unsigned long long n;

    if (cfg == NULL || section == NULL || out == NULL) {
        return 0;
    }

    if (strncmp(section, MAILBOXD_USER_INI_SECTION_PREFIX,
                strlen(MAILBOXD_USER_INI_SECTION_PREFIX)) != 0) {
        return 0;
    }

    memset(out, 0, sizeof(*out));

    value = mailboxd_config_get(cfg, section, "id", NULL);
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    n = strtoull(value, NULL, 10);
    if (n == 0) {
        return 0;
    }
    out->id = (uint64_t)n;

    value = mailboxd_config_get(cfg, section, "username", NULL);
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    mailboxd_strlcpy(out->username, value, sizeof(out->username));
    mailboxd_username_normalize(out->username);

    value = mailboxd_config_get(cfg, section, "nickname", NULL);
    if (value != NULL) {
        mailboxd_strlcpy(out->nickname, value, sizeof(out->nickname));
    }

    value = mailboxd_config_get(cfg, section, "level", "user");
    out->level = mailboxd_user_level_parse(value);
    out->active = mailboxd_config_get_bool(cfg, section, "active", 0);

    value = mailboxd_config_get(cfg, section, "created", NULL);
    if (value != NULL && value[0] != '\0') {
        out->created_at = (time_t)strtoll(value, NULL, 10);
    }

    value = mailboxd_config_get(cfg, section, "fullname", NULL);
    if (value != NULL) {
        mailboxd_strlcpy(out->full_name, value, sizeof(out->full_name));
    }

    value = mailboxd_config_get(cfg, section, "country", NULL);
    if (value != NULL) {
        mailboxd_strlcpy(out->country, value, sizeof(out->country));
    }

    value = mailboxd_config_get(cfg, section, "location", NULL);
    if (value != NULL) {
        mailboxd_strlcpy(out->location, value, sizeof(out->location));
    }

    value = mailboxd_config_get(cfg, section, "email", NULL);
    if (value != NULL) {
        mailboxd_strlcpy(out->email, value, sizeof(out->email));
    }

    value = mailboxd_config_get(cfg, section, "password", NULL);
    if (value != NULL) {
        mailboxd_strlcpy(out->password, value, sizeof(out->password));
    }

    value = mailboxd_config_get(cfg, section, "last_login", NULL);
    if (value != NULL && value[0] != '\0') {
        out->last_login_at = (time_t)strtoll(value, NULL, 10);
    }

    return 1;
}

typedef struct section_list_ctx {
    char names[MAILBOXD_USERS_PER_FILE][MAILBOXD_CONFIG_SECTION_MAX];
    size_t count;
} section_list_ctx_t;

static void section_list_collect(const char *section, const char *key,
                                 const char *value, void *ctx)
{
    section_list_ctx_t *list = (section_list_ctx_t *)ctx;
    size_t i;

    (void)key;
    (void)value;

    if (section == NULL || list == NULL) {
        return;
    }

    if (strncmp(section, MAILBOXD_USER_INI_SECTION_PREFIX,
                strlen(MAILBOXD_USER_INI_SECTION_PREFIX)) != 0) {
        return;
    }

    for (i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], section) == 0) {
            return;
        }
    }

    if (list->count >= MAILBOXD_USERS_PER_FILE) {
        return;
    }

    mailboxd_strlcpy(list->names[list->count], section,
                  sizeof(list->names[list->count]));
    list->count++;
}

static mailboxd_result_t load_user_shard(const char *path, user_shard_t *shard)
{
    mailboxd_config_t cfg;
    section_list_ctx_t sections;
    size_t i;
    mailboxd_result_t rc;

    if (path == NULL || shard == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(shard, 0, sizeof(*shard));
    memset(&sections, 0, sizeof(sections));

    rc = mailboxd_config_load(&cfg, path);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    mailboxd_config_foreach(&cfg, section_list_collect, &sections);

    for (i = 0; i < sections.count; i++) {
        mailboxd_user_record_t user;

        if (!parse_user_section(&cfg, sections.names[i], &user)) {
            continue;
        }

        if (shard->count >= MAILBOXD_USERS_PER_FILE) {
            mailboxd_config_free(&cfg);
            return MAILBOXD_ERR_IO;
        }

        shard->users[shard->count++] = user;
    }

    mailboxd_config_free(&cfg);
    return MAILBOXD_OK;
}

static mailboxd_result_t save_user_shard(const struct flatfile_state *state,
                                      unsigned shard_index,
                                      const user_shard_t *shard)
{
    char path[MAILBOXD_PATH_MAX];
    FILE *fp;
    size_t i;
    mailboxd_result_t rc;

    if (state == NULL || shard == NULL || shard->count > MAILBOXD_USERS_PER_FILE) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = user_shard_path(state, shard_index, path, sizeof(path));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    fp = fopen(path, "w");
    if (fp == NULL) {
        return MAILBOXD_ERR_IO;
    }

    fprintf(fp, "; MailboxD user file (max %u users)\n",
            (unsigned)MAILBOXD_USERS_PER_FILE);
    fprintf(fp, "; shard %u\n\n", shard_index);

    for (i = 0; i < shard->count; i++) {
        const mailboxd_user_record_t *user = &shard->users[i];

        fprintf(fp, "[user.%llu]\n", (unsigned long long)user->id);
        fprintf(fp, "id = %llu\n", (unsigned long long)user->id);
        fprintf(fp, "username = %s\n", user->username);
        fprintf(fp, "nickname = %s\n", user->nickname);
        fprintf(fp, "level = %s\n", mailboxd_user_level_name(user->level));
        fprintf(fp, "active = %s\n", mailboxd_bool_to_string(user->active));
        fprintf(fp, "created = %lld\n", (long long)user->created_at);
        fprintf(fp, "fullname = %s\n", user->full_name);
        fprintf(fp, "country = %s\n", user->country);
        fprintf(fp, "location = %s\n", user->location);
        fprintf(fp, "email = %s\n", user->email);
        fprintf(fp, "password = %s\n", user->password);
        fprintf(fp, "last_login = %lld\n", (long long)user->last_login_at);

        if (user->nickname[0] != '\0') {
            (void)write_nickname_file(state, user->username, user->nickname);
        }
    }

    if (fclose(fp) != 0) {
        return MAILBOXD_ERR_IO;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t find_user_shard(const struct flatfile_state *state,
                                      uint64_t user_id,
                                      unsigned *shard_index,
                                      size_t *slot_index)
{
    unsigned shard;
    unsigned last;
    char path[MAILBOXD_PATH_MAX];
    user_shard_t loaded;
    size_t i;
    mailboxd_result_t rc;

    if (state == NULL || user_id == 0 || shard_index == NULL ||
        slot_index == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    last = last_user_shard_index(state);

    for (shard = 1; shard <= last; shard++) {
        if (user_shard_path(state, shard, path, sizeof(path)) != MAILBOXD_OK) {
            continue;
        }
        if (!file_exists(path)) {
            continue;
        }

        rc = load_user_shard(path, &loaded);
        if (rc != MAILBOXD_OK) {
            return rc;
        }

        for (i = 0; i < loaded.count; i++) {
            if (loaded.users[i].id == user_id) {
                *shard_index = shard;
                *slot_index = i;
                return MAILBOXD_OK;
            }
        }
    }

    return MAILBOXD_ERR_NOT_FOUND;
}

static mailboxd_result_t append_user_record(struct flatfile_state *state,
                                         const mailboxd_user_record_t *user)
{
    unsigned shard;
    unsigned last;
    char path[MAILBOXD_PATH_MAX];
    user_shard_t loaded;
    mailboxd_result_t rc;

    if (state == NULL || user == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    last = last_user_shard_index(state);
    if (last == 0) {
        shard = 1;
        memset(&loaded, 0, sizeof(loaded));
    } else {
        rc = user_shard_path(state, last, path, sizeof(path));
        if (rc != MAILBOXD_OK) {
            return rc;
        }

        rc = load_user_shard(path, &loaded);
        if (rc != MAILBOXD_OK) {
            return rc;
        }

        if (loaded.count >= MAILBOXD_USERS_PER_FILE) {
            shard = last + 1;
            memset(&loaded, 0, sizeof(loaded));
        } else {
            shard = last;
        }
    }

    if (loaded.count >= MAILBOXD_USERS_PER_FILE) {
        return MAILBOXD_ERR_BUSY;
    }

    loaded.users[loaded.count++] = *user;
    return save_user_shard(state, shard, &loaded);
}

static mailboxd_result_t ensure_default_sysop(mailboxd_storage_t *storage)
{
    struct flatfile_state *state;
    size_t sysop_count = 0;
    mailboxd_user_record_t sysop;
    char plain_password[MAILBOXD_PASSWORD_MAX_LEN + 1];
    uint64_t user_id;
    time_t now = time(NULL);
    mailboxd_result_t rc;

    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_flatfile_count_level(storage, MAILBOXD_LEVEL_SYSOP,
                                            &sysop_count);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (sysop_count > 0) {
        return MAILBOXD_OK;
    }

    rc = bump_counter(state->user_next_path, &user_id);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    memset(&sysop, 0, sizeof(sysop));
    sysop.id = user_id;
    mailboxd_strlcpy(sysop.username, "sysop", sizeof(sysop.username));
    mailboxd_strlcpy(sysop.nickname, MAILBOXD_DEFAULT_SYSOP_USERNAME,
                  sizeof(sysop.nickname));
    sysop.level = MAILBOXD_LEVEL_SYSOP;
    sysop.active = 1;
    sysop.created_at = now;
    mailboxd_strlcpy(sysop.full_name, "System Operator", sizeof(sysop.full_name));
    /* Fixed initial Sysop password, matching PRTERM's built-in admin
     * default ("PRTerm") — change it on first login via /passwd. */
    mailboxd_strlcpy(plain_password, MAILBOXD_SYSOP_INIT_PASSWORD,
                  sizeof(plain_password));
    if (mailboxd_password_hash(plain_password,
                            sysop.password, sizeof(sysop.password)) != MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    rc = append_user_record(state, &sysop);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    mailboxd_log_info("[storage] created default Sysop at %s/users/users.ini (login: %s / %s)",
           storage->path, sysop.nickname, plain_password);
    mailboxd_security_log_write("sysop_created user=%s", sysop.nickname);
    return MAILBOXD_OK;
}

static mailboxd_result_t foreach_user(struct flatfile_state *state,
                                   mailboxd_result_t (*fn)(const mailboxd_user_record_t *user,
                                                        void *ctx),
                                   void *ctx)
{
    unsigned shard;
    unsigned last;
    char path[MAILBOXD_PATH_MAX];
    user_shard_t loaded;
    size_t i;
    mailboxd_result_t rc;

    if (state == NULL || fn == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    last = last_user_shard_index(state);
    for (shard = 1; shard <= last; shard++) {
        rc = user_shard_path(state, shard, path, sizeof(path));
        if (rc != MAILBOXD_OK) {
            return rc;
        }

        rc = load_user_shard(path, &loaded);
        if (rc != MAILBOXD_OK) {
            return rc;
        }

        for (i = 0; i < loaded.count; i++) {
            rc = fn(&loaded.users[i], ctx);
            if (rc != MAILBOXD_OK) {
                return rc;
            }
        }
    }

    return MAILBOXD_OK;
}

typedef struct find_user_ctx {
    const char *username;
    mailboxd_user_record_t *out;
    int found;
} find_user_ctx_t;

static mailboxd_result_t find_user_cb(const mailboxd_user_record_t *user, void *ctx)
{
    find_user_ctx_t *fctx = (find_user_ctx_t *)ctx;

    if (str_ieq(user->username, fctx->username)) {
        *fctx->out = *user;
        fctx->found = 1;
        return MAILBOXD_ERR_BUSY;
    }

    return MAILBOXD_OK;
}

typedef struct find_nickname_ctx {
    const char *nickname;
    mailboxd_user_record_t *out;
    int found;
} find_nickname_ctx_t;

static mailboxd_result_t find_nickname_cb(const mailboxd_user_record_t *user, void *ctx)
{
    find_nickname_ctx_t *fctx = (find_nickname_ctx_t *)ctx;

    if (user->nickname[0] != '\0' && str_ieq(user->nickname, fctx->nickname)) {
        *fctx->out = *user;
        fctx->found = 1;
        return MAILBOXD_ERR_BUSY;
    }

    return MAILBOXD_OK;
}

typedef struct identity_taken_ctx {
    const char *username;
    const char *nickname;
    int taken;
} identity_taken_ctx_t;

static mailboxd_result_t identity_taken_cb(const mailboxd_user_record_t *user,
                                        void *ctx)
{
    identity_taken_ctx_t *ictx = (identity_taken_ctx_t *)ctx;

    if (str_ieq(user->username, ictx->username) ||
        str_ieq(user->username, ictx->nickname) ||
        (user->nickname[0] != '\0' &&
         (str_ieq(user->nickname, ictx->nickname) ||
          str_ieq(user->nickname, ictx->username)))) {
        ictx->taken = 1;
        return MAILBOXD_ERR_BUSY;
    }

    return MAILBOXD_OK;
}

typedef struct count_level_ctx {
    mailboxd_user_level_t level;
    size_t count;
} count_level_ctx_t;

static mailboxd_result_t count_level_cb(const mailboxd_user_record_t *user, void *ctx)
{
    count_level_ctx_t *cctx = (count_level_ctx_t *)ctx;

    if (user->level == cctx->level) {
        cctx->count++;
    }

    return MAILBOXD_OK;
}

typedef struct migrate_passwords_ctx {
    mailboxd_storage_t *storage;
    mailboxd_result_t last_error;
} migrate_passwords_ctx_t;

static mailboxd_result_t migrate_passwords_cb(const mailboxd_user_record_t *user,
                                           void *ctx)
{
    migrate_passwords_ctx_t *mctx = (migrate_passwords_ctx_t *)ctx;
    mailboxd_user_record_t updated;
    mailboxd_result_t rc;

    if (!mailboxd_password_is_plain(user->password)) {
        return MAILBOXD_OK;
    }

    updated = *user;
    rc = mailboxd_password_hash(user->password, updated.password,
                             sizeof(updated.password));
    if (rc != MAILBOXD_OK) {
        mctx->last_error = rc;
        return rc;
    }

    rc = mailboxd_storage_update_user(mctx->storage, &updated);
    if (rc != MAILBOXD_OK) {
        mctx->last_error = rc;
        return rc;
    }

    mailboxd_log_info("[storage] upgraded plain password to sha256 for user %s",
           user->username);
    return MAILBOXD_OK;
}

static mailboxd_result_t migrate_legacy_users_dat(mailboxd_storage_t *storage)
{
    struct flatfile_state *state;
    FILE *fp;
    char line[MAILBOXD_CONFIG_LINE_MAX];
    user_shard_t shard;
    unsigned shard_index = 1;
    mailboxd_result_t rc;

    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (!file_exists(state->legacy_users_path)) {
        return MAILBOXD_OK;
    }

    if (last_user_shard_index(state) > 0) {
        return MAILBOXD_OK;
    }

    fp = fopen(state->legacy_users_path, "r");
    if (fp == NULL) {
        return MAILBOXD_ERR_IO;
    }

    memset(&shard, 0, sizeof(shard));

    while (fgets(line, sizeof(line), fp) != NULL) {
        mailboxd_user_record_t user;
        char *nl;

        nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }

        if (!parse_legacy_user_line(line, &user)) {
            continue;
        }

        if (shard.count >= MAILBOXD_USERS_PER_FILE) {
            rc = save_user_shard(state, shard_index, &shard);
            if (rc != MAILBOXD_OK) {
                fclose(fp);
                return rc;
            }
            shard_index++;
            memset(&shard, 0, sizeof(shard));
        }

        shard.users[shard.count++] = user;
    }

    fclose(fp);

    if (shard.count > 0) {
        rc = save_user_shard(state, shard_index, &shard);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    {
        char backup[MAILBOXD_PATH_MAX];

        if (mailboxd_path_join(backup, sizeof(backup), storage->path,
                            "users.dat.migrated") == MAILBOXD_OK) {
            (void)rename(state->legacy_users_path, backup);
        }
    }

    mailboxd_log_info("[storage] migrated legacy users.dat to INI user files");
    return MAILBOXD_OK;
}

typedef struct migrate_identities_ctx {
    mailboxd_storage_t *storage;
    struct flatfile_state *state;
    mailboxd_result_t last_error;
} migrate_identities_ctx_t;

static mailboxd_result_t migrate_identities_cb(const mailboxd_user_record_t *user,
                                              void *ctx)
{
    migrate_identities_ctx_t *mctx = (migrate_identities_ctx_t *)ctx;
    mailboxd_user_record_t updated;
    char stored_username[MAILBOXD_USER_NAME_MAX];
    int changed = 0;
    mailboxd_result_t rc;

    if (user == NULL || mctx == NULL) {
        return MAILBOXD_OK;
    }

    updated = *user;
    mailboxd_strlcpy(stored_username, updated.username, sizeof(stored_username));

    mailboxd_username_normalize(updated.username);
    if (!str_ieq(stored_username, updated.username)) {
        changed = 1;
    }

    if (updated.nickname[0] == '\0') {
        if (read_nickname_file(mctx->state, updated.username, updated.nickname,
                               sizeof(updated.nickname)) != MAILBOXD_OK) {
            mailboxd_nickname_infer(stored_username, updated.nickname,
                                 sizeof(updated.nickname));
        }
        changed = 1;
    }

    if (!changed) {
        (void)write_nickname_file(mctx->state, updated.username,
                                  updated.nickname);
        return MAILBOXD_OK;
    }

    rc = mailboxd_storage_flatfile_update_user(mctx->storage, &updated);
    if (rc != MAILBOXD_OK) {
        mctx->last_error = rc;
        return rc;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t migrate_user_identities(mailboxd_storage_t *storage)
{
    struct flatfile_state *state;
    migrate_identities_ctx_t ctx;

    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.storage = storage;
    ctx.state = state;
    ctx.last_error = MAILBOXD_OK;
    return foreach_user(state, migrate_identities_cb, &ctx);
}

static mailboxd_result_t migrate_plain_passwords(mailboxd_storage_t *storage)
{
    struct flatfile_state *state;
    migrate_passwords_ctx_t ctx;

    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    ctx.storage = storage;
    ctx.last_error = MAILBOXD_OK;
    return foreach_user(state, migrate_passwords_cb, &ctx);
}

mailboxd_result_t mailboxd_storage_flatfile_open(mailboxd_storage_t *storage)
{
    struct flatfile_state *state;
    mailboxd_result_t rc;

    if (storage == NULL || storage->path == NULL) {
        return MAILBOXD_ERR_INVALID;
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

    rc = mailboxd_path_join(state->users_dir, sizeof(state->users_dir),
                         storage->path, "users");
    if (rc != MAILBOXD_OK) {
        free(state);
        return rc;
    }

    if (mkdir_p(state->users_dir) != 0) {
        free(state);
        return MAILBOXD_ERR_IO;
    }

    rc = mailboxd_path_join(state->legacy_users_path, sizeof(state->legacy_users_path),
                         storage->path, "users.dat");
    if (rc != MAILBOXD_OK) {
        free(state);
        return rc;
    }

    rc = mailboxd_path_join(state->sessions_path, sizeof(state->sessions_path),
                   storage->path, "sessions.dat");
    if (rc != MAILBOXD_OK) {
        free(state);
        return rc;
    }

    rc = mailboxd_path_join(state->session_next_path, sizeof(state->session_next_path),
                   storage->path, "session.next");
    if (rc != MAILBOXD_OK) {
        free(state);
        return rc;
    }

    rc = mailboxd_path_join(state->user_next_path, sizeof(state->user_next_path),
                   storage->path, "user.next");
    if (rc != MAILBOXD_OK) {
        free(state);
        return rc;
    }

    storage->backend_data = state;

    rc = migrate_legacy_users_dat(storage);
    if (rc != MAILBOXD_OK) {
        mailboxd_storage_flatfile_close(storage);
        return rc;
    }

    rc = ensure_default_sysop(storage);
    if (rc != MAILBOXD_OK) {
        mailboxd_storage_flatfile_close(storage);
        return rc;
    }

    rc = migrate_user_identities(storage);
    if (rc != MAILBOXD_OK) {
        mailboxd_storage_flatfile_close(storage);
        return rc;
    }

    rc = migrate_plain_passwords(storage);
    if (rc != MAILBOXD_OK) {
        mailboxd_storage_flatfile_close(storage);
        return rc;
    }

    return MAILBOXD_OK;
}

void mailboxd_storage_flatfile_close(mailboxd_storage_t *storage)
{
    if (storage == NULL) {
        return;
    }

    free(storage->backend_data);
    storage->backend_data = NULL;
}

mailboxd_result_t mailboxd_storage_flatfile_find_user(mailboxd_storage_t *storage,
                                                const char *username,
                                                mailboxd_user_record_t *out)
{
    struct flatfile_state *state;
    find_user_ctx_t ctx;
    mailboxd_result_t rc;

    if (storage == NULL || username == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    ctx.username = username;
    ctx.out = out;
    ctx.found = 0;

    rc = foreach_user(state, find_user_cb, &ctx);
    if (rc == MAILBOXD_ERR_BUSY) {
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    return MAILBOXD_ERR_NOT_FOUND;
}

mailboxd_result_t mailboxd_storage_flatfile_resolve_user(mailboxd_storage_t *storage,
                                                   const char *name,
                                                   mailboxd_user_record_t *out)
{
    char normalized[MAILBOXD_USER_NAME_MAX];
    struct flatfile_state *state;
    find_nickname_ctx_t nctx;
    mailboxd_result_t rc;

    if (storage == NULL || name == NULL || out == NULL || name[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    mailboxd_strlcpy(normalized, name, sizeof(normalized));
    mailboxd_username_normalize(normalized);

    rc = mailboxd_storage_flatfile_find_user(storage, normalized, out);
    if (rc == MAILBOXD_OK) {
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_ERR_NOT_FOUND) {
        return rc;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    nctx.nickname = name;
    nctx.out = out;
    nctx.found = 0;

    rc = foreach_user(state, find_nickname_cb, &nctx);
    if (rc == MAILBOXD_ERR_BUSY) {
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    return MAILBOXD_ERR_NOT_FOUND;
}

mailboxd_result_t mailboxd_storage_flatfile_count_level(mailboxd_storage_t *storage,
                                                  mailboxd_user_level_t level,
                                                  size_t *count)
{
    struct flatfile_state *state;
    count_level_ctx_t ctx;
    mailboxd_result_t rc;

    if (storage == NULL || count == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    ctx.level = level;
    ctx.count = 0;

    rc = foreach_user(state, count_level_cb, &ctx);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    *count = ctx.count;
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_storage_flatfile_foreach_user(mailboxd_storage_t *storage,
                                                   mailboxd_storage_user_fn fn,
                                                   void *ctx)
{
    struct flatfile_state *state;

    if (storage == NULL || fn == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    return foreach_user(state, fn, ctx);
}

mailboxd_result_t mailboxd_storage_flatfile_register_user(mailboxd_storage_t *storage,
                                                    const mailboxd_user_registration_t *reg,
                                                    mailboxd_user_record_t *out)
{
    struct flatfile_state *state;
    mailboxd_user_record_t existing;
    uint64_t user_id;
    time_t now = time(NULL);
    mailboxd_result_t rc;

    if (storage == NULL || reg == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (!mailboxd_registration_valid(reg, storage->guest_prefix)) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_flatfile_find_user(storage, reg->username, &existing);
    if (rc == MAILBOXD_OK) {
        return MAILBOXD_ERR_BUSY;
    }
    if (rc != MAILBOXD_ERR_NOT_FOUND) {
        return rc;
    }

    {
        identity_taken_ctx_t taken;

        memset(&taken, 0, sizeof(taken));
        taken.username = reg->username;
        taken.nickname = reg->nickname;

        rc = foreach_user(state, identity_taken_cb, &taken);
        if (rc == MAILBOXD_ERR_BUSY || taken.taken) {
            return MAILBOXD_ERR_BUSY;
        }
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    rc = bump_counter(state->user_next_path, &user_id);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    memset(out, 0, sizeof(*out));
    out->id = user_id;
    mailboxd_strlcpy(out->username, reg->username, sizeof(out->username));
    mailboxd_username_normalize(out->username);
    mailboxd_strlcpy(out->nickname, reg->nickname, sizeof(out->nickname));
    out->level = MAILBOXD_LEVEL_USER;
    out->active = 0;
    out->created_at = now;
    mailboxd_strlcpy(out->full_name, reg->full_name, sizeof(out->full_name));
    mailboxd_strlcpy(out->country, reg->country, sizeof(out->country));
    mailboxd_strlcpy(out->location, reg->location, sizeof(out->location));
    mailboxd_strlcpy(out->email, reg->email, sizeof(out->email));

    if (reg->password[0] != '\0') {
        if (!mailboxd_password_plain_valid(reg->password)) {
            return MAILBOXD_ERR_INVALID;
        }
        rc = mailboxd_password_hash(reg->password, out->password,
                                 sizeof(out->password));
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    return append_user_record(state, out);
}

mailboxd_result_t mailboxd_storage_flatfile_update_user(mailboxd_storage_t *storage,
                                                  const mailboxd_user_record_t *user)
{
    struct flatfile_state *state;
    unsigned shard_index;
    size_t slot_index;
    char path[MAILBOXD_PATH_MAX];
    user_shard_t shard;
    mailboxd_result_t rc;

    if (storage == NULL || user == NULL || user->id == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = find_user_shard(state, user->id, &shard_index, &slot_index);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = user_shard_path(state, shard_index, path, sizeof(path));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = load_user_shard(path, &shard);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (slot_index >= shard.count) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    shard.users[slot_index] = *user;
    mailboxd_username_normalize(shard.users[slot_index].username);
    return save_user_shard(state, shard_index, &shard);
}

mailboxd_result_t mailboxd_storage_flatfile_delete_user(mailboxd_storage_t *storage,
                                                  uint64_t user_id)
{
    struct flatfile_state *state;
    unsigned shard_index;
    size_t slot_index;
    char path[MAILBOXD_PATH_MAX];
    user_shard_t shard;
    mailboxd_result_t rc;

    if (storage == NULL || user_id == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    state = storage->backend_data;
    if (state == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = find_user_shard(state, user_id, &shard_index, &slot_index);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = user_shard_path(state, shard_index, path, sizeof(path));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = load_user_shard(path, &shard);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (slot_index >= shard.count) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    remove_nickname_file(state, shard.users[slot_index].username);

    if (slot_index + 1 < shard.count) {
        memmove(&shard.users[slot_index], &shard.users[slot_index + 1],
                (shard.count - slot_index - 1) * sizeof(shard.users[0]));
    }
    shard.count--;

    return save_user_shard(state, shard_index, &shard);
}

mailboxd_result_t mailboxd_storage_flatfile_session_begin(mailboxd_storage_t *storage,
                                                    const mailboxd_user_record_t *user,
                                                    const char *transport,
                                                    mailboxd_session_record_t *out)
{
    struct flatfile_state *state = storage->backend_data;
    char line[384];
    time_t now = time(NULL);
    mailboxd_result_t rc;

    if (state == NULL || user == NULL || transport == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    rc = bump_counter(state->session_next_path, &out->session_id);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    out->user_id = user->id;
    mailboxd_strlcpy(out->username, user->username, sizeof(out->username));
    mailboxd_strlcpy(out->transport, transport, sizeof(out->transport));
    out->connected_at = now;
    out->active = 1;

    snprintf(line, sizeof(line), "%llu|%llu|%s|%s|%lld|0|1",
             (unsigned long long)out->session_id,
             (unsigned long long)out->user_id,
             out->username, out->transport, (long long)out->connected_at);

    return append_line(state->sessions_path, line);
}

mailboxd_result_t mailboxd_storage_flatfile_session_end(mailboxd_storage_t *storage,
                                                  uint64_t session_id)
{
    struct flatfile_state *state = storage->backend_data;
    char line[384];
    time_t now = time(NULL);

    if (state == NULL || session_id == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    snprintf(line, sizeof(line), "%llu|0|_|_|0|%lld|0",
             (unsigned long long)session_id, (long long)now);

    return append_line(state->sessions_path, line);
}
