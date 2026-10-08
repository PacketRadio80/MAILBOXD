/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include "mailboxd/security.h"
#if !defined(MAILBOXD_CLIENT_BUILD)
#include "mailboxd/config.h"
#endif
#include "mailboxd/limits.h"
#include "mailboxd/util.h"
#include "mailboxd/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MAILBOXD_SECURITY_LINE_MAX 512u

static char g_security_dir[MAILBOXD_PATH_MAX];
static int g_security_ready;
static FILE *g_security_file;
static pthread_mutex_t g_security_lock = PTHREAD_MUTEX_INITIALIZER;

static int mkdir_p(const char *path)
{
    char buf[MAILBOXD_PATH_MAX];
    size_t len;
    size_t i;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    mailboxd_strlcpy(buf, path, sizeof(buf));
    len = strlen(buf);
    while (len > 0 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    for (i = 1; i < len; i++) {
        if (buf[i] != '/') {
            continue;
        }
        buf[i] = '\0';
        if (buf[0] != '\0' && mkdir(buf, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
        buf[i] = '/';
    }

    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static int security_open_file(void)
{
    char path[MAILBOXD_PATH_MAX];

    if (g_security_dir[0] == '\0') {
        return -1;
    }

    if (g_security_file != NULL) {
        return 0;
    }

    if (mkdir_p(g_security_dir) != 0) {
        mailboxd_log_warn("[security] cannot create directory %s",
                g_security_dir);
        return -1;
    }

    if (mailboxd_path_join(path, sizeof(path), g_security_dir,
                        MAILBOXD_SECURITY_LOG_FILE) != MAILBOXD_OK) {
        return -1;
    }

    g_security_file = fopen(path, "a");
    if (g_security_file == NULL) {
        mailboxd_log_warn("[security] cannot open %s", path);
        return -1;
    }

    return 0;
}

#if !defined(MAILBOXD_CLIENT_BUILD)
void mailboxd_security_log_config_apply(const struct mailboxd_config *config)
{
    const char *dir_raw;

    mailboxd_security_log_shutdown();
    g_security_dir[0] = '\0';
    g_security_ready = 0;

    if (config != NULL) {
        dir_raw = mailboxd_config_get(config, "log", "dir", NULL);
        if (dir_raw != NULL && dir_raw[0] != '\0') {
            if (mailboxd_path_resolve(g_security_dir, sizeof(g_security_dir),
                                   dir_raw) != MAILBOXD_OK) {
                mailboxd_log_warn("[security] invalid log dir path");
                return;
            }
        } else {
            if (mailboxd_path_resolve(g_security_dir, sizeof(g_security_dir),
                                   MAILBOXD_DIR_LOGS) != MAILBOXD_OK) {
                mailboxd_log_warn("[security] cannot resolve default log dir");
                return;
            }
        }
    } else if (mailboxd_path_resolve(g_security_dir, sizeof(g_security_dir),
                                    MAILBOXD_DIR_LOGS) != MAILBOXD_OK) {
        return;
    }

    g_security_ready = 1;

    if (security_open_file() != 0) {
        g_security_ready = 0;
        return;
    }

    mailboxd_log_info("[security] log=%s/%s", g_security_dir, MAILBOXD_SECURITY_LOG_FILE);
    mailboxd_security_log_write("startup");
}
#else
void mailboxd_security_log_config_apply(const struct mailboxd_config *config)
{
    (void)config;
}
#endif

void mailboxd_security_log_write(const char *fmt, ...)
{
    char message[MAILBOXD_SECURITY_LINE_MAX];
    char line[MAILBOXD_SECURITY_LINE_MAX + 64];
    char stamp[32];
    va_list ap;
    time_t now;
    struct tm tm_buf;
    struct tm *tm;

    if (!g_security_ready || fmt == NULL) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    now = time(NULL);
    tm = localtime_r(&now, &tm_buf);
    if (tm == NULL) {
        return;
    }

    if (strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm) == 0) {
        return;
    }

    snprintf(line, sizeof(line), "%s %s\n", stamp, message);

    pthread_mutex_lock(&g_security_lock);

    if (security_open_file() == 0) {
        fputs(line, g_security_file);
        fflush(g_security_file);
    }

    pthread_mutex_unlock(&g_security_lock);
}

mailboxd_result_t mailboxd_security_log_current_path(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    out[0] = '\0';
    if (!g_security_ready || g_security_dir[0] == '\0') {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    return mailboxd_path_join(out, out_len, g_security_dir, MAILBOXD_SECURITY_LOG_FILE);
}

void mailboxd_security_log_shutdown(void)
{
    pthread_mutex_lock(&g_security_lock);

    if (g_security_file != NULL) {
        fclose(g_security_file);
        g_security_file = NULL;
    }

    pthread_mutex_unlock(&g_security_lock);
    g_security_ready = 0;
}
