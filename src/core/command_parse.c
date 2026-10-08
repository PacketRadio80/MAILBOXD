/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/command.h"
#include "mailboxd/limits.h"
#include "mailboxd/util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char *mailboxd_strdup(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }

    len = strlen(s) + 1;
    if (!mailboxd_size_ok(len)) {
        return NULL;
    }
    copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

static const char *skip_leading_space(const char *line)
{
    if (line == NULL) {
        return NULL;
    }

    while (*line != '\0' && isspace((unsigned char)*line)) {
        line++;
    }

    return line;
}

static char *ltrim_copy(char *s)
{
    char *start;

    if (s == NULL) {
        return NULL;
    }

    start = (char *)skip_leading_space(s);
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    return s;
}

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

static int strip_one_command_alias(char *rest, const char *alias)
{
    size_t alias_len;

    if (alias == NULL || rest == NULL) {
        return -1;
    }

    alias_len = strlen(alias);

    if (str_ieq(rest, alias)) {
        return 1;
    }

    if (strncmp(rest, alias, alias_len) == 0 &&
        isspace((unsigned char)rest[alias_len])) {
        memmove(rest, rest + alias_len, strlen(rest + alias_len) + 1);
        ltrim_copy(rest);
        return rest[0] == '\0' ? 1 : 0;
    }

    return -1;
}

static int strip_command_alias(char *rest)
{
    static const char *aliases[] = { "commands", "command", "cmd", NULL };
    int rc;
    size_t i;

    if (rest == NULL) {
        return 0;
    }

    if (rest[0] == '\0') {
        return 1;
    }

    for (i = 0; aliases[i] != NULL; i++) {
        rc = strip_one_command_alias(rest, aliases[i]);
        if (rc >= 0) {
            return rc;
        }
    }

    return 0;
}

mailboxd_command_scope_t mailboxd_command_classify(const char *line)
{
    const char *p = skip_leading_space(line);

    if (p == NULL || *p == '\0') {
        return MAILBOXD_CMD_SCOPE_LOCAL;
    }

    if (*p == '/') {
        return MAILBOXD_CMD_SCOPE_MAILBOXD;
    }

    if (*p == ';' || *p == '#') {
        return MAILBOXD_CMD_SCOPE_COMMENT;
    }

    return MAILBOXD_CMD_SCOPE_LOCAL;
}

int mailboxd_command_is_comment(const char *line)
{
    return mailboxd_command_classify(line) == MAILBOXD_CMD_SCOPE_COMMENT;
}

int mailboxd_command_is_mailboxd(const char *line)
{
    return mailboxd_command_classify(line) == MAILBOXD_CMD_SCOPE_MAILBOXD;
}

static size_t count_tokens(const char *line)
{
    size_t count = 0;
    size_t i = 0;
    int in_token = 0;

    while (line[i] != '\0') {
        if (!isspace((unsigned char)line[i])) {
            if (!in_token) {
                count++;
                in_token = 1;
            }
        } else {
            in_token = 0;
        }
        i++;
    }

    return count;
}

static mailboxd_result_t split_tokens(char *line, char **tokens, size_t max)
{
    size_t count = 0;
    size_t i = 0;

    while (line[i] != '\0' && count < max) {
        while (line[i] != '\0' && isspace((unsigned char)line[i])) {
            i++;
        }
        if (line[i] == '\0') {
            break;
        }
        tokens[count++] = line + i;
        while (line[i] != '\0' && !isspace((unsigned char)line[i])) {
            i++;
        }
        if (line[i] != '\0') {
            line[i] = '\0';
            i++;
        }
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t parse_mailboxd_line(const char *line, mailboxd_parsed_command_t *out)
{
    char *rest;
    char *work;
    size_t token_count;
    char **tokens;
    size_t i;
    mailboxd_result_t rc;

    if (line[0] != '/') {
        return MAILBOXD_ERR_INVALID;
    }

    rest = mailboxd_strdup(line + 1);
    if (rest == NULL) {
        return MAILBOXD_ERR_NOMEM;
    }

    ltrim_copy(rest);

    if (strip_command_alias(rest)) {
        out->verb = mailboxd_strdup("help");
        if (out->verb == NULL) {
            free(rest);
            return MAILBOXD_ERR_NOMEM;
        }
        free(rest);
        return MAILBOXD_OK;
    }

    token_count = count_tokens(rest);
    if (token_count == 0) {
        out->verb = mailboxd_strdup("help");
        free(rest);
        if (out->verb == NULL) {
            return MAILBOXD_ERR_NOMEM;
        }
        return MAILBOXD_OK;
    }

    if (token_count > MAILBOXD_CMD_TOKEN_MAX) {
        free(rest);
        return MAILBOXD_ERR_INVALID;
    }

    work = mailboxd_strdup(rest);
    free(rest);
    if (work == NULL) {
        return MAILBOXD_ERR_NOMEM;
    }

    tokens = calloc(token_count, sizeof(*tokens));
    if (tokens == NULL) {
        free(work);
        return MAILBOXD_ERR_NOMEM;
    }

    rc = split_tokens(work, tokens, token_count);
    if (rc != MAILBOXD_OK) {
        free(tokens);
        free(work);
        return rc;
    }

    out->verb = mailboxd_strdup(tokens[0]);
    if (out->verb == NULL) {
        free(tokens);
        free(work);
        return MAILBOXD_ERR_NOMEM;
    }

    out->argc = token_count > 1 ? token_count - 1 : 0;
    if (out->argc > 0) {
        out->argv = calloc(out->argc, sizeof(*out->argv));
        if (out->argv == NULL) {
            free(tokens);
            free(work);
            mailboxd_command_free(out);
            return MAILBOXD_ERR_NOMEM;
        }

        for (i = 0; i < out->argc; i++) {
            out->argv[i] = mailboxd_strdup(tokens[i + 1]);
            if (out->argv[i] == NULL) {
                free(tokens);
                free(work);
                mailboxd_command_free(out);
                return MAILBOXD_ERR_NOMEM;
            }
        }
    }

    free(tokens);
    free(work);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_command_parse(const char *line,
                                   mailboxd_parsed_command_t *out)
{
    char *trimmed;
    mailboxd_result_t rc;

    if (line == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));

    trimmed = mailboxd_strdup(skip_leading_space(line));
    if (trimmed == NULL) {
        return MAILBOXD_ERR_NOMEM;
    }

    out->scope = mailboxd_command_classify(trimmed);
    out->line = trimmed;

    if (out->scope != MAILBOXD_CMD_SCOPE_MAILBOXD) {
        return MAILBOXD_OK;
    }

    rc = parse_mailboxd_line(trimmed, out);
    if (rc != MAILBOXD_OK) {
        mailboxd_command_free(out);
    }

    return rc;
}

void mailboxd_command_free(mailboxd_parsed_command_t *cmd)
{
    size_t i;

    if (cmd == NULL) {
        return;
    }

    free(cmd->line);
    free(cmd->verb);

    for (i = 0; i < cmd->argc; i++) {
        free(cmd->argv[i]);
    }
    free(cmd->argv);

    memset(cmd, 0, sizeof(*cmd));
}
