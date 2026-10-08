/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/chat.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/config.h"
#include "mailboxd/util.h"
#include "mailboxd/traffic.h"
#include "mailboxd/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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

static int channel_name_char_ok(char ch)
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == ' ' || ch == '-' || ch == '_';
}

static int channel_name_valid(const char *name)
{
    size_t len;
    size_t i;

    if (name == NULL) {
        return 0;
    }

    len = strlen(name);
    if (len < 2 || len >= MAILBOXD_CHAT_CHANNEL_NAME_MAX) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        if (!channel_name_char_ok(name[i])) {
            return 0;
        }
    }

    return 1;
}

void mailboxd_chat_config_defaults(mailboxd_chat_config_t *chat)
{
    unsigned i;

    if (chat == NULL) {
        return;
    }

    chat->channel_count = MAILBOXD_DEFAULT_CHAT_CHANNELS;
    chat->message_max = MAILBOXD_CHAT_MESSAGE_MAX;

    for (i = 0; i < MAILBOXD_CHAT_CHANNEL_MAX; i++) {
        snprintf(chat->names[i], sizeof(chat->names[i]), "Channel%u", i + 1);
    }
}

void mailboxd_chat_config_apply(mailboxd_chat_config_t *chat,
                             const mailboxd_config_t *config)
{
    unsigned count;
    unsigned i;
    char key[16];
    const char *value;

    if (chat == NULL || config == NULL) {
        return;
    }

    mailboxd_chat_config_defaults(chat);

    count = mailboxd_config_get_uint(config, "chat", "channels",
                                  MAILBOXD_DEFAULT_CHAT_CHANNELS, 1u,
                                  MAILBOXD_CHAT_CHANNEL_MAX);
    chat->channel_count = count;

    chat->message_max = mailboxd_config_get_uint(config, "chat", "message_max",
                                              MAILBOXD_CHAT_MESSAGE_MAX, 1u,
                                              MAILBOXD_LINE_MAX - 1u);

    for (i = 1; i <= chat->channel_count; i++) {
        snprintf(key, sizeof(key), "channel%u", i);
        value = mailboxd_config_get(config, "chat", key, NULL);
        if (value != NULL && value[0] != '\0' && channel_name_valid(value)) {
            mailboxd_strlcpy(chat->names[i - 1], value,
                          sizeof(chat->names[i - 1]));
        }
    }

    mailboxd_log_info("[chat] channels=%u message_max=%u",
           chat->channel_count, chat->message_max);
}

const char *mailboxd_chat_channel_name(const mailboxd_chat_config_t *chat,
                                    unsigned channel_index)
{
    if (chat == NULL || channel_index == 0 ||
        channel_index > chat->channel_count) {
        return NULL;
    }

    return chat->names[channel_index - 1];
}

mailboxd_result_t mailboxd_chat_resolve_channel(const mailboxd_chat_config_t *chat,
                                          const char *spec,
                                          unsigned *out_index)
{
    char *end;
    unsigned long parsed;
    unsigned i;

    if (chat == NULL || spec == NULL || out_index == NULL || spec[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    parsed = strtoul(spec, &end, 10);
    if (end != spec && *end == '\0' && parsed >= 1 &&
        parsed <= chat->channel_count) {
        *out_index = (unsigned)parsed;
        return MAILBOXD_OK;
    }

    for (i = 1; i <= chat->channel_count; i++) {
        if (str_ieq(chat->names[i - 1], spec)) {
            *out_index = i;
            return MAILBOXD_OK;
        }
    }

    return MAILBOXD_ERR_NOT_FOUND;
}

void mailboxd_chat_list_channels(mailboxd_session_t *session,
                              const mailboxd_chat_config_t *chat)
{
    unsigned i;
    char line[MAILBOXD_CHAT_CHANNEL_NAME_MAX + 32];
    unsigned current;

    if (session == NULL || chat == NULL) {
        return;
    }

    current = mailboxd_session_chat_channel(session);

    mailboxd_session_write_line(session, "Channels:");
    for (i = 1; i <= chat->channel_count; i++) {
        snprintf(line, sizeof(line), "  %u  %s", i, chat->names[i - 1]);
        mailboxd_session_write_line(session, line);
    }

    if (current > 0) {
        snprintf(line, sizeof(line), "Current: %u %s",
                 current, mailboxd_chat_channel_name(chat, current));
        mailboxd_session_write_line(session, line);
    } else {
        mailboxd_session_write_line(session, "Use /chat <n> or <name>.");
    }
}

typedef struct chat_broadcast_ctx {
    mailboxd_session_t *from;
    const char *message;
    const char *from_user;
    unsigned channel;
} chat_broadcast_ctx_t;

static void chat_broadcast_visitor(mailboxd_session_t *session, void *userdata)
{
    chat_broadcast_ctx_t *ctx = (chat_broadcast_ctx_t *)userdata;
    char line[MAILBOXD_USER_NAME_MAX + MAILBOXD_LINE_MAX + 16];

    if (session == NULL || ctx == NULL || ctx->message == NULL) {
        return;
    }

    if (mailboxd_session_area(session) != MAILBOXD_AREA_CHAT) {
        return;
    }

    if (mailboxd_session_chat_channel(session) != ctx->channel) {
        return;
    }

    if (session == ctx->from) {
        snprintf(line, sizeof(line), "ME: %s", ctx->message);
    } else {
        snprintf(line, sizeof(line), "%s: %s", ctx->from_user, ctx->message);
    }

    mailboxd_session_write_line(session, line);
}

mailboxd_result_t mailboxd_chat_post(mailboxd_service_t *service,
                               mailboxd_session_t *from,
                               const char *message)
{
    chat_broadcast_ctx_t ctx;
    const mailboxd_chat_config_t *chat;
    unsigned channel;

    if (service == NULL || from == NULL || message == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (message[0] == '\0') {
        return MAILBOXD_OK;
    }

    chat = mailboxd_service_get_chat(service);
    if (chat == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (strlen(message) > chat->message_max) {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_session_area(from) != MAILBOXD_AREA_CHAT) {
        return MAILBOXD_ERR_INVALID;
    }

    channel = mailboxd_session_chat_channel(from);
    if (channel == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    ctx.from = from;
    ctx.message = message;
    ctx.from_user = mailboxd_session_display_name(from);
    ctx.channel = channel;

    mailboxd_service_visit_sessions(service, chat_broadcast_visitor, &ctx);
    return MAILBOXD_OK;
}

#define CHAT_SHOWALL_MAX 128u

typedef struct chat_show_entry {
    unsigned channel;
    char label[MAILBOXD_USER_NAME_MAX + MAILBOXD_CHAT_CHANNEL_NAME_MAX + 4];
} chat_show_entry_t;

typedef struct chat_show_ctx {
    mailboxd_session_t *requester;
    unsigned channel;
    unsigned count;
    chat_show_entry_t entries[CHAT_SHOWALL_MAX];
    const mailboxd_chat_config_t *chat;
} chat_show_ctx_t;

static void chat_show_channel_visitor(mailboxd_session_t *session, void *userdata)
{
    chat_show_ctx_t *ctx = (chat_show_ctx_t *)userdata;

    if (session == NULL || ctx == NULL || session == ctx->requester) {
        return;
    }

    if (mailboxd_session_area(session) != MAILBOXD_AREA_CHAT) {
        return;
    }

    if (mailboxd_session_chat_channel(session) != ctx->channel) {
        return;
    }

    if (mailboxd_session_is_guest(session)) {
        return;
    }

    if (mailboxd_session_hidden_from_who(session)) {
        return;
    }

    mailboxd_session_write_line(ctx->requester, mailboxd_session_display_name(session));
}

void mailboxd_chat_show_channel(mailboxd_service_t *service, mailboxd_session_t *session)
{
    chat_show_ctx_t ctx;
    unsigned channel;

    if (service == NULL || session == NULL) {
        return;
    }

    if (mailboxd_session_area(session) != MAILBOXD_AREA_CHAT) {
        mailboxd_session_write_line(session, "Not in a chat channel.");
        return;
    }

    channel = mailboxd_session_chat_channel(session);
    if (channel == 0) {
        mailboxd_session_write_line(session, "Not in a chat channel.");
        return;
    }

    ctx.requester = session;
    ctx.channel = channel;
    ctx.count = 0;
    ctx.chat = NULL;

    mailboxd_service_visit_sessions(service, chat_show_channel_visitor, &ctx);
}

static void chat_show_all_collect(mailboxd_session_t *session, void *userdata)
{
    chat_show_ctx_t *ctx = (chat_show_ctx_t *)userdata;
    unsigned channel;
    const char *ch_name;
    chat_show_entry_t *entry;

    if (session == NULL || ctx == NULL || ctx->chat == NULL) {
        return;
    }

    if (mailboxd_session_area(session) != MAILBOXD_AREA_CHAT) {
        return;
    }

    if (mailboxd_session_is_guest(session)) {
        return;
    }

    if (mailboxd_session_hidden_from_who(session)) {
        return;
    }

    channel = mailboxd_session_chat_channel(session);
    if (channel == 0 || channel > ctx->chat->channel_count) {
        return;
    }

    if (ctx->count >= CHAT_SHOWALL_MAX) {
        return;
    }

    ch_name = mailboxd_chat_channel_name(ctx->chat, channel);
    if (ch_name == NULL) {
        return;
    }

    entry = &ctx->entries[ctx->count++];
    entry->channel = channel;
    snprintf(entry->label, sizeof(entry->label), "%s@%s",
             mailboxd_session_display_name(session), ch_name);
}

static int chat_show_entry_cmp(const void *a, const void *b)
{
    const chat_show_entry_t *ea = (const chat_show_entry_t *)a;
    const chat_show_entry_t *eb = (const chat_show_entry_t *)b;

    if (ea->channel != eb->channel) {
        return (ea->channel < eb->channel) ? -1 : 1;
    }

    return strcmp(ea->label, eb->label);
}

static void chat_showall_flush(mailboxd_session_t *session, char *line, size_t *pos)
{
    if (session == NULL || line == NULL || pos == NULL || *pos == 0) {
        return;
    }

    line[*pos] = '\0';
    mailboxd_session_write_line(session, line);
    line[0] = '\0';
    *pos = 0;
}

static void chat_showall_append(mailboxd_session_t *session, char *line,
                                size_t line_size, size_t *pos,
                                const char *entry)
{
    size_t entry_len;
    size_t need;

    if (session == NULL || line == NULL || pos == NULL || entry == NULL) {
        return;
    }

    entry_len = strlen(entry);
    need = (*pos > 0) ? (*pos + 1 + entry_len) : entry_len;

    if (*pos > 0 && need > MAILBOXD_LINE_WIDTH) {
        chat_showall_flush(session, line, pos);
        need = entry_len;
    }

    if (entry_len >= line_size) {
        return;
    }

    if (*pos > 0) {
        line[(*pos)++] = ' ';
    }

    memcpy(line + *pos, entry, entry_len);
    *pos += entry_len;
    line[*pos] = '\0';
}

void mailboxd_chat_show_all(mailboxd_service_t *service, mailboxd_session_t *session)
{
    chat_show_ctx_t ctx;
    char line[MAILBOXD_LINE_WIDTH + 1];
    size_t pos = 0;
    unsigned i;

    if (service == NULL || session == NULL) {
        return;
    }

    ctx.requester = session;
    ctx.channel = 0;
    ctx.count = 0;
    ctx.chat = mailboxd_service_get_chat(service);
    if (ctx.chat == NULL) {
        return;
    }

    mailboxd_service_visit_sessions(service, chat_show_all_collect, &ctx);

    if (ctx.count == 0) {
        return;
    }

    qsort(ctx.entries, ctx.count, sizeof(ctx.entries[0]), chat_show_entry_cmp);

    line[0] = '\0';
    for (i = 0; i < ctx.count; i++) {
        chat_showall_append(session, line, sizeof(line), &pos, ctx.entries[i].label);
    }

    chat_showall_flush(session, line, &pos);
}
