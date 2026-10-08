/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_BROADCAST_H
#define MAILBOXD_BROADCAST_H

/**
 * MailboxD broadcast — Main instance only.
 *
 * User command `/broadcast` (alias `/announce`): instant `+++` message to every
 * online local telnet user session on this Main.
 * Format: `+++ <from>: <message>` (manual Sysop/grant announce).
 * Automatic system notices use `***` instead.
 *
 * There is no RF side here — beacons, AX.25 and everything on air belong to
 * PRTERM.
 */

#include "mailboxd/config.h"
#include "mailboxd/limits.h"
#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Local Main announce message cap. */
#define MAILBOXD_BROADCAST_MESSAGE_MAX   240u

typedef struct mailboxd_broadcast_config {
    int enabled;
} mailboxd_broadcast_config_t;

struct mailboxd_service;
struct mailboxd_session;

void mailboxd_broadcast_config_defaults(mailboxd_broadcast_config_t *cfg);

/** Load `[broadcast]` from INI. */
void mailboxd_broadcast_config_apply(mailboxd_broadcast_config_t *cfg,
                                  const mailboxd_config_t *config);

const mailboxd_broadcast_config_t *mailboxd_service_get_broadcast(
    const struct mailboxd_service *service);

/**
 * Send @p message to every online local user on this Main (Sysop `/broadcast`).
 */
mailboxd_result_t mailboxd_broadcast_announce(struct mailboxd_service *service,
                                        struct mailboxd_session *from,
                                        const char *message);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_BROADCAST_H */
