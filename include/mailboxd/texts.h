/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_TEXTS_H
#define MAILBOXD_TEXTS_H

#include "mailboxd/types.h"
#include "mailboxd/limits.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_session;

#define MAILBOXD_DEFAULT_TEXTS_PATH MAILBOXD_DIR_TEXT

#define MAILBOXD_TEXT_BANNER  "banner.txt"
#define MAILBOXD_TEXT_MOTD    "motd.txt"
#define MAILBOXD_TEXT_NEWS    "news.txt"
#define MAILBOXD_TEXT_RULES   "rules.txt"
#define MAILBOXD_TEXT_VERSION "version.txt"

/** Substituted in banner.txt / version.txt — MailboxD version number (e.g. 2.4.0). */
#define MAILBOXD_BANNER_TOKEN_VERSION "@version@"
/** Substituted in banner.txt line 2. */
#define MAILBOXD_BANNER_TOKEN_SERVICE "@service@"
/** Substituted in motd.txt (current session nickname). */
#define MAILBOXD_TEXT_TOKEN_USERNAME "@username@"
/** Substituted in version.txt — host operating system name (e.g. Linux). */
#define MAILBOXD_TEXT_TOKEN_OS "@os@"
/** Substituted in text files — local time per `[time]` (default HH:MM:SS). */
#define MAILBOXD_TEXT_TOKEN_TIME "%time%"
/** Substituted in text files — local date per `[time] date=` (default YYYY/MM/DD). */
#define MAILBOXD_TEXT_TOKEN_DATE "%date%"

#define MAILBOXD_TEXTS_PATH_MAX 512

typedef struct mailboxd_texts_config {
    char path[MAILBOXD_TEXTS_PATH_MAX];
} mailboxd_texts_config_t;

void mailboxd_texts_config_defaults(mailboxd_texts_config_t *texts);

/**
 * Build full path to a text file under the configured texts directory.
 * Returns MAILBOXD_OK on success.
 */
mailboxd_result_t mailboxd_texts_resolve(const mailboxd_texts_config_t *texts,
                                   const char *filename,
                                   char *out, size_t out_len);

/**
 * Send a text file line-by-line to the session (connection output).
 * Expands @version@, @service@, @username@, @os@, %time%, and %date%.
 * Missing files are skipped silently.
 */
mailboxd_result_t mailboxd_texts_send_file(const mailboxd_texts_config_t *texts,
                                     struct mailboxd_session *session,
                                     const char *filename);

/**
 * Send banner.txt with @version@ and @service@ tokens expanded.
 */
mailboxd_result_t mailboxd_texts_send_banner(const mailboxd_texts_config_t *texts,
                                       struct mailboxd_session *session,
                                       const char *version,
                                       const char *service_name);

/**
 * Send motd.txt with @username@ expanded from @p session.
 */
mailboxd_result_t mailboxd_texts_send_motd(const mailboxd_texts_config_t *texts,
                                    struct mailboxd_session *session);

/**
 * Send version.txt with @version@ and @os@ expanded from this host.
 * Falls back to the built-in two-line version block if the file is missing.
 */
mailboxd_result_t mailboxd_texts_send_version(const mailboxd_texts_config_t *texts,
                                        struct mailboxd_session *session);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_TEXTS_H */
