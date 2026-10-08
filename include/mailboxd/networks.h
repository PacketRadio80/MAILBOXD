/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_NETWORKS_H
#define MAILBOXD_NETWORKS_H

#include "mailboxd/config.h"
#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Connection / link adapter switches from INI `[networks]`.
 *
 * MailboxD is a single local BBX (v2.9.0): telnet is the only user/
 * operator transport; PRTERM reaches MailboxD over the separate
 * mailboxd_prterm unix-domain socket (not gated by this struct).
 */
typedef struct mailboxd_networks_config {
    /** Telnet (`transport.telnet`) — the user/operator access. */
    int telnet;
} mailboxd_networks_config_t;

void mailboxd_networks_config_defaults(mailboxd_networks_config_t *networks);

/** Load `[networks]` from an INI file. */
void mailboxd_networks_config_apply(mailboxd_networks_config_t *networks,
                                 const mailboxd_config_t *config);

/**
 * Non-zero when @p plugin_name is the role's static core transport.
 * Ignores `[transport.*] enabled` and is started when built.
 *
 * v2.8.0: no static transport — the web front-end is PRTERM, and the
 * user/operator access is telnet or the PRTERM<->MailboxD link.
 */
int mailboxd_networks_is_static_transport(const char *plugin_name);

/**
 * Non-zero when transport @p plugin_name should be started for this config.
 * Respects instance plugin allow-list and `[networks]` toggles.
 */
int mailboxd_networks_transport_wanted(const char *plugin_name,
                                    const mailboxd_networks_config_t *networks);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_NETWORKS_H */
