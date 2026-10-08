/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include "mailboxd/log.h"
#if !defined(MAILBOXD_CLIENT_BUILD)
#include "mailboxd/config.h"
#endif
#include "mailboxd/limits.h"
#include "mailboxd/util.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAILBOXD_LOG_LINE_MAX 1024u

/*
 * Weekly MailboxD file log (replaces stuck month-keyed "daily" open handle).
 *
 * Naming: YYYYMMDD-week-mailboxd.log
 *   YYYYMMDD = ISO week start (Monday, local time). One file per 7-day week.
 *
 * Cycle (forever):
 *   Days 1–7  → append to the current week file.
 *   Day 8     → switch to the new week file (new Monday), then pack every
 *               closed/legacy packable log under [log] dir into
 *               logs/arc/<YYYYMMDD>-week-mailboxd.tar.bz2 (bzip2) and delete
 *               those sources only after tar succeeds.
 *   Pack batches of up to 7 members per archive (oldest first); repeat until
 *   fewer than 1 packable remain. Legacy daily names (YYYYMMDD-mailboxd.log)
 *   are packable and are swept on day-8 switch (and on startup when ≥7).
 *
 * security.log is never packed or deleted here.
 */

#define MAILBOXD_LOG_ARC_SUBDIR "arc"
#define MAILBOXD_LOG_PACK_BATCH 7
#define MAILBOXD_LOG_PACKABLE_MAX 64

static mailboxd_log_config_t g_log_config;
static int g_log_ready;
static FILE *g_log_file;
/* ISO-Monday yyyymmdd of the file currently open (0 = none). */
static int g_log_open_week_yyyymmdd;
static char g_log_open_name[64];
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

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

void mailboxd_log_config_defaults(mailboxd_log_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    cfg->enabled = 1;
    cfg->dir[0] = '\0';
    cfg->level = MAILBOXD_LOG_WARN;
}

mailboxd_log_level_t mailboxd_log_parse_level(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return MAILBOXD_LOG_WARN;
    }

    if (strcasecmp(value, "debug") == 0) {
        return MAILBOXD_LOG_DEBUG;
    }
    if (strcasecmp(value, "stats") == 0) {
        return MAILBOXD_LOG_STATS;
    }
    if (strcasecmp(value, "info") == 0) {
        return MAILBOXD_LOG_INFO;
    }
    if (strcasecmp(value, "warn") == 0) {
        return MAILBOXD_LOG_WARN;
    }

    return MAILBOXD_LOG_WARN;
}

const char *mailboxd_log_level_name(mailboxd_log_level_t level)
{
    switch (level) {
    case MAILBOXD_LOG_DEBUG:
        return "debug";
    case MAILBOXD_LOG_STATS:
        return "stats";
    case MAILBOXD_LOG_INFO:
        return "info";
    case MAILBOXD_LOG_WARN:
        return "warn";
    default:
        return "?";
    }
}

int mailboxd_log_level_visible(mailboxd_log_level_t level)
{
    if (!g_log_ready) {
        return 1;
    }

    return level >= g_log_config.level;
}

const mailboxd_log_config_t *mailboxd_log_config_get(void)
{
    return g_log_ready ? &g_log_config : NULL;
}

int mailboxd_log_enabled(void)
{
    return g_log_ready && g_log_config.enabled && g_log_file != NULL;
}

static void log_close_file(void)
{
    if (g_log_file != NULL) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    g_log_open_week_yyyymmdd = 0;
    g_log_open_name[0] = '\0';
}

/* Monday of the ISO week containing @p tm (local wall calendar). */
static int log_week_start_tm(const struct tm *tm, struct tm *out)
{
    time_t t;
    int from_monday;

    if (tm == NULL || out == NULL) {
        return -1;
    }

    *out = *tm;
    /* tm_wday: 0=Sun … 6=Sat → days since Monday */
    from_monday = (out->tm_wday + 6) % 7;
    out->tm_mday -= from_monday;
    out->tm_hour = 12; /* avoid DST midnight edge when normalizing */
    out->tm_min = 0;
    out->tm_sec = 0;
    out->tm_isdst = -1;
    t = mktime(out);
    if (t == (time_t)-1) {
        return -1;
    }
    if (localtime_r(&t, out) == NULL) {
        return -1;
    }
    return 0;
}

static int log_week_yyyymmdd(const struct tm *tm)
{
    struct tm week;

    if (log_week_start_tm(tm, &week) != 0) {
        return 0;
    }
    return (week.tm_year + 1900) * 10000
         + (week.tm_mon + 1) * 100
         + week.tm_mday;
}

static void log_week_basename(int week_yyyymmdd, char *name, size_t name_len)
{
    snprintf(name, name_len, "%08d-week-mailboxd.log", week_yyyymmdd);
}

static int log_build_path_for_week(char *out, size_t out_len, int week_yyyymmdd)
{
    char name[64];

    log_week_basename(week_yyyymmdd, name, sizeof(name));
    return mailboxd_path_join(out, out_len, g_log_config.dir, name) == MAILBOXD_OK
               ? 0
               : -1;
}

static int log_is_eight_digits(const char *s)
{
    size_t i;

    for (i = 0; i < 8; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }
    return 1;
}

/*
 * Packable: current-scheme week files, or legacy daily YYYYMMDD-mailboxd.log.
 * Never security.log, arc/, or the active week file.
 */
static int log_name_is_packable(const char *name)
{
    size_t len;

    if (name == NULL || name[0] == '\0' || name[0] == '.') {
        return 0;
    }
    if (strcmp(name, g_log_open_name) == 0) {
        return 0;
    }

    len = strlen(name);
    if (len == 22 && log_is_eight_digits(name) &&
        strcmp(name + 8, "-week-mailboxd.log") == 0) {
        return 1;
    }
    /* Legacy daily: 20260720-mailboxd.log (8 digits + 10). */
    if (len == 18 && log_is_eight_digits(name) &&
        strcmp(name + 8, "-mailboxd.log") == 0) {
        return 1;
    }
    return 0;
}

static int log_cmp_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int log_run_tar_bz2(const char *arc_path, char **basenames, int count)
{
    pid_t pid;
    int status;
    int i;
    char *argv[MAILBOXD_LOG_PACKABLE_MAX + 8];
    int argc = 0;

    if (arc_path == NULL || basenames == NULL || count <= 0 ||
        count > MAILBOXD_LOG_PACKABLE_MAX) {
        return -1;
    }

    argv[argc++] = "tar";
    argv[argc++] = "-C";
    argv[argc++] = (char *)g_log_config.dir;
    argv[argc++] = "-cjf";
    argv[argc++] = (char *)arc_path;
    for (i = 0; i < count; i++) {
        argv[argc++] = basenames[i];
    }
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp("tar", argv);
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

static int log_collect_packable(char namebuf[][64], char **names, int max)
{
    DIR *dir;
    struct dirent *ent;
    int count = 0;

    dir = opendir(g_log_config.dir);
    if (dir == NULL) {
        return 0;
    }

    while ((ent = readdir(dir)) != NULL && count < max) {
        if (!log_name_is_packable(ent->d_name)) {
            continue;
        }
        if (strlen(ent->d_name) >= 64) {
            continue;
        }
        mailboxd_strlcpy(namebuf[count], ent->d_name, 64);
        names[count] = namebuf[count];
        count++;
    }
    closedir(dir);
    return count;
}

/* Pack up to MAILBOXD_LOG_PACK_BATCH oldest packable files. Returns packed count. */
static int log_archive_one_batch(void)
{
    char *names[MAILBOXD_LOG_PACKABLE_MAX];
    char namebuf[MAILBOXD_LOG_PACKABLE_MAX][64];
    int count;
    int pack_n;
    int i;
    char arc_dir[MAILBOXD_PATH_MAX];
    char arc_path[MAILBOXD_PATH_MAX];
    char arc_name[80];
    struct stat st;

    count = log_collect_packable(namebuf, names, MAILBOXD_LOG_PACKABLE_MAX);
    if (count <= 0) {
        return 0;
    }

    qsort(names, (size_t)count, sizeof(names[0]), log_cmp_names);
    pack_n = count < MAILBOXD_LOG_PACK_BATCH ? count : MAILBOXD_LOG_PACK_BATCH;

    if (mailboxd_path_join(arc_dir, sizeof(arc_dir), g_log_config.dir,
                        MAILBOXD_LOG_ARC_SUBDIR) != MAILBOXD_OK) {
        fprintf(stderr, "[log] archive path too long\n");
        return -1;
    }
    if (mkdir_p(arc_dir) != 0) {
        fprintf(stderr, "[log] cannot create %s\n", arc_dir);
        return -1;
    }

    snprintf(arc_name, sizeof(arc_name), "%.8s-week-mailboxd.tar.bz2", names[0]);
    if (mailboxd_path_join(arc_path, sizeof(arc_path), arc_dir, arc_name) !=
        MAILBOXD_OK) {
        fprintf(stderr, "[log] archive file path too long\n");
        return -1;
    }
    if (stat(arc_path, &st) == 0) {
        snprintf(arc_name, sizeof(arc_name),
                 "%.8s-week-mailboxd-%ld.tar.bz2", names[0], (long)time(NULL));
        if (mailboxd_path_join(arc_path, sizeof(arc_path), arc_dir, arc_name) !=
            MAILBOXD_OK) {
            return -1;
        }
    }

    if (log_run_tar_bz2(arc_path, names, pack_n) != 0) {
        fprintf(stderr, "[log] tar.bz2 failed for %s — sources kept\n",
                arc_path);
        return -1;
    }

    for (i = 0; i < pack_n; i++) {
        char full[MAILBOXD_PATH_MAX];
        if (mailboxd_path_join(full, sizeof(full), g_log_config.dir, names[i]) !=
            MAILBOXD_OK) {
            continue;
        }
        if (unlink(full) != 0) {
            fprintf(stderr, "[log] cannot remove packed %s\n", full);
        }
    }

    fprintf(stderr, "[log] archived %d file(s) → %s\n", pack_n, arc_path);
    return pack_n;
}

/*
 * @p on_week_switch: day-8 path — pack every closed/legacy file (batches of 7).
 * Otherwise (startup): pack only when ≥7 packable (legacy daily sweep).
 */
static void log_archive_packable(int on_week_switch)
{
    char *names[MAILBOXD_LOG_PACKABLE_MAX];
    char namebuf[MAILBOXD_LOG_PACKABLE_MAX][64];
    int count;
    int guard;

    if (g_log_config.dir[0] == '\0') {
        return;
    }

    count = log_collect_packable(namebuf, names, MAILBOXD_LOG_PACKABLE_MAX);
    if (count <= 0) {
        return;
    }
    if (!on_week_switch && count < MAILBOXD_LOG_PACK_BATCH) {
        return;
    }

    for (guard = 0; guard < MAILBOXD_LOG_PACKABLE_MAX; guard++) {
        int packed = log_archive_one_batch();
        if (packed <= 0) {
            break;
        }
        count = log_collect_packable(namebuf, names, MAILBOXD_LOG_PACKABLE_MAX);
        if (count <= 0) {
            break;
        }
        if (!on_week_switch && count < MAILBOXD_LOG_PACK_BATCH) {
            break;
        }
    }
}

mailboxd_result_t mailboxd_log_current_path(char *out, size_t out_len)
{
    time_t now;
    struct tm tm_buf;
    struct tm *tm;
    int week;

    if (out == NULL || out_len == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    out[0] = '\0';
    if (!g_log_ready || !g_log_config.enabled || g_log_config.dir[0] == '\0') {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    now = time(NULL);
    tm = localtime_r(&now, &tm_buf);
    if (tm == NULL) {
        return MAILBOXD_ERR_IO;
    }

    week = log_week_yyyymmdd(tm);
    if (week == 0 || log_build_path_for_week(out, out_len, week) != 0) {
        return MAILBOXD_ERR_IO;
    }

    return MAILBOXD_OK;
}

static int log_open_for_time(const struct tm *tm)
{
    char path[MAILBOXD_PATH_MAX];
    FILE *fp;
    int week;
    int rotated = 0;

    if (tm == NULL) {
        return -1;
    }

    week = log_week_yyyymmdd(tm);
    if (week == 0) {
        return -1;
    }

    if (g_log_file != NULL && g_log_open_week_yyyymmdd == week) {
        return 0;
    }

    if (g_log_file != NULL) {
        rotated = 1;
    }

    log_close_file();

    if (mkdir_p(g_log_config.dir) != 0) {
        fprintf(stderr, "[log] cannot create directory %s\n", g_log_config.dir);
        return -1;
    }

    if (log_build_path_for_week(path, sizeof(path), week) != 0) {
        fprintf(stderr, "[log] path too long for log file\n");
        return -1;
    }

    fp = fopen(path, "a");
    if (fp == NULL) {
        fprintf(stderr, "[log] cannot open %s\n", path);
        return -1;
    }

    g_log_file = fp;
    g_log_open_week_yyyymmdd = week;
    log_week_basename(week, g_log_open_name, sizeof(g_log_open_name));

    /* Day-8 switch packs closed week (+ legacy). Startup sweeps if ≥7. */
    log_archive_packable(rotated);

    return 0;
}

static int log_ensure_open(void)
{
    time_t now;
    struct tm tm_buf;
    struct tm *tm;

    if (!g_log_config.enabled || g_log_config.dir[0] == '\0') {
        return -1;
    }

    now = time(NULL);
    tm = localtime_r(&now, &tm_buf);
    if (tm == NULL) {
        return -1;
    }

    return log_open_for_time(tm);
}

#if !defined(MAILBOXD_CLIENT_BUILD)
void mailboxd_log_config_apply(const struct mailboxd_config *config)
{
    const char *dir_raw;
    const char *level_raw;
    mailboxd_log_level_t parsed;

    mailboxd_log_shutdown();
    mailboxd_log_config_defaults(&g_log_config);

    if (config != NULL) {
        g_log_config.enabled =
            mailboxd_config_get_bool(config, "log", "enabled", 1);
        dir_raw = mailboxd_config_get(config, "log", "dir", NULL);
        level_raw = mailboxd_config_get(config, "log", "level", "warn");

        parsed = mailboxd_log_parse_level(level_raw);
        if (level_raw != NULL && level_raw[0] != '\0' &&
            strcmp(level_raw, mailboxd_log_level_name(parsed)) != 0 &&
            strcasecmp(level_raw, mailboxd_log_level_name(parsed)) != 0) {
            fprintf(stderr,
                    "[log] unknown level '%s' — using warn (debug|stats|info|warn)\n",
                    level_raw);
        }
        g_log_config.level = parsed;

        if (dir_raw != NULL && dir_raw[0] != '\0') {
            if (mailboxd_path_resolve(g_log_config.dir, sizeof(g_log_config.dir),
                                   dir_raw) != MAILBOXD_OK) {
                fprintf(stderr, "[log] invalid dir path\n");
                g_log_config.enabled = 0;
            }
        } else if (g_log_config.enabled) {
            if (mailboxd_path_resolve(g_log_config.dir, sizeof(g_log_config.dir),
                                   MAILBOXD_DIR_LOGS) != MAILBOXD_OK) {
                fprintf(stderr, "[log] cannot resolve default log directory\n");
                g_log_config.enabled = 0;
            }
        }
    } else {
        if (mailboxd_path_resolve(g_log_config.dir, sizeof(g_log_config.dir),
                               MAILBOXD_DIR_LOGS) != MAILBOXD_OK) {
            g_log_config.enabled = 0;
        }
    }

    g_log_ready = 1;

    if (g_log_config.enabled) {
        if (log_ensure_open() != 0) {
            g_log_config.enabled = 0;
        }
    }

    printf("[log] enabled=%s dir=%s level=%s week=%s\n",
           mailboxd_bool_to_string(g_log_config.enabled),
           g_log_config.dir[0] != '\0' ? g_log_config.dir : "-",
           mailboxd_log_level_name(g_log_config.level),
           g_log_open_name[0] != '\0' ? g_log_open_name : "-");
}
#else
void mailboxd_log_config_apply(const struct mailboxd_config *config)
{
    (void)config;
    mailboxd_log_config_defaults(&g_log_config);
    g_log_ready = 1;
}
#endif

static void log_write_console(mailboxd_log_level_t level, const char *message)
{
    FILE *out = (level == MAILBOXD_LOG_WARN) ? stderr : stdout;

    fputs(message, out);
    fputc('\n', out);
    fflush(out);
}

static void log_write_file(mailboxd_log_level_t level, const char *message)
{
    char line[MAILBOXD_LOG_LINE_MAX + 64];
    time_t now;
    struct tm tm_buf;
    struct tm *tm;

    if (!g_log_config.enabled) {
        return;
    }

    pthread_mutex_lock(&g_log_lock);

    if (log_ensure_open() != 0) {
        pthread_mutex_unlock(&g_log_lock);
        return;
    }

    now = time(NULL);
    tm = localtime_r(&now, &tm_buf);
    if (tm == NULL) {
        pthread_mutex_unlock(&g_log_lock);
        return;
    }

    if (mailboxd_time_format_stamp(line, sizeof(line), tm, NULL) != MAILBOXD_OK) {
        pthread_mutex_unlock(&g_log_lock);
        return;
    }

    {
        size_t stamp_len = strlen(line);
        snprintf(line + stamp_len, sizeof(line) - stamp_len,
                 " [%s] %s\n", mailboxd_log_level_name(level), message);
    }

    fputs(line, g_log_file);
    fflush(g_log_file);

    pthread_mutex_unlock(&g_log_lock);
}

void mailboxd_log_write(mailboxd_log_level_t level, const char *fmt, ...)
{
    char message[MAILBOXD_LOG_LINE_MAX];
    va_list ap;

    if (fmt == NULL) {
        return;
    }

    if (g_log_ready && !mailboxd_log_level_visible(level)) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    log_write_console(level, message);
    if (g_log_ready) {
        log_write_file(level, message);
    }
}

void mailboxd_log_shutdown(void)
{
    pthread_mutex_lock(&g_log_lock);
    log_close_file();
    g_log_ready = 0;
    mailboxd_log_config_defaults(&g_log_config);
    pthread_mutex_unlock(&g_log_lock);
}
