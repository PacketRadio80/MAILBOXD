/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_ENTERTAIN_CONFIG_H
#define MAILBOXD_ENTERTAIN_CONFIG_H

#include "mailboxd/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Entertain feature area — Main / standalone Main only.
 * Chess is a fixed part of Entertain (no separate chess= INI key).
 * Future: /proxychess (stub) for mesh play via Mains.
 */
typedef struct mailboxd_entertain_config {
    /** Non-zero when [entertain] enabled=yes and role allows. */
    int enabled;
} mailboxd_entertain_config_t;

void mailboxd_entertain_config_defaults(mailboxd_entertain_config_t *cfg);

/** Load [entertain]; force off on Secondary/Proxy. */
void mailboxd_entertain_config_apply(const mailboxd_config_t *config);

const mailboxd_entertain_config_t *mailboxd_entertain_config_get(void);

/** Non-zero when Entertain plugin should load. */
int mailboxd_entertain_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_ENTERTAIN_CONFIG_H */
