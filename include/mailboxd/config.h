/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_CONFIG_H
#define MAILBOXD_CONFIG_H

#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mailboxd_config_entry {
    char *section;
    char *key;
    char *value;
} mailboxd_config_entry_t;

typedef struct mailboxd_config {
    mailboxd_config_entry_t *entries;
    size_t count;
} mailboxd_config_t;

/** Load configuration from an INI file. */
mailboxd_result_t mailboxd_config_load(mailboxd_config_t *config, const char *path);

/** Release all memory held by @p config. */
void mailboxd_config_free(mailboxd_config_t *config);

/**
 * Return the value for @p key in @p section, or @p default_value if missing.
 * Both @p section and @p key are case-sensitive.
 */
const char *mailboxd_config_get(const mailboxd_config_t *config,
                             const char *section,
                             const char *key,
                             const char *default_value);

/** Parse MailboxD boolean tokens (yes/no and aliases; see mailboxd_parse_bool). */
int mailboxd_config_get_bool(const mailboxd_config_t *config,
                          const char *section,
                          const char *key,
                          int default_value);

/**
 * Parse an unsigned decimal integer in [@p min_value, @p max_value].
 * Returns @p default_value when missing or invalid.
 */
unsigned mailboxd_config_get_uint(const mailboxd_config_t *config,
                               const char *section,
                               const char *key,
                               unsigned default_value,
                               unsigned min_value,
                               unsigned max_value);

/**
 * Build a semicolon-separated key=value string for all keys in @p section.
 * Caller must free the returned string.
 */
char *mailboxd_config_format_section(const mailboxd_config_t *config,
                                  const char *section);

/**
 * Resolve INI section for a transport plugin: @c transport.&lt;plugin&gt; first,
 * else the first @c transport.&lt;plugin&gt;&lt;digits&gt; section with keys
 * (e.g. @c transport.telnet1). Writes the chosen name to @p out_section.
 * Returns 1 when a numbered (or legacy) section with keys was found, else 0.
 */
int mailboxd_config_resolve_transport_section(const mailboxd_config_t *config,
                                           const char *plugin_name,
                                           char *out_section,
                                           size_t out_size);

typedef void (*mailboxd_config_iter_fn)(const char *section, const char *key,
                                     const char *value, void *ctx);

typedef void (*mailboxd_config_section_iter_fn)(const char *section, void *ctx);

/** Invoke @p fn for every entry in @p config (in file order). */
void mailboxd_config_foreach(const mailboxd_config_t *config,
                          mailboxd_config_iter_fn fn, void *ctx);

/** Invoke @p fn once per unique section name (first occurrence order). */
void mailboxd_config_foreach_section(const mailboxd_config_t *config,
                                  mailboxd_config_section_iter_fn fn, void *ctx);

/** Set or replace a key in @p config (in-memory only until saved). */
mailboxd_result_t mailboxd_config_set(mailboxd_config_t *config,
                               const char *section,
                               const char *key,
                               const char *value);

/** Remove all keys in @p section from @p config. */
void mailboxd_config_remove_section(mailboxd_config_t *config, const char *section);

/** Write @p config to @p path (INI format). */
mailboxd_result_t mailboxd_config_save(const mailboxd_config_t *config,
                                 const char *path);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_CONFIG_H */
