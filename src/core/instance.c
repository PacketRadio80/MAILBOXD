/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/instance.h"
#include "mailboxd/log.h"
#include "mailboxd/util.h"

/*
 * v2.8.0: MailboxD is a single local BBX. The role is always MAIN,
 * standalone mode is gone, and "Secondary" / "Proxy" do not exist.
 * The functions below are kept as compatibility stubs that return
 * a single static answer; the previous code was dead anyway.
 */

mailboxd_instance_role_t mailboxd_instance_role(void)
{
    return MAILBOXD_INSTANCE_MAIN;
}

const char *mailboxd_instance_binary_name(void)
{
    return "mailboxd";
}

const char *mailboxd_instance_role_name(void)
{
    return "Main";
}

int mailboxd_instance_offers_user_bbx(void)
{
    return 1;
}

void mailboxd_instance_set_standalone(int enabled)
{
    (void)enabled;  /* no-op; kept for compatibility */
}

int mailboxd_instance_standalone(void)
{
    return 0;
}

/*
 * Plugin-allowed table for the single Main role (ssh and the HBX
 * circuit hub are gone entirely, not just disabled):
 *
 *   telnet       — yes  (the user-facing transport; CLI clients connect)
 *   mailboxd_prterm — yes (PRTERM front-end over the unix-domain sock)
 *   entertain    — yes  (chess, Main-only feature)
 *   other plugins — yes
 */
int mailboxd_instance_plugin_allowed(const char *plugin_name)
{
    if (plugin_name == NULL || plugin_name[0] == '\0') {
        return 0;
    }
    return 1;
}

mailboxd_result_t mailboxd_networks_enforce_instance(mailboxd_networks_config_t *networks)
{
    if (networks == NULL) {
        return MAILBOXD_ERR_INVALID;
    }
    mailboxd_log_info("[instance] binary=%s role=%s user_bbx=%s telnet=%s",
                      mailboxd_instance_binary_name(),
                      mailboxd_instance_role_name(),
                      mailboxd_instance_offers_user_bbx() ? "yes" : "no",
                      mailboxd_bool_to_string(networks->telnet));
    return MAILBOXD_OK;
}
