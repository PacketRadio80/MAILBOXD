/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_SOCKET_H
#define MAILBOXD_SOCKET_H

#if defined(__linux__) || defined(__GLIBC__)
#define _DEFAULT_SOURCE 1
#endif

#include <sys/socket.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Suppress SIGPIPE on send() (SO_NOSIGPIPE on BSD/MacOS; noop on Linux). */
void mailboxd_socket_nosigpipe(int fd);

/**
 * Format the peer IP address of a connected socket into @p buf.
 * IPv4 dotted-quad or IPv6 colon form (no brackets).
 */
mailboxd_result_t mailboxd_socket_peer_name(int fd, char *buf, size_t buf_len);

/** Log bind(2) failure; hints when @c EADDRINUSE (MailboxD already running). */
void mailboxd_socket_log_bind_failure(const char *component, const char *addr,
                                   unsigned port);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_SOCKET_H */
