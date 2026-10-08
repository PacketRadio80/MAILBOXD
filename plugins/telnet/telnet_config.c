/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/telnet.h"
#include "mailboxd/util.h"

#include <stdlib.h>
#include <string.h>

static unsigned int parse_port(const char *value)
{
    char *end;
    unsigned long port;

    if (value == NULL || value[0] == '\0') {
        return MAILBOXD_TELNET_DEFAULT_PORT;
    }

    port = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || port == 0 || port > 65535u) {
        return MAILBOXD_TELNET_DEFAULT_PORT;
    }

    return (unsigned int)port;
}

static const char *find_kv(const char *config, const char *key,
                           char *scratch, size_t scratch_len)
{
    const char *cursor = config;
    size_t key_len = strlen(key);

    if (config == NULL || key == NULL) {
        return NULL;
    }

    while (*cursor != '\0') {
        const char *sep = strchr(cursor, ';');
        const char *end = sep != NULL ? sep : cursor + strlen(cursor);
        const char *eq = strchr(cursor, '=');

        if (eq != NULL && eq < end && (size_t)(eq - cursor) == key_len &&
            strncmp(cursor, key, key_len) == 0) {
            const char *value = eq + 1;
            size_t value_len = (size_t)(end - value);

            if (scratch != NULL && scratch_len > 0) {
                if (value_len >= scratch_len) {
                    value_len = scratch_len - 1;
                }
                memcpy(scratch, value, value_len);
                scratch[value_len] = '\0';
                return scratch;
            }

            return value;
        }

        if (sep == NULL) {
            break;
        }
        cursor = sep + 1;
    }

    return NULL;
}

void mailboxd_telnet_config_defaults(mailboxd_telnet_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    mailboxd_strlcpy(config->bind_v4, MAILBOXD_TELNET_DEFAULT_BIND_V4,
                  sizeof(config->bind_v4));
    mailboxd_strlcpy(config->bind_v6, MAILBOXD_TELNET_DEFAULT_BIND_V6,
                  sizeof(config->bind_v6));
    config->port = MAILBOXD_TELNET_DEFAULT_PORT;
    config->ipv4 = 1;
    config->ipv6 = 1;
}

mailboxd_result_t mailboxd_telnet_config_parse(const char *config,
                                         mailboxd_telnet_config_t *out)
{
    char scratch[MAILBOXD_TELNET_BIND_V6_MAX];
    const char *value;

    if (out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    mailboxd_telnet_config_defaults(out);

    if (config == NULL || config[0] == '\0') {
        return MAILBOXD_OK;
    }

    value = find_kv(config, "port", scratch, sizeof(scratch));
    out->port = parse_port(value);

    value = find_kv(config, "bind", scratch, sizeof(scratch));
    if (value != NULL && value[0] != '\0') {
        if (strchr(value, ':') != NULL) {
            mailboxd_strlcpy(out->bind_v6, value, sizeof(out->bind_v6));
        } else {
            mailboxd_strlcpy(out->bind_v4, value, sizeof(out->bind_v4));
        }
    }

    value = find_kv(config, "bind6", scratch, sizeof(scratch));
    if (value != NULL && value[0] != '\0') {
        mailboxd_strlcpy(out->bind_v6, value, sizeof(out->bind_v6));
    }

    value = find_kv(config, "ipv4", scratch, sizeof(scratch));
    if (value != NULL) {
        out->ipv4 = mailboxd_parse_bool(value, 1);
    }

    value = find_kv(config, "ipv6", scratch, sizeof(scratch));
    if (value != NULL) {
        out->ipv6 = mailboxd_parse_bool(value, 1);
    }

    return MAILBOXD_OK;
}
