/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/registry.h"
#include "mailboxd/limits.h"

#include <string.h>

typedef struct registry_entry {
    const mailboxd_transport_plugin_t *plugin;
} registry_entry_t;

static registry_entry_t g_registry[MAILBOXD_MAX_PLUGINS];
static size_t g_registry_count = 0;

mailboxd_result_t mailboxd_registry_register(const mailboxd_transport_plugin_t *plugin)
{
    if (plugin == NULL || plugin->name == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_registry_find(plugin->name) != NULL) {
        return MAILBOXD_ERR_BUSY;
    }

    if (g_registry_count >= MAILBOXD_MAX_PLUGINS) {
        return MAILBOXD_ERR_NOMEM;
    }

    g_registry[g_registry_count].plugin = plugin;
    g_registry_count++;
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_registry_unregister(const char *name)
{
    size_t i;

    if (name == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    for (i = 0; i < g_registry_count; i++) {
        if (strcmp(g_registry[i].plugin->name, name) == 0) {
            size_t remaining = g_registry_count - i - 1;
            if (remaining > 0) {
                memmove(&g_registry[i], &g_registry[i + 1],
                        remaining * sizeof(registry_entry_t));
            }
            g_registry_count--;
            return MAILBOXD_OK;
        }
    }

    return MAILBOXD_ERR_NOT_FOUND;
}

const mailboxd_transport_plugin_t *mailboxd_registry_find(const char *name)
{
    size_t i;

    if (name == NULL) {
        return NULL;
    }

    for (i = 0; i < g_registry_count; i++) {
        if (strcmp(g_registry[i].plugin->name, name) == 0) {
            return g_registry[i].plugin;
        }
    }

    return NULL;
}

void mailboxd_registry_foreach(mailboxd_registry_iter_fn fn, void *userdata)
{
    size_t i;

    if (fn == NULL) {
        return;
    }

    for (i = 0; i < g_registry_count; i++) {
        fn(g_registry[i].plugin, userdata);
    }
}
