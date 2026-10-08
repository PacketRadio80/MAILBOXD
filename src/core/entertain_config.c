/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/entertain_config.h"
#include "mailboxd/instance.h"
#include "mailboxd/log.h"
#include "mailboxd/util.h"

#include <string.h>

static mailboxd_entertain_config_t g_entertain;
static int g_entertain_ready;

void mailboxd_entertain_config_defaults(mailboxd_entertain_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 0;
}

void mailboxd_entertain_config_apply(const mailboxd_config_t *config)
{
    mailboxd_instance_role_t role;
    const char *legacy;

    mailboxd_entertain_config_defaults(&g_entertain);
    g_entertain_ready = 1;

    if (config != NULL) {
        g_entertain.enabled = mailboxd_config_get_bool(config, "entertain",
                                                    "enabled", 0);
        /* Legacy: [networks] entertain= — prefer [entertain] enabled= */
        legacy = mailboxd_config_get(config, "networks", "entertain", NULL);
        if (legacy != NULL && legacy[0] != '\0' &&
            !mailboxd_config_get(config, "entertain", "enabled", NULL)) {
            g_entertain.enabled = mailboxd_bool_is_true(legacy);
            mailboxd_log_warn("[entertain] legacy [networks] entertain= — "
                           "use [entertain] enabled=yes");
        }
    }

    role = mailboxd_instance_role();
    if (g_entertain.enabled && role != MAILBOXD_INSTANCE_MAIN) {
        mailboxd_log_warn("[entertain] Main / standalone Main only — forcing off "
                       "(role=%s)",
                       mailboxd_instance_role_name());
        g_entertain.enabled = 0;
    }

    mailboxd_log_info("[entertain] enabled=%s (chess built-in; Main/standalone only)",
                   mailboxd_bool_to_string(g_entertain.enabled));
}

const mailboxd_entertain_config_t *mailboxd_entertain_config_get(void)
{
    if (!g_entertain_ready) {
        mailboxd_entertain_config_defaults(&g_entertain);
    }
    return &g_entertain;
}

int mailboxd_entertain_enabled(void)
{
    return mailboxd_entertain_config_get()->enabled;
}
