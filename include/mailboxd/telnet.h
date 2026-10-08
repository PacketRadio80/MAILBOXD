/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_TELNET_H
#define MAILBOXD_TELNET_H

#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default MailboxD telnet TCP port (BBX / terminal convention). */
#define MAILBOXD_TELNET_DEFAULT_PORT 2323u

#define MAILBOXD_TELNET_BIND_V4_MAX 64
#define MAILBOXD_TELNET_BIND_V6_MAX 64

#define MAILBOXD_TELNET_DEFAULT_BIND_V4 "0.0.0.0"
#define MAILBOXD_TELNET_DEFAULT_BIND_V6 "::"

typedef struct mailboxd_telnet_config {
    char bind_v4[MAILBOXD_TELNET_BIND_V4_MAX];
    char bind_v6[MAILBOXD_TELNET_BIND_V6_MAX];
    unsigned int port;
    int ipv4;
    int ipv6;
} mailboxd_telnet_config_t;

void mailboxd_telnet_config_defaults(mailboxd_telnet_config_t *config);

/**
 * Parse a semicolon-separated key=value transport config string
 * (from transport.telnet INI section).
 */
mailboxd_result_t mailboxd_telnet_config_parse(const char *config,
                                         mailboxd_telnet_config_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_TELNET_H */
