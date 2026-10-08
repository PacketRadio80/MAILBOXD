/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_REGISTRY_H
#define MAILBOXD_REGISTRY_H

#include "mailboxd/plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register a transport plugin (called at plugin init time). */
mailboxd_result_t mailboxd_registry_register(const mailboxd_transport_plugin_t *plugin);

/** Unregister a transport plugin by name. */
mailboxd_result_t mailboxd_registry_unregister(const char *name);

/** Look up a registered plugin by name. Returns NULL if not found. */
const mailboxd_transport_plugin_t *mailboxd_registry_find(const char *name);

/** Iterate over all registered plugins. */
typedef void (*mailboxd_registry_iter_fn)(const mailboxd_transport_plugin_t *plugin,
                                       void *userdata);
void mailboxd_registry_foreach(mailboxd_registry_iter_fn fn, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_REGISTRY_H */
