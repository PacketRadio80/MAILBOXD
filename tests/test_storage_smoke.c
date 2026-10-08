/*
 * test_storage_smoke.c — P0.4: flatfile + SQLite core smoke
 *
 * Tests: open/close, register_user, find_user, resolve_user,
 *        count_level, update_user, delete_user, session_begin/end.
 * Runs against both flatfile and SQLite backends in temporary directories.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mailboxd/storage.h"
#include "mailboxd/auth.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned g_failures;

static void check_rc(const char *label, mailboxd_result_t got, mailboxd_result_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", label, (int)got, (int)want);
        g_failures++;
    }
}

static void check_str(const char *label, const char *got, const char *want)
{
    if (got == NULL || want == NULL || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got '%s' want '%s'\n",
                label, got != NULL ? got : "(null)", want);
        g_failures++;
    }
}

static void check_uint64(const char *label, uint64_t got, uint64_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %llu want %llu\n",
                label, (unsigned long long)got, (unsigned long long)want);
        g_failures++;
    }
}

static void check_nonzero(const char *label, uint64_t val)
{
    if (val == 0) {
        fprintf(stderr, "FAIL %s: expected non-zero\n", label);
        g_failures++;
    }
}

static void mkdir_p(const char *path)
{
    char tmp[512];
    char *p;
    size_t len;

    mailboxd_strlcpy(tmp, path, sizeof(tmp));
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void rm_rf(const char *path)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static mailboxd_storage_t *open_backend(mailboxd_storage_backend_kind_t backend,
                                     const char *path)
{
    mailboxd_storage_options_t opts;

    memset(&opts, 0, sizeof(opts));
    opts.backend = backend;
    opts.path = path;
    return mailboxd_storage_open(&opts);
}

/*
 * Run the full CRUD + session smoke against one backend.
 */
static void smoke_backend(mailboxd_storage_backend_kind_t backend,
                          const char *label,
                          const char *path)
{
    mailboxd_storage_t *st;
    mailboxd_user_registration_t reg;
    mailboxd_user_record_t user;
    mailboxd_user_record_t found;
    mailboxd_session_record_t sess;
    size_t count;
    char test_user[64];

    snprintf(test_user, sizeof(test_user), "smk_%s", label);

    rm_rf(path);
    mkdir_p(path);

    st = open_backend(backend, path);
    if (st == NULL) {
        fprintf(stderr, "FAIL %s: storage_open returned NULL\n", label);
        g_failures++;
        return;
    }

    check_uint64("backend kind",
                 (uint64_t)mailboxd_storage_backend(st), (uint64_t)backend);

    /* register_user — level defaults to MAILBOXD_LEVEL_USER */
    memset(&reg, 0, sizeof(reg));
    mailboxd_strlcpy(reg.username, test_user, sizeof(reg.username));
    mailboxd_strlcpy(reg.password, "testpass123", sizeof(reg.password));
    mailboxd_strlcpy(reg.nickname, test_user, sizeof(reg.nickname));
    mailboxd_strlcpy(reg.full_name, "Smoke Test", sizeof(reg.full_name));
    mailboxd_strlcpy(reg.country, "DE", sizeof(reg.country));
    mailboxd_strlcpy(reg.location, "localhost", sizeof(reg.location));
    mailboxd_strlcpy(reg.email, "smoke@test.local", sizeof(reg.email));

    check_rc("register_user",
             mailboxd_storage_register_user(st, &reg, &user), MAILBOXD_OK);
    check_nonzero("user id", user.id);
    check_str("user username", user.username, test_user);
    check_uint64("user level", (uint64_t)user.level,
                 (uint64_t)MAILBOXD_LEVEL_USER);

    /* find_user */
    memset(&found, 0, sizeof(found));
    check_rc("find_user",
             mailboxd_storage_find_user(st, test_user, &found), MAILBOXD_OK);
    check_uint64("found id matches", found.id, user.id);
    check_str("found username", found.username, test_user);

    /* find_user nonexistent */
    check_rc("find_user missing",
             mailboxd_storage_find_user(st, "no_such_user", &found),
             MAILBOXD_ERR_NOT_FOUND);

    /* resolve_user (by name) */
    memset(&found, 0, sizeof(found));
    check_rc("resolve_user",
             mailboxd_storage_resolve_user(st, test_user, &found), MAILBOXD_OK);
    check_uint64("resolve id matches", found.id, user.id);

    /* count_level */
    count = 0;
    check_rc("count_level",
             mailboxd_storage_count_level(st, MAILBOXD_LEVEL_USER, &count),
             MAILBOXD_OK);
    /* count >= 1 (our user) */

    /* update_user */
    user.level = MAILBOXD_LEVEL_SYSOP;
    check_rc("update_user",
             mailboxd_storage_update_user(st, &user), MAILBOXD_OK);

    memset(&found, 0, sizeof(found));
    check_rc("find after update",
             mailboxd_storage_find_user(st, test_user, &found), MAILBOXD_OK);
    check_uint64("updated level", (uint64_t)found.level,
                 (uint64_t)MAILBOXD_LEVEL_SYSOP);

    /* session_begin */
    memset(&sess, 0, sizeof(sess));
    check_rc("session_begin",
             mailboxd_storage_session_begin(st, &user, "test", &sess), MAILBOXD_OK);
    check_nonzero("session id", sess.session_id);

    /* session_end */
    check_rc("session_end",
             mailboxd_storage_session_end(st, sess.session_id), MAILBOXD_OK);

    /* delete_user */
    check_rc("delete_user",
             mailboxd_storage_delete_user(st, user.id), MAILBOXD_OK);

    /* find after delete */
    check_rc("find after delete",
             mailboxd_storage_find_user(st, test_user, &found),
             MAILBOXD_ERR_NOT_FOUND);

    /* Persistence: close and reopen, verify functional */
    mailboxd_storage_close(st);
    st = open_backend(backend, path);
    if (st == NULL) {
        fprintf(stderr, "FAIL %s: reopen returned NULL\n", label);
        g_failures++;
        return;
    }

    /* Re-register after reopen to verify backend is functional */
    check_rc("re-register after reopen",
             mailboxd_storage_register_user(st, &reg, &user), MAILBOXD_OK);
    check_nonzero("re-registered id", user.id);

    mailboxd_storage_delete_user(st, user.id);
    mailboxd_storage_close(st);

    rm_rf(path);
}

int main(void)
{
    char path_ff[256];
    char path_sql[256];

    snprintf(path_ff, sizeof(path_ff), "/tmp/mailboxd-smoke-flatfile-%d",
             (int)getpid());
    snprintf(path_sql, sizeof(path_sql), "/tmp/mailboxd-smoke-sqlite-%d",
             (int)getpid());

    smoke_backend(MAILBOXD_STORAGE_FLATFILE, "flatfile", path_ff);
    smoke_backend(MAILBOXD_STORAGE_SQLITE, "sqlite", path_sql);

    if (g_failures != 0) {
        fprintf(stderr, "%u failure(s)\n", g_failures);
        return 1;
    }

    puts("test_storage_smoke: ok");
    return 0;
}
