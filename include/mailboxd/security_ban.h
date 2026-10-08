/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_SECURITY_BAN_H
#define MAILBOXD_SECURITY_BAN_H

#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_config;

typedef enum mailboxd_ban_backend {
    MAILBOXD_BAN_BACKEND_INTERNAL = 0,
    MAILBOXD_BAN_BACKEND_LOG,
    MAILBOXD_BAN_BACKEND_IPTABLES,
    MAILBOXD_BAN_BACKEND_NFTABLES,
    MAILBOXD_BAN_BACKEND_HOSTS
} mailboxd_ban_backend_t;

void mailboxd_security_ban_config_apply(const struct mailboxd_config *config);
void mailboxd_security_ban_shutdown(void);

/** Expire bans and prune stale failure/rate records (service loop). */
void mailboxd_security_ban_tick(void);

/** Non-zero when @p ip is currently banned. */
int mailboxd_security_ban_is_banned(const char *ip);

/**
 * Normalize @p in into @p out (uppercase CALL/CALL-SSID or link_id).
 * Returns non-zero when valid.
 */
int mailboxd_security_callid_normalize(const char *in, char *out, size_t out_cap);

/** Non-zero when @p callid is banned (AX.25 callsign or HBX link_id). */
int mailboxd_security_ban_callid_is_banned(const char *callid);

/**
 * Accept gate for a CALLID (AX.25 source, etc.) before PRTERM hands it
 * off. Returns non-zero when allowed.
 */
int mailboxd_security_ban_callid_accept(const char *callid);

/**
 * Accept gate: rate-limit check + ban check.
 * Returns non-zero when the connection may proceed.
 */
int mailboxd_security_ban_accept(const char *ip);

/**
 * Same as @ref mailboxd_security_ban_accept using the peer address of @p fd.
 * Allows the connection when the peer address cannot be resolved.
 */
int mailboxd_security_ban_accept_fd(int fd);

/** Record a failed login for @p transport (telnet, …). */
void mailboxd_security_ban_login_fail(const char *ip, const char *transport);

/**
 * Record excessive abuse (chat/mail flood, register spam, …).
 * Uses @c abuse_maxretry / @c abuse_findtime — not login @c maxretry.
 * Normal traffic limits ([chat], [mail], [traffic]) never call this.
 */
void mailboxd_security_ban_abuse_report(const char *ip, const char *category);

/** Same as @ref mailboxd_security_ban_abuse_report for @p callid. */
void mailboxd_security_ban_callid_abuse_report(const char *callid,
                                            const char *category);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_SECURITY_BAN_H */
