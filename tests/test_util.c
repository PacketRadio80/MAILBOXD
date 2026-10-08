/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/limits.h"
#include "mailboxd/log.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_failures;

static void check_int(const char *label, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", label, got, want);
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

int main(void)
{
    check_int("true yes", mailboxd_bool_is_true("yes"), 1);
    check_int("true enable", mailboxd_bool_is_true("enable"), 1);
    check_int("false no", mailboxd_bool_is_false("no"), 1);
    check_int("false disable", mailboxd_bool_is_false("disable"), 1);

    check_int("parse yes", mailboxd_parse_bool("yes", 0), 1);
    check_int("parse false", mailboxd_parse_bool("false", 1), 0);
    check_int("parse empty", mailboxd_parse_bool("", 1), 1);
    check_int("parse garbage", mailboxd_parse_bool("maybe", 0), 0);

    check_str("bool yes", mailboxd_bool_to_string(1), MAILBOXD_BOOL_YES);
    check_str("bool no", mailboxd_bool_to_string(0), MAILBOXD_BOOL_NO);

    check_str("result ok", mailboxd_result_name(MAILBOXD_OK), "ok");
    check_str("result invalid", mailboxd_result_name(MAILBOXD_ERR_INVALID), "invalid");
    check_str("result unknown", mailboxd_result_name((mailboxd_result_t)-99), "unknown");

    check_int("log parse debug", (int)mailboxd_log_parse_level("debug"),
              (int)MAILBOXD_LOG_DEBUG);
    check_int("log parse stats", (int)mailboxd_log_parse_level("stats"),
              (int)MAILBOXD_LOG_STATS);
    check_int("log parse info", (int)mailboxd_log_parse_level("info"),
              (int)MAILBOXD_LOG_INFO);
    check_int("log parse warn", (int)mailboxd_log_parse_level("warn"),
              (int)MAILBOXD_LOG_WARN);
    check_int("log parse unknown", (int)mailboxd_log_parse_level("verbose"),
              (int)MAILBOXD_LOG_WARN);
    check_str("log name stats", mailboxd_log_level_name(MAILBOXD_LOG_STATS), "stats");

    {
        char path[MAILBOXD_PATH_MAX];
        const char *home = getenv("HOME");

        if (mailboxd_path_expand(path, sizeof(path), NULL) == MAILBOXD_OK) {
            check_str("path expand null", path, MAILBOXD_DIR_DATA);
        }
        if (home != NULL && home[0] != '\0') {
            char want[MAILBOXD_PATH_MAX];

            snprintf(want, sizeof(want), "%s/custom", home);
            if (mailboxd_path_expand(path, sizeof(path), "~/custom") == MAILBOXD_OK) {
                check_str("path expand tilde", path, want);
            }
            if (mailboxd_path_expand(path, sizeof(path), "~") == MAILBOXD_OK) {
                check_str("path expand home only", path, home);
            }
        }
    }

    {
        char path[MAILBOXD_PATH_MAX];
        char want[MAILBOXD_PATH_MAX];

        mailboxd_install_root_set("/opt/mailboxd");
        if (mailboxd_path_resolve(path, sizeof(path), MAILBOXD_DIR_DATA) == MAILBOXD_OK) {
            snprintf(want, sizeof(want), "/opt/mailboxd/%s", MAILBOXD_DIR_DATA);
            check_str("path resolve data", path, want);
        }
        if (mailboxd_path_resolve(path, sizeof(path), MAILBOXD_DIR_LOGS) == MAILBOXD_OK) {
            snprintf(want, sizeof(want), "/opt/mailboxd/%s", MAILBOXD_DIR_LOGS);
            check_str("path resolve logs", path, want);
        }
        mailboxd_install_root_set(NULL);
    }

    {
        char os_name[MAILBOXD_PATH_MAX];

        if (mailboxd_platform_os_name(os_name, sizeof(os_name)) != MAILBOXD_OK) {
            fprintf(stderr, "FAIL platform os name: lookup failed\n");
            g_failures++;
        } else if (os_name[0] == '\0') {
            fprintf(stderr, "FAIL platform os name: empty\n");
            g_failures++;
#if defined(__linux__)
        } else if (strcmp(os_name, "Linux") != 0) {
            fprintf(stderr, "FAIL platform os name: got '%s' want 'Linux'\n",
                    os_name);
            g_failures++;
#endif
        }
    }

    {
        struct tm tm_ref = {0};
        char time_buf[32];
        char date_buf[32];
        mailboxd_time_format_t fmt;

        tm_ref.tm_year = 126;
        tm_ref.tm_mon = 6;
        tm_ref.tm_mday = 8;
        tm_ref.tm_hour = 15;
        tm_ref.tm_min = 4;
        tm_ref.tm_sec = 5;

        mailboxd_time_format_defaults(&fmt);
        if (mailboxd_time_format_time(time_buf, sizeof(time_buf), &tm_ref,
                                   &fmt) == MAILBOXD_OK) {
            check_str("time 24h seconds", time_buf, "15:04:05");
        }

        fmt.date_format = MAILBOXD_DATE_US;
        if (mailboxd_time_format_date(date_buf, sizeof(date_buf), &tm_ref,
                                   &fmt) == MAILBOXD_OK) {
            check_str("date us", date_buf, "07/08/2026");
        }

        fmt.date_format = MAILBOXD_DATE_ISO;
        if (mailboxd_time_format_date(date_buf, sizeof(date_buf), &tm_ref,
                                   &fmt) == MAILBOXD_OK) {
            check_str("date iso", date_buf, "2026/07/08");
        }
    }

    if (g_failures != 0) {
        fprintf(stderr, "%u test(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }

    puts("test_util: ok");
    return EXIT_SUCCESS;
}
