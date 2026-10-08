/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_PRIVILEGE_H
#define MAILBOXD_PRIVILEGE_H

#include "mailboxd/config.h"
#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Apply [service] user=/group=/uid=/gid= from @p config.
 *
 * Linux: mailboxd must not keep running as root. When euid==0, @c user is
 * required and privileges drop immediately via setuid/setgid.
 * Non-root starts are unchanged (optional @c user must match effective uid).
 */
mailboxd_result_t mailboxd_privilege_apply_from_config(const mailboxd_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_PRIVILEGE_H */
