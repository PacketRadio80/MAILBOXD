/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_TYPES_H
#define MAILBOXD_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mailboxd_result {
    MAILBOXD_OK = 0,
    /** Input is a local/mailbox command (not a MailboxD '/' command). */
    MAILBOXD_LOCAL_CMD = 1,
    /** Session should be closed (e.g. /exit). */
    MAILBOXD_SESSION_END = 2,
    MAILBOXD_ERR_INVALID = -1,
    MAILBOXD_ERR_NOMEM = -2,
    MAILBOXD_ERR_NOT_FOUND = -3,
    MAILBOXD_ERR_IO = -4,
    MAILBOXD_ERR_UNSUPPORTED = -5,
    MAILBOXD_ERR_BUSY = -6,
    MAILBOXD_ERR_DENIED = -7
} mailboxd_result_t;

typedef enum mailboxd_transport_kind {
    MAILBOXD_TRANSPORT_TELNET = 1,
    /** Non-interactive internal transport (the mailboxd_prterm plugin's
     * session kind) — never a user BBX login, never shown in /who. */
    MAILBOXD_TRANSPORT_INTERNAL = 2,
    /* MAILBOXD_TRANSPORT_SSH removed 2026-10-08: ssh was never wired to
     * an actual plugin; telnet is the only CLI transport. */
    /* MAILBOXD_TRANSPORT_WEBSOCKET removed 2026-10-08: PRTERM is the only web
     * front-end. */
    /* MAILBOXD_TRANSPORT_MAINS_PROXY removed 2026-10-08: there is no
     * "mains_proxy" / "Secondary" / "Proxy" role in the v2.8.0 line —
     * MailboxD is a single local BBX and the only inter-process link is
     * the PRTERM<->MailboxD unix-domain socket. Numbers 1-5 are kept
     * stable in logs; value 5 is now reserved. */
} mailboxd_transport_kind_t;

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_TYPES_H */
