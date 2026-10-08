/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_LOG_H
#define MAILBOXD_LOG_H

#include "mailboxd/types.h"
#include "mailboxd/limits.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * File + console log severity (least to most severe for filtering):
 * debug → stats → info → warn (default threshold).
 * Messages at or above the configured [log] level are emitted.
 */
typedef enum mailboxd_log_level {
    MAILBOXD_LOG_DEBUG = 0,
    MAILBOXD_LOG_STATS = 1,
    MAILBOXD_LOG_INFO = 2,
    MAILBOXD_LOG_WARN = 3
} mailboxd_log_level_t;

typedef struct mailboxd_log_config {
    int enabled;
    char dir[MAILBOXD_PATH_MAX];
    mailboxd_log_level_t level;
} mailboxd_log_config_t;

void mailboxd_log_config_defaults(mailboxd_log_config_t *cfg);
void mailboxd_log_config_apply(const struct mailboxd_config *config);
const mailboxd_log_config_t *mailboxd_log_config_get(void);

/** Non-zero when file logging is active. */
int mailboxd_log_enabled(void);

/**
 * Copy absolute path of the current mailboxd log file into @p out.
 * Returns MAILBOXD_OK when logging is enabled and a path is available.
 */
mailboxd_result_t mailboxd_log_current_path(char *out, size_t out_len);

const char *mailboxd_log_level_name(mailboxd_log_level_t level);
mailboxd_log_level_t mailboxd_log_parse_level(const char *value);

/** Non-zero when @p level would be written (console and file). */
int mailboxd_log_level_visible(mailboxd_log_level_t level);

void mailboxd_log_write(mailboxd_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void mailboxd_log_shutdown(void);

#define mailboxd_log_debug(...) mailboxd_log_write(MAILBOXD_LOG_DEBUG, __VA_ARGS__)
#define mailboxd_log_stats(...) mailboxd_log_write(MAILBOXD_LOG_STATS, __VA_ARGS__)
#define mailboxd_log_info(...)  mailboxd_log_write(MAILBOXD_LOG_INFO, __VA_ARGS__)
#define mailboxd_log_warn(...)  mailboxd_log_write(MAILBOXD_LOG_WARN, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_LOG_H */
