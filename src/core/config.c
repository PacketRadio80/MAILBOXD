/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/config.h"
#include "mailboxd/limits.h"
#include "mailboxd/util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAILBOXD_CONFIG_INITIAL_CAP 32

static char *mailboxd_strdup(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }

    len = strlen(s) + 1;
    copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

static char *trim_inplace(char *s)
{
    char *end;

    if (s == NULL) {
        return NULL;
    }

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }

    if (*s == '\0') {
        return s;
    }

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return s;
}

static void strip_inline_comment_inplace(char *value)
{
    size_t i;
    int in_single = 0;
    int in_double = 0;

    if (value == NULL) {
        return;
    }

    for (i = 0; value[i] != '\0'; i++) {
        char c = value[i];

        if (!in_single && !in_double && (c == ';' || c == '#')) {
            value[i] = '\0';
            break;
        }

        if (!in_double && c == '\'') {
            in_single = !in_single;
        } else if (!in_single && c == '"') {
            in_double = !in_double;
        }
    }

    {
        char *end = value + strlen(value);

        while (end > value && isspace((unsigned char)*(end - 1))) {
            *--end = '\0';
        }
    }
}

static int is_comment_line(const char *line)
{
    const char *p = line;

    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }

    return *p == ';' || *p == '#';
}

static mailboxd_result_t append_entry(mailboxd_config_t *config,
                                 const char *section,
                                 const char *key,
                                 const char *value)
{
    mailboxd_config_entry_t *entry;
    mailboxd_config_entry_t *grown;

    if (section == NULL || key == NULL || value == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (strlen(section) >= MAILBOXD_CONFIG_SECTION_MAX ||
        strlen(key) >= MAILBOXD_CONFIG_KEY_MAX ||
        strlen(value) >= MAILBOXD_CONFIG_VALUE_MAX) {
        return MAILBOXD_ERR_INVALID;
    }

    if (config->count == 0) {
        config->entries = malloc(MAILBOXD_CONFIG_INITIAL_CAP * sizeof(*config->entries));
        if (config->entries == NULL) {
            return MAILBOXD_ERR_NOMEM;
        }
    } else if ((config->count % MAILBOXD_CONFIG_INITIAL_CAP) == 0) {
        grown = realloc(config->entries,
                        (config->count + MAILBOXD_CONFIG_INITIAL_CAP) *
                            sizeof(*config->entries));
        if (grown == NULL) {
            return MAILBOXD_ERR_NOMEM;
        }
        config->entries = grown;
    }

    entry = &config->entries[config->count];
    entry->section = mailboxd_strdup(section);
    entry->key = mailboxd_strdup(key);
    entry->value = mailboxd_strdup(value);

    if (entry->section == NULL || entry->key == NULL || entry->value == NULL) {
        free(entry->section);
        free(entry->key);
        free(entry->value);
        return MAILBOXD_ERR_NOMEM;
    }

    config->count++;
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_config_load(mailboxd_config_t *config, const char *path)
{
    FILE *fp;
    char line[MAILBOXD_CONFIG_LINE_MAX];
    char current_section[MAILBOXD_CONFIG_SECTION_MAX] = "";
    char *eq;
    char *key;
    char *value;

    if (config == NULL || path == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(config, 0, sizeof(*config));

    fp = fopen(path, "r");
    if (fp == NULL) {
        return MAILBOXD_ERR_IO;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *content;
        size_t len;

        len = strlen(line);
        if (len > 0 && line[len - 1] != '\n' &&
            len >= sizeof(line) - 1) {
            fclose(fp);
            mailboxd_config_free(config);
            return MAILBOXD_ERR_INVALID;
        }

        content = trim_inplace(line);

        if (content[0] == '\0' || is_comment_line(content)) {
            continue;
        }

        if (content[0] == '[') {
            char *closing = strchr(content, ']');

            if (closing == NULL) {
                fclose(fp);
                mailboxd_config_free(config);
                return MAILBOXD_ERR_INVALID;
            }

            *closing = '\0';
            strncpy(current_section, content + 1, sizeof(current_section) - 1);
            current_section[sizeof(current_section) - 1] = '\0';
            trim_inplace(current_section);
            continue;
        }

        if (current_section[0] == '\0') {
            fclose(fp);
            mailboxd_config_free(config);
            return MAILBOXD_ERR_INVALID;
        }

        eq = strchr(content, '=');
        if (eq == NULL) {
            fclose(fp);
            mailboxd_config_free(config);
            return MAILBOXD_ERR_INVALID;
        }

        *eq = '\0';
        key = trim_inplace(content);
        value = trim_inplace(eq + 1);
        strip_inline_comment_inplace(value);

        len = strlen(value);
        if (len >= 2 &&
            ((value[0] == '"' && value[len - 1] == '"') ||
             (value[0] == '\'' && value[len - 1] == '\''))) {
            value[len - 1] = '\0';
            value++;
        }

        if (append_entry(config, current_section, key, value) != MAILBOXD_OK) {
            fclose(fp);
            mailboxd_config_free(config);
            return MAILBOXD_ERR_NOMEM;
        }
    }

    fclose(fp);
    return MAILBOXD_OK;
}

void mailboxd_config_free(mailboxd_config_t *config)
{
    size_t i;

    if (config == NULL) {
        return;
    }

    for (i = 0; i < config->count; i++) {
        free(config->entries[i].section);
        free(config->entries[i].key);
        free(config->entries[i].value);
    }

    free(config->entries);
    config->entries = NULL;
    config->count = 0;
}

const char *mailboxd_config_get(const mailboxd_config_t *config,
                             const char *section,
                             const char *key,
                             const char *default_value)
{
    size_t i;

    if (config == NULL || section == NULL || key == NULL) {
        return default_value;
    }

    for (i = 0; i < config->count; i++) {
        if (strcmp(config->entries[i].section, section) == 0 &&
            strcmp(config->entries[i].key, key) == 0) {
            return config->entries[i].value;
        }
    }

    return default_value;
}

int mailboxd_config_get_bool(const mailboxd_config_t *config,
                          const char *section,
                          const char *key,
                          int default_value)
{
    const char *value = mailboxd_config_get(config, section, key, NULL);

    if (value == NULL) {
        return default_value;
    }

    return mailboxd_parse_bool(value, default_value);
}

unsigned mailboxd_config_get_uint(const mailboxd_config_t *config,
                               const char *section,
                               const char *key,
                               unsigned default_value,
                               unsigned min_value,
                               unsigned max_value)
{
    const char *value;
    char *end;
    unsigned long parsed;

    value = mailboxd_config_get(config, section, key, NULL);
    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0') {
        return default_value;
    }

    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }

    return (unsigned)parsed;
}

static int config_section_has_keys(const mailboxd_config_t *config,
                                   const char *section)
{
    size_t i;

    if (config == NULL || section == NULL) {
        return 0;
    }

    for (i = 0; i < config->count; i++) {
        const mailboxd_config_entry_t *entry = &config->entries[i];

        if (strcmp(entry->section, section) != 0) {
            continue;
        }
        if (strcmp(entry->key, "enabled") == 0) {
            continue;
        }
        return 1;
    }

    return 0;
}

static int config_suffix_is_digits(const char *suffix)
{
    if (suffix == NULL || suffix[0] == '\0') {
        return 0;
    }

    while (*suffix != '\0') {
        if (!isdigit((unsigned char)*suffix)) {
            return 0;
        }
        suffix++;
    }

    return 1;
}

int mailboxd_config_resolve_transport_section(const mailboxd_config_t *config,
                                           const char *plugin_name,
                                           char *out_section,
                                           size_t out_size)
{
    char prefix[128];
    size_t prefix_len;
    size_t i;

    if (config == NULL || plugin_name == NULL || out_section == NULL ||
        out_size == 0) {
        return 0;
    }

    snprintf(prefix, sizeof(prefix), "transport.%s", plugin_name);
    if (config_section_has_keys(config, prefix)) {
        mailboxd_strlcpy(out_section, prefix, out_size);
        return 1;
    }

    prefix_len = strlen(prefix);
    for (i = 0; i < config->count; i++) {
        const char *sec = config->entries[i].section;
        const char *suffix;

        if (sec == NULL || strncmp(sec, prefix, prefix_len) != 0) {
            continue;
        }

        suffix = sec + prefix_len;
        if (!config_suffix_is_digits(suffix)) {
            continue;
        }
        if (!config_section_has_keys(config, sec)) {
            continue;
        }

        mailboxd_strlcpy(out_section, sec, out_size);
        return 1;
    }

    mailboxd_strlcpy(out_section, prefix, out_size);
    return 0;
}

char *mailboxd_config_format_section(const mailboxd_config_t *config,
                                  const char *section)
{
    size_t i;
    size_t cap = 64;
    size_t len = 0;
    char *out;
    int first = 1;

    if (config == NULL || section == NULL) {
        return NULL;
    }

    out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }
    out[0] = '\0';

    for (i = 0; i < config->count; i++) {
        const mailboxd_config_entry_t *entry = &config->entries[i];
        size_t need;

        if (strcmp(entry->section, section) != 0) {
            continue;
        }

        if (strcmp(entry->key, "enabled") == 0) {
            continue;
        }

        need = strlen(entry->key) + strlen(entry->value) + 2;
        if (!first) {
            need++;
        }

        if (len + need + 1 > cap) {
            char *grown;

            while (len + need + 1 > cap) {
                cap *= 2;
            }
            grown = realloc(out, cap);
            if (grown == NULL) {
                free(out);
                return NULL;
            }
            out = grown;
        }

        if (!first) {
            out[len++] = ';';
            out[len] = '\0';
        }

        len += (size_t)snprintf(out + len, cap - len, "%s=%s",
                                entry->key, entry->value);
        first = 0;
    }

    return out;
}

void mailboxd_config_foreach(const mailboxd_config_t *config,
                          mailboxd_config_iter_fn fn, void *ctx)
{
    size_t i;

    if (config == NULL || fn == NULL) {
        return;
    }

    for (i = 0; i < config->count; i++) {
        const mailboxd_config_entry_t *entry = &config->entries[i];

        fn(entry->section, entry->key, entry->value, ctx);
    }
}

void mailboxd_config_foreach_section(const mailboxd_config_t *config,
                                  mailboxd_config_section_iter_fn fn, void *ctx)
{
    size_t i;

    if (config == NULL || fn == NULL) {
        return;
    }

    for (i = 0; i < config->count; i++) {
        const char *section = config->entries[i].section;
        size_t j;
        int seen = 0;

        for (j = 0; j < i; j++) {
            if (strcmp(config->entries[j].section, section) == 0) {
                seen = 1;
                break;
            }
        }

        if (!seen) {
            fn(section, ctx);
        }
    }
}

mailboxd_result_t mailboxd_config_set(mailboxd_config_t *config,
                               const char *section,
                               const char *key,
                               const char *value)
{
    size_t i;

    if (config == NULL || section == NULL || key == NULL || value == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    for (i = 0; i < config->count; i++) {
        if (strcmp(config->entries[i].section, section) == 0 &&
            strcmp(config->entries[i].key, key) == 0) {
            char *copy = mailboxd_strdup(value);

            if (copy == NULL) {
                return MAILBOXD_ERR_NOMEM;
            }
            free(config->entries[i].value);
            config->entries[i].value = copy;
            return MAILBOXD_OK;
        }
    }

    return append_entry(config, section, key, value);
}

void mailboxd_config_remove_section(mailboxd_config_t *config, const char *section)
{
    size_t i;

    if (config == NULL || section == NULL) {
        return;
    }

    i = 0;
    while (i < config->count) {
        if (strcmp(config->entries[i].section, section) == 0) {
            free(config->entries[i].section);
            free(config->entries[i].key);
            free(config->entries[i].value);
            if (i + 1 < config->count) {
                memmove(&config->entries[i], &config->entries[i + 1],
                        (config->count - i - 1) * sizeof(config->entries[i]));
            }
            config->count--;
        } else {
            i++;
        }
    }
}

mailboxd_result_t mailboxd_config_save(const mailboxd_config_t *config,
                                 const char *path)
{
    FILE *fp;
    size_t i;
    const char *current = NULL;

    if (config == NULL || path == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    fp = fopen(path, "w");
    if (fp == NULL) {
        return MAILBOXD_ERR_IO;
    }

    for (i = 0; i < config->count; i++) {
        const mailboxd_config_entry_t *entry = &config->entries[i];

        if (current == NULL || strcmp(current, entry->section) != 0) {
            if (current != NULL) {
                fputc('\n', fp);
            }
            fprintf(fp, "[%s]\n", entry->section);
            current = entry->section;
        }
        fprintf(fp, "%s = %s\n", entry->key, entry->value);
    }

    fclose(fp);
    return MAILBOXD_OK;
}
