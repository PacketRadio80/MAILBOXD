/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/networks.h"
#include "mailboxd/entertain_config.h"
#include "mailboxd/instance.h"
#include "mailboxd/config.h"
#include "mailboxd/util.h"
#include "mailboxd/log.h"

#include <stdio.h>
#include <string.h>

static int str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }

    while (*a != '\0' && *b != '\0') {
        char ca = (char)(*a >= 'A' && *a <= 'Z' ? *a + 32 : *a);
        char cb = (char)(*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);

        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static int networks_has_key(const mailboxd_config_t *config, const char *key)
{
    const char *value;

    if (config == NULL || key == NULL) {
        return 0;
    }

    value = mailboxd_config_get(config, "networks", key, NULL);
    return value != NULL && value[0] != '\0';
}

void mailboxd_networks_config_defaults(mailboxd_networks_config_t *networks)
{
    if (networks == NULL) {
        return;
    }

    networks->telnet = 1;
}

void mailboxd_networks_config_apply(mailboxd_networks_config_t *networks,
                                 const mailboxd_config_t *config)
{
    if (networks == NULL) {
        return;
    }

    mailboxd_networks_config_defaults(networks);

    if (config == NULL) {
        return;
    }

    if (networks_has_key(config, "telnet")) {
        networks->telnet = mailboxd_config_get_bool(config, "networks",
                                                 "telnet", 1);
    }

    mailboxd_log_info("[networks] telnet=%s",
                   mailboxd_bool_to_string(networks->telnet));
}

int mailboxd_networks_is_static_transport(const char *plugin_name)
{
    if (plugin_name == NULL || plugin_name[0] == '\0') {
        return 0;
    }

    /* v2.8.0: no static transport. WebSocket was removed; the only
     * static path was WebSocket for Main, which is now gone. */
    (void)plugin_name;
    return 0;
}

int mailboxd_networks_transport_wanted(const char *plugin_name,
                                    const mailboxd_networks_config_t *networks)
{
    if (plugin_name == NULL || plugin_name[0] == '\0') {
        return 0;
    }

    if (!mailboxd_instance_plugin_allowed(plugin_name)) {
        return 0;
    }

    if (mailboxd_networks_is_static_transport(plugin_name)) {
        return 1;
    }

    if (networks == NULL) {
        return 0;
    }

    if (str_ieq(plugin_name, "telnet")) {
        return networks->telnet;
    }

    if (str_ieq(plugin_name, "entertain")) {
        (void)networks;
        return mailboxd_entertain_enabled();
    }

    /*
     * The PRTERM<->MailboxD link is not a user/operator transport;
     * it is enabled by the plugin's [transport.mailboxd_prterm] enabled=
     * key. The networks config block has no per-plugin switch for it.
     */
    if (str_ieq(plugin_name, "mailboxd_prterm")) {
        return 1;
    }

    return 0;
}
