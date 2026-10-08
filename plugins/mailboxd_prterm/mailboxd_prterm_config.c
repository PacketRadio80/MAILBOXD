/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/mailboxd_prterm.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mailboxd_mailboxd_prterm_config_defaults(mailboxd_mailboxd_prterm_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->enabled = 0;
    mailboxd_strlcpy(config->bind, MAILBOXD_PRTERM_DEFAULT_PATH,
                  sizeof config->bind);
    config->mode = MAILBOXD_PRTERM_DEFAULT_MODE;
    mailboxd_strlcpy(config->link_token, MAILBOXD_PRTERM_DEFAULT_TOKEN,
                  sizeof config->link_token);
}

static const char *find_kv(const char *config, const char *key,
                           char *scratch, size_t scratch_len)
{
    /*
     * Like the telnet plugin: walk `;`-separated key=value pairs
     * and return the value for the requested key (NUL-terminated
     * into the caller-provided scratch buffer). The same simple
     * wire format is used because the plugin_start() API takes a
     * `const char *config` string built from the INI section.
     */
    if (config == NULL || key == NULL || scratch == NULL || scratch_len == 0) {
        return NULL;
    }

    size_t klen = strlen(key);
    const char *p = config;
    while (*p != '\0') {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            const char *e = p;
            while (*e != '\0' && *e != ';') {
                e++;
            }
            size_t vlen = (size_t)(e - p);
            if (vlen >= scratch_len) {
                vlen = scratch_len - 1;
            }
            memcpy(scratch, p, vlen);
            scratch[vlen] = '\0';
            return scratch;
        }
        while (*p != '\0' && *p != ';') {
            p++;
        }
        if (*p == ';') {
            p++;
        }
    }
    return NULL;
}

int mailboxd_mailboxd_prterm_config_parse(const char *kv,
                                      mailboxd_mailboxd_prterm_config_t *out)
{
    if (out == NULL) {
        return -1;
    }
    mailboxd_mailboxd_prterm_config_defaults(out);

    if (kv == NULL || kv[0] == '\0') {
        return 0;
    }

    char scratch[MAILBOXD_PATH_MAX];
    const char *v;

    v = find_kv(kv, "bind", scratch, sizeof scratch);
    if (v != NULL && v[0] != '\0') {
        mailboxd_strlcpy(out->bind, v, sizeof out->bind);
    }
    v = find_kv(kv, "link_token", scratch, sizeof scratch);
    if (v != NULL && v[0] != '\0') {
        mailboxd_strlcpy(out->link_token, v, sizeof out->link_token);
    }
    v = find_kv(kv, "mode", scratch, sizeof scratch);
    if (v != NULL && v[0] != '\0') {
        unsigned long m = strtoul(v, NULL, 8);
        if (m == 0 || (m & 0777) != m) {
            return -1;
        }
        out->mode = (unsigned int)m;
    }
    v = find_kv(kv, "enabled", scratch, sizeof scratch);
    if (v != NULL && v[0] != '\0') {
        if (strcmp(v, "yes") == 0 || strcmp(v, "true") == 0 || strcmp(v, "1") == 0) {
            out->enabled = 1;
        } else {
            out->enabled = 0;
        }
    }
    return 0;
}
