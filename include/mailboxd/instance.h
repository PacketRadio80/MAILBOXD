/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_INSTANCE_H
#define MAILBOXD_INSTANCE_H

#include "mailboxd/networks.h"
#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * v2.8.0 instance model.
 *
 * MailboxD is a single local BBX. There is no "Main / Secondary / Proxy"
 * split, no mains_proxy mesh and no per-binary source file
 * (instance_role_*.c) — those are gone. The role is always MAIN.
 *
 * The functions below are kept because the registration, network
 * enforce and plugin-allowed code paths still reference them, but
 * they are no longer runtime-decided. They return the static
 * "MAIN" answer and a static "ok" verdict on every call.
 */
typedef enum mailboxd_instance_role {
    MAILBOXD_INSTANCE_MAIN = 1
} mailboxd_instance_role_t;

mailboxd_instance_role_t mailboxd_instance_role(void);

/** v2.8.0: standalone mode is gone (the old "Main + local RF on one
 * host" distinction only made sense while the Main was the only
 * role; now there is only one role). The functions are kept as
 * compatibility stubs and always return 0/disabled. */
void mailboxd_instance_set_standalone(int enabled);
int  mailboxd_instance_standalone(void);

/** Always "mailboxd". */
const char *mailboxd_instance_binary_name(void);

/** Always "Main". */
const char *mailboxd_instance_role_name(void);

/** Always 1 — MailboxD is a local BBX in v2.8.0. */
int mailboxd_instance_offers_user_bbx(void);

/** Kept for compatibility; v2.8.0 has no role-specific overrides. */
mailboxd_result_t mailboxd_networks_enforce_instance(mailboxd_networks_config_t *networks);

int mailboxd_instance_plugin_allowed(const char *plugin_name);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_INSTANCE_H */
