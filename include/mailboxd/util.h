/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_UTIL_H
#define MAILBOXD_UTIL_H

#include "mailboxd/types.h"

#include <stddef.h>
#include <time.h>

struct tm;

struct mailboxd_config;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Safe string copy (always NUL-terminates when @p dst_size > 0).
 * Returns bytes copied excluding the terminator.
 */
size_t mailboxd_strlcpy(char *dst, const char *src, size_t dst_size);

/**
 * Join @p base and @p name into @p out with a single '/'.
 * Rejects empty components and path traversal ("..").
 */
mailboxd_result_t mailboxd_path_join(char *out, size_t out_len,
                               const char *base, const char *name);

/** Default relative storage path (@ref MAILBOXD_DIR_DATA). */
mailboxd_result_t mailboxd_default_user_data_path(char *out, size_t out_len);

/**
 * Expand a config path: `~` → $HOME, `~/foo` → $HOME/foo.
 * Empty or NULL @p path → @ref MAILBOXD_DIR_DATA. Other paths copied unchanged.
 */
mailboxd_result_t mailboxd_path_expand(char *out, size_t out_len, const char *path);

/** Set the MailboxD install root used by @ref mailboxd_path_resolve. */
void mailboxd_install_root_set(const char *root);

/** Install root set by @ref mailboxd_install_root_set, or NULL when unset. */
const char *mailboxd_install_root_get(void);

/**
 * Resolve a config path: @ref mailboxd_path_expand, then prefix relative paths
 * with the install root when set.
 */
mailboxd_result_t mailboxd_path_resolve(char *out, size_t out_len, const char *path);

/** Parent directory of @p path (POSIX `/` rules). */
mailboxd_result_t mailboxd_path_dirname(const char *path, char *out, size_t out_len);

/**
 * Host operating system name for display (e.g. Linux, FreeBSD, MacOS, Windows).
 * Does not include OS version or kernel release.
 */
mailboxd_result_t mailboxd_platform_os_name(char *out, size_t out_len);

/** Return non-zero when @p len is safe for MailboxD allocations. */
int mailboxd_size_ok(size_t len);

/**
 * MailboxD boolean configuration standard.
 * Canonical written form: @c yes / @c no (see @ref MAILBOXD_BOOL_YES / @ref MAILBOXD_BOOL_NO).
 * Accepted true:  yes, true, enable, enabled, on, 1
 * Accepted false: no, false, disable, disabled, off, 0
 * Matching is case-insensitive.
 */
#define MAILBOXD_BOOL_YES "yes"
#define MAILBOXD_BOOL_NO  "no"

/** Return non-zero when @p value is a recognized true token. */
int mailboxd_bool_is_true(const char *value);

/** Return non-zero when @p value is a recognized false token. */
int mailboxd_bool_is_false(const char *value);

/**
 * Parse a boolean string. Returns 1 or 0 when recognized; otherwise
 * returns @p default_value (also used when @p value is NULL or empty).
 */
int mailboxd_parse_bool(const char *value, int default_value);

/** Canonical @c yes / @c no string for a boolean value. */
const char *mailboxd_bool_to_string(int value);

/** Short name for @p rc (logging / tests). */
const char *mailboxd_result_name(mailboxd_result_t rc);

/**
 * MailboxD system-local date/time formatting for logs and text/ tokens.
 * Default: 24-hour clock with seconds, ISO date YYYY/MM/DD.
 */
typedef enum mailboxd_date_format {
    MAILBOXD_DATE_ISO = 0,
    MAILBOXD_DATE_ISO_SHORT = 1,
    MAILBOXD_DATE_US = 2,
    MAILBOXD_DATE_EU = 3,
} mailboxd_date_format_t;

typedef struct mailboxd_time_format {
    int clock_12h;
    int seconds;
    mailboxd_date_format_t date_format;
} mailboxd_time_format_t;

void mailboxd_time_format_defaults(mailboxd_time_format_t *fmt);
const mailboxd_time_format_t *mailboxd_time_format_get(void);
void mailboxd_time_config_apply(const struct mailboxd_config *config);

/** Local wall-clock time for text tokens and stamps. */
mailboxd_result_t mailboxd_time_local_now(struct tm *out);

const char *mailboxd_date_format_name(mailboxd_date_format_t fmt);

/**
 * Format @p tm as clock time (default @c HH:MM:SS, 12h when configured).
 */
mailboxd_result_t mailboxd_time_format_time(char *out, size_t out_len,
                                      const struct tm *tm,
                                      const mailboxd_time_format_t *fmt);

/**
 * Format @p tm as calendar date (default @c YYYY/MM/DD).
 */
mailboxd_result_t mailboxd_time_format_date(char *out, size_t out_len,
                                      const struct tm *tm,
                                      const mailboxd_time_format_t *fmt);

/**
 * Format @p tm as @c yyyymmdd HH:MM per @p fmt (compact log stamp).
 * @return MAILBOXD_OK on success.
 */
mailboxd_result_t mailboxd_time_format_stamp(char *out, size_t out_len,
                                       const struct tm *tm,
                                       const mailboxd_time_format_t *fmt);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_UTIL_H */
