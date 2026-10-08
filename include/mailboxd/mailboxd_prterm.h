/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_MAILBOXD_PRTERM_H
#define MAILBOXD_MAILBOXD_PRTERM_H

#include <stddef.h>
#include "mailboxd/limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PRTERM <-> MailboxD link plugin.
 *
 * A unix-domain socket server. PRTERM connects once, sends its
 * `LINK_TOKEN <token>\n` handshake, and from then on exchanges
 * ASCII line-protocol requests (USER CREATE / LIST / MAIL / BAND)
 * and push events (EVENT MAIL <user> <id>) with the daemon.
 *
 * INI: [transport.mailboxd_prterm] (MailboxD's standard transport.<name> shape)
 *   enabled   = yes
 *   bind      = /var/mailboxd/prterm.sock
 *   mode      = 0660
 *   link_token= <shared secret>
 *
 * Note: the working INI shape was originally [link.prterm] but the
 * plugin loads via the standard `transport.<name>` key like telnet;
 * this header documents the actual key PRTERMs deployment uses.
 *
 * Default values match the `nobody` MailboxD user (Punkt D in the
 * operator spec): the daemon runs as `nobody:nogroup`, the sock
 * mode is 0660, and PRTERM joins `nogroup` to read/write.
 *
 * Wire protocol version: 1
 */

#define MAILBOXD_PRTERM_DEFAULT_PATH "/var/mailboxd/prterm.sock"
#define MAILBOXD_PRTERM_DEFAULT_MODE 0660u
#define MAILBOXD_PRTERM_DEFAULT_TOKEN "changeme"
#define MAILBOXD_PRTERM_TOKEN_MAX 128
#define MAILBOXD_PRTERM_LINE_MAX 2048
#define MAILBOXD_PRTERM_VERSION 1

typedef struct mailboxd_mailboxd_prterm_config {
    int enabled;
    char bind[MAILBOXD_PATH_MAX];
    unsigned int mode;
    char link_token[MAILBOXD_PRTERM_TOKEN_MAX];
} mailboxd_mailboxd_prterm_config_t;

void mailboxd_mailboxd_prterm_config_defaults(mailboxd_mailboxd_prterm_config_t *config);
int  mailboxd_mailboxd_prterm_config_parse(const char *kv,
                                      mailboxd_mailboxd_prterm_config_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_MAILBOXD_PRTERM_H */
