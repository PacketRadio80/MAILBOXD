/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_SECURITY_H
#define MAILBOXD_SECURITY_H

#include "mailboxd/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_config;

/** Log file name under the configured logs directory. */
#define MAILBOXD_SECURITY_LOG_FILE "security.log"

void mailboxd_security_log_config_apply(const struct mailboxd_config *config);
void mailboxd_security_log_shutdown(void);

/** Append one line to security.log (timestamp added). Always enabled when the log directory is available. */
void mailboxd_security_log_write(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/** Absolute path to security.log when the security log is ready. */
mailboxd_result_t mailboxd_security_log_current_path(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_SECURITY_H */
