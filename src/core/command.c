/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/mailboxd.h"
#include "mailboxd/command.h"
#include "mailboxd/commands_registry.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/texts.h"
#include "mailboxd/storage.h"
#include "mailboxd/auth.h"
#include "mailboxd/chat.h"
#include "mailboxd/conference.h"
#include "mailboxd/mail.h"
#include "mailboxd/broadcast.h"
#include "mailboxd/security.h"
#include "mailboxd/security_ban.h"
#include "mailboxd/service.h"
#include "mailboxd/password.h"
#include "mailboxd/traffic.h"
#include "mailboxd/monitor.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

static void cmd_deny_privilege(mailboxd_session_t *session);

static const char *cmd_help_unknown(mailboxd_session_t *session)
{
    if (mailboxd_session_is_guest(session) ||
        (mailboxd_session_login_prompt(session) &&
         !mailboxd_session_logged_in(session))) {
        return "Unknown command. Try /help.";
    }

    return "Unknown command. /help for list.";
}

static int cmd_verb_allowed_login_prompt(const char *verb)
{
    if (verb == NULL || verb[0] == '\0') {
        return 1;
    }

    if (mailboxd_commands_registry_verb_allowed(MAILBOXD_LEVEL_GUEST, verb)) {
        return 1;
    }

    return str_ieq(verb, "exit") || str_ieq(verb, "logout") ||
           str_ieq(verb, "bye") || str_ieq(verb, "quit");
}

static mailboxd_result_t cmd_check_access(mailboxd_session_t *session,
                                       const char *verb)
{
    mailboxd_user_level_t level = mailboxd_session_user_level(session);

    if (!mailboxd_session_logged_in(session) &&
        mailboxd_session_login_prompt(session)) {
        if (cmd_verb_allowed_login_prompt(verb)) {
            return MAILBOXD_OK;
        }
        mailboxd_session_write_line(session,
            "Log in with /login or use /register.");
        return MAILBOXD_ERR_DENIED;
    }

    if (mailboxd_commands_registry_verb_allowed(level, verb)) {
        return MAILBOXD_OK;
    }

    /* Optional [monitor] allow= grants beyond commands.yaml min level. */
    if ((str_ieq(verb, "monitor") || str_ieq(verb, "mon")) &&
        mailboxd_monitor_enabled() &&
        mailboxd_monitor_user_allowed(mailboxd_session_username(session))) {
        return MAILBOXD_OK;
    }

    if (level == MAILBOXD_LEVEL_GUEST) {
        mailboxd_session_write_line(session,
            "Not available to guests. Try /help.");
    } else if (str_ieq(verb, "register")) {
        mailboxd_session_write_line(session,
            "Only guests may self-register with /register.");
    } else if (str_ieq(verb, "changeme")) {
        mailboxd_session_write_line(session,
            "Only registered users may use /changeme.");
    } else {
        mailboxd_session_write_line(session, "Insufficient privileges.");
    }

    return MAILBOXD_ERR_DENIED;
}

static int cmd_deleteme_confirmed(const char *arg)
{
    return arg != NULL && mailboxd_bool_is_true(arg);
}

static void cmd_registry_usage(mailboxd_session_t *session, const char *verb)
{
    const char *canonical = mailboxd_commands_registry_canonical(verb);

    mailboxd_commands_registry_show_help(session, canonical);
}

static mailboxd_result_t cmd_help_topic(mailboxd_session_t *session, const char *topic)
{
    mailboxd_user_level_t level = mailboxd_session_user_level(session);
    const char *canonical;
    const mailboxd_command_def_t *def;

    if (topic == NULL || topic[0] == '\0') {
        mailboxd_commands_registry_show_menu(session);
        return MAILBOXD_OK;
    }

    canonical = mailboxd_commands_registry_canonical(topic);

    if (!mailboxd_commands_registry_help_allowed(level, canonical)) {
        /* Allow-list grants may read /help monitor without Sysop min. */
        if (!((str_ieq(canonical, "monitor") || str_ieq(canonical, "mon")) &&
              mailboxd_monitor_session_may_use(session))) {
            mailboxd_session_write_line(session, cmd_help_unknown(session));
            return MAILBOXD_ERR_NOT_FOUND;
        }
    }

    def = mailboxd_commands_registry_find(canonical);
    if (def == NULL || def->line1[0] == '\0') {
        mailboxd_session_write_line(session, cmd_help_unknown(session));
        return MAILBOXD_ERR_NOT_FOUND;
    }

    mailboxd_commands_registry_show_help(session, canonical);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_help(mailboxd_session_t *session,
                               const mailboxd_parsed_command_t *cmd)
{
    if (cmd->argc == 0) {
        mailboxd_commands_registry_show_menu(session);
        return MAILBOXD_OK;
    }

    return cmd_help_topic(session, cmd->argv[0]);
}

static mailboxd_result_t cmd_news(mailboxd_service_t *service, mailboxd_session_t *session)
{
    const mailboxd_texts_config_t *texts = mailboxd_service_get_texts(service);

    if (texts == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_texts_send_file(texts, session, MAILBOXD_TEXT_NEWS) != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "(no news available)");
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_banner(mailboxd_service_t *service, mailboxd_session_t *session)
{
    const mailboxd_texts_config_t *texts = mailboxd_service_get_texts(service);

    if (texts == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    (void)mailboxd_texts_send_banner(texts, session, MAILBOXD_VERSION_STRING,
                                  mailboxd_service_get_name(service));
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_motd(mailboxd_service_t *service, mailboxd_session_t *session)
{
    const mailboxd_texts_config_t *texts = mailboxd_service_get_texts(service);

    if (texts == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_texts_send_motd(texts, session) != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "(no motd available)");
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_rules(mailboxd_service_t *service, mailboxd_session_t *session)
{
    const mailboxd_texts_config_t *texts = mailboxd_service_get_texts(service);

    if (texts == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_texts_send_file(texts, session, MAILBOXD_TEXT_RULES) != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "(no rules available)");
    }

    return MAILBOXD_OK;
}

static const char *who_transport_label(const char *transport)
{
    if (transport == NULL || transport[0] == '\0') {
        return "unknown";
    }

    if (str_ieq(transport, "telnet")) {
        return "telnet";
    }

    return transport;
}

typedef struct who_ctx {
    mailboxd_session_t *requester;
    unsigned count;
    int at_plugin;
} who_ctx_t;

static void who_list_visitor(mailboxd_session_t *session, void *userdata)
{
    who_ctx_t *ctx = (who_ctx_t *)userdata;
    const mailboxd_session_record_t *rec;
    const char *name;
    const char *transport;
    char line[96];
    char user_at[80];

    if (ctx == NULL || session == NULL || !mailboxd_session_logged_in(session)) {
        return;
    }

    if (!mailboxd_session_is_interactive_user(session)) {
        return;
    }

    if (mailboxd_session_hidden_from_who(session)) {
        return;
    }

    if (ctx->at_plugin) {
        if (mailboxd_session_format_user_at_plugin(session, user_at,
                                               sizeof(user_at)) != MAILBOXD_OK) {
            return;
        }
        snprintf(line, sizeof(line), "  %s", user_at);
    } else {
        name = mailboxd_session_display_name(session);
        rec = mailboxd_session_record(session);
        transport = who_transport_label(rec != NULL ? rec->transport : NULL);
        if (rec == NULL || rec->transport[0] == '\0') {
            transport = who_transport_label(session->transport != NULL ?
                                            session->transport->name : NULL);
        }
        snprintf(line, sizeof(line), "  %-16s  %s", name, transport);
    }

    mailboxd_session_write_line(ctx->requester, line);
    ctx->count++;
}

static mailboxd_result_t cmd_who(mailboxd_service_t *service, mailboxd_session_t *session)
{
    who_ctx_t ctx;
    char total[32];

    ctx.requester = session;
    ctx.count = 0;
    ctx.at_plugin = mailboxd_service_login_announce(service);

    mailboxd_session_write_line(session, "Online users:");
    mailboxd_service_visit_sessions(service, who_list_visitor, &ctx);

    if (ctx.count == 0) {
        mailboxd_session_write_line(session, "  (none)");
    }

    snprintf(total, sizeof(total), "Total: %u", ctx.count);
    mailboxd_session_write_line(session, total);
    return MAILBOXD_OK;
}

typedef struct users_stats_ctx {
    size_t by_level[6];
    size_t total;
} users_stats_ctx_t;

static mailboxd_result_t users_stats_cb(const mailboxd_user_record_t *user, void *ctx)
{
    users_stats_ctx_t *stats = (users_stats_ctx_t *)ctx;

    if (user == NULL || stats == NULL) {
        return MAILBOXD_OK;
    }

    if (user->level < MAILBOXD_LEVEL_SYSOP || user->level > MAILBOXD_LEVEL_USER) {
        return MAILBOXD_OK;
    }

    stats->by_level[user->level]++;
    stats->total++;
    return MAILBOXD_OK;
}

static void users_compute_percents(const size_t *counts, size_t total,
                                   unsigned *percents)
{
    mailboxd_user_level_t level;
    mailboxd_user_level_t adjust_level = MAILBOXD_LEVEL_SYSOP;
    unsigned sum = 0;
    unsigned max_remainder = 0;

    if (total == 0) {
        return;
    }

    for (level = MAILBOXD_LEVEL_SYSOP; level <= MAILBOXD_LEVEL_USER; level++) {
        unsigned scaled = (unsigned)((counts[level] * 1000u) / total);
        unsigned rem = scaled % 10u;

        percents[level] = scaled / 10u;
        sum += percents[level];
        if (rem > max_remainder) {
            max_remainder = rem;
            adjust_level = level;
        }
    }

    if (sum < 100u) {
        percents[adjust_level] += 100u - sum;
    }
}

static mailboxd_result_t cmd_users(mailboxd_service_t *service, mailboxd_session_t *session)
{
    mailboxd_storage_t *storage;
    users_stats_ctx_t stats;
    unsigned percents[6];
    mailboxd_user_level_t level;
    char line[48];
    char total[40];
    mailboxd_result_t rc;

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        mailboxd_session_write_line(session, "User storage unavailable.");
        return MAILBOXD_ERR_INVALID;
    }

    memset(&stats, 0, sizeof(stats));
    memset(percents, 0, sizeof(percents));

    rc = mailboxd_storage_foreach_user(storage, users_stats_cb, &stats);
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Could not read user database.");
        return rc;
    }

    mailboxd_session_write_line(session, "Registered users:");

    if (stats.total == 0) {
        mailboxd_session_write_line(session, "  (none)");
    } else {
        users_compute_percents(stats.by_level, stats.total, percents);
        for (level = MAILBOXD_LEVEL_SYSOP; level <= MAILBOXD_LEVEL_USER; level++) {
            if (stats.by_level[level] == 0) {
                continue;
            }

            snprintf(line, sizeof(line), "  %-8s %3u%%",
                     mailboxd_user_level_name(level), percents[level]);
            mailboxd_session_write_line(session, line);
        }
    }

    snprintf(total, sizeof(total), "Total users: %zu", stats.total);
    mailboxd_session_write_line(session, total);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_session(mailboxd_session_t *session)
{
    char buf[128];
    const mailboxd_session_record_t *rec = mailboxd_session_record(session);

    if (rec == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    snprintf(buf, sizeof(buf), "User: %s", mailboxd_session_display_name(session));
    mailboxd_session_write_line(session, buf);
    snprintf(buf, sizeof(buf), "Level: %s",
             mailboxd_user_level_name(mailboxd_session_user_level(session)));
    mailboxd_session_write_line(session, buf);
    snprintf(buf, sizeof(buf), "Session: %llu",
             (unsigned long long)rec->session_id);
    mailboxd_session_write_line(session, buf);
    snprintf(buf, sizeof(buf), "Transport: %s", rec->transport);
    mailboxd_session_write_line(session, buf);
    return MAILBOXD_OK;
}

static mailboxd_result_t parse_profile_fields(const mailboxd_parsed_command_t *cmd,
                                           unsigned name_start,
                                           unsigned tail_extra,
                                           mailboxd_user_registration_t *reg)
{
    size_t i;
    size_t pos = 0;
    unsigned suffix;

    if (cmd == NULL || reg == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    suffix = 3u + tail_extra;
    if (cmd->argc < name_start + suffix) {
        return MAILBOXD_ERR_INVALID;
    }

    mailboxd_strlcpy(reg->email, cmd->argv[cmd->argc - 1u - tail_extra],
                  sizeof(reg->email));
    mailboxd_strlcpy(reg->location, cmd->argv[cmd->argc - 2u - tail_extra],
                  sizeof(reg->location));
    mailboxd_strlcpy(reg->country, cmd->argv[cmd->argc - 3u - tail_extra],
                  sizeof(reg->country));

    reg->full_name[0] = '\0';
    for (i = name_start; i + suffix < cmd->argc; i++) {
        size_t part_len;
        const char *part = cmd->argv[i];

        if (part == NULL) {
            continue;
        }

        part_len = strlen(part);
        if (pos > 0) {
            if (pos + 1 >= sizeof(reg->full_name)) {
                return MAILBOXD_ERR_INVALID;
            }
            reg->full_name[pos++] = ' ';
        }

        if (pos + part_len >= sizeof(reg->full_name)) {
            return MAILBOXD_ERR_INVALID;
        }

        memcpy(reg->full_name + pos, part, part_len);
        pos += part_len;
    }

    reg->full_name[pos] = '\0';
    return MAILBOXD_OK;
}

static mailboxd_result_t parse_registration(const mailboxd_parsed_command_t *cmd,
                                         mailboxd_user_registration_t *reg,
                                         int with_password)
{
    mailboxd_result_t rc;
    unsigned min_args = with_password ? 6u : 5u;
    unsigned tail_extra = with_password ? 1u : 0u;

    if (cmd == NULL || reg == NULL || cmd->argc < min_args) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(reg, 0, sizeof(*reg));
    mailboxd_strlcpy(reg->nickname, cmd->argv[0], sizeof(reg->nickname));
    mailboxd_strlcpy(reg->username, cmd->argv[0], sizeof(reg->username));
    mailboxd_username_normalize(reg->username);

    if (with_password) {
        mailboxd_strlcpy(reg->password, cmd->argv[cmd->argc - 1],
                      sizeof(reg->password));
    }

    rc = parse_profile_fields(cmd, 1, tail_extra, reg);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t parse_changeme(const mailboxd_parsed_command_t *cmd,
                                     const char *username,
                                     mailboxd_user_registration_t *reg,
                                     const char **old_password,
                                     const char **new_password)
{
    mailboxd_result_t rc;

    if (cmd == NULL || username == NULL || reg == NULL ||
        old_password == NULL || new_password == NULL || cmd->argc < 6) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(reg, 0, sizeof(*reg));
    mailboxd_strlcpy(reg->username, username, sizeof(reg->username));
    mailboxd_username_normalize(reg->username);
    *old_password = cmd->argv[0];
    *new_password = cmd->argv[1];

    rc = parse_profile_fields(cmd, 2, 0, reg);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t parse_userchange(const mailboxd_parsed_command_t *cmd,
                                       mailboxd_user_registration_t *reg,
                                       const char **new_password)
{
    mailboxd_result_t rc;

    if (cmd == NULL || reg == NULL || new_password == NULL || cmd->argc < 7) {
        return MAILBOXD_ERR_INVALID;
    }

    memset(reg, 0, sizeof(*reg));
    mailboxd_strlcpy(reg->username, cmd->argv[0], sizeof(reg->username));
    mailboxd_username_normalize(reg->username);
    *new_password = cmd->argv[1];

    rc = parse_profile_fields(cmd, 2, 0, reg);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_lookup_user(mailboxd_service_t *service,
                                      const char *username,
                                      mailboxd_user_record_t *out)
{
    mailboxd_storage_t *storage;
    mailboxd_result_t rc;

    if (service == NULL || username == NULL || out == NULL ||
        username[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_resolve_user(storage, username, out);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    return rc;
}

static void cmd_deny_privilege(mailboxd_session_t *session)
{
    mailboxd_session_write_line(session, "Insufficient privileges.");
}

static mailboxd_result_t cmd_activate(mailboxd_service_t *service,
                                   mailboxd_session_t *session,
                                   const mailboxd_parsed_command_t *cmd)
{
    mailboxd_user_record_t target;
    mailboxd_user_level_t actor_level;
    char buf[128];
    mailboxd_result_t rc;

    if (cmd->argc < 1 || cmd->argv[0] == NULL || cmd->argv[0][0] == '\0') {
        mailboxd_session_write_line(session, "Usage: /activate <username>");
        return MAILBOXD_OK;
    }

    actor_level = mailboxd_session_user_level(session);
    if (!mailboxd_commands_registry_verb_allowed(actor_level, "activate")) {
        cmd_deny_privilege(session);
        return MAILBOXD_ERR_DENIED;
    }

    rc = cmd_lookup_user(service, cmd->argv[0], &target);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "Unknown user.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (target.level != MAILBOXD_LEVEL_USER) {
        mailboxd_session_write_line(session,
            "Only pending registered user accounts can be activated.");
        return MAILBOXD_OK;
    }

    if (target.active) {
        mailboxd_session_write_line(session, "Account is already active.");
        return MAILBOXD_OK;
    }

    target.active = 1;
    rc = mailboxd_storage_update_user(mailboxd_service_get_storage(service), &target);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    snprintf(buf, sizeof(buf), "Activated '%s'.",
             mailboxd_user_display_name(&target));
    mailboxd_session_write_line(session, buf);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_promote(mailboxd_service_t *service,
                                  mailboxd_session_t *session,
                                  const mailboxd_parsed_command_t *cmd)
{
    mailboxd_user_record_t target;
    mailboxd_user_level_t actor_level;
    mailboxd_user_level_t new_level;
    char buf[128];
    mailboxd_result_t rc;

    if (cmd->argc < 2 || cmd->argv[0] == NULL || cmd->argv[0][0] == '\0' ||
        cmd->argv[1] == NULL || cmd->argv[1][0] == '\0') {
        mailboxd_session_write_line(session,
            "Usage: /promote <username> admin|mod");
        return MAILBOXD_OK;
    }

    actor_level = mailboxd_session_user_level(session);
    new_level = mailboxd_user_level_parse(cmd->argv[1]);
    if (new_level != MAILBOXD_LEVEL_ADMIN && new_level != MAILBOXD_LEVEL_MOD) {
        mailboxd_session_write_line(session,
            "Level must be admin or mod.");
        return MAILBOXD_OK;
    }

    rc = cmd_lookup_user(service, cmd->argv[0], &target);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "Unknown user.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (!mailboxd_commands_registry_may_promote(actor_level, target.level, target.active,
                                new_level)) {
        if (actor_level == MAILBOXD_LEVEL_MOD || actor_level == MAILBOXD_LEVEL_USER) {
            cmd_deny_privilege(session);
        } else if (new_level == MAILBOXD_LEVEL_ADMIN) {
            mailboxd_session_write_line(session,
                "Only the Sysop may add Admins.");
        } else {
            mailboxd_session_write_line(session,
                "Only Sysop or Admin may add Mods.");
        }
        return MAILBOXD_ERR_DENIED;
    }

    if (target.level == new_level) {
        snprintf(buf, sizeof(buf), "'%s' is already %s.",
                 mailboxd_user_display_name(&target),
                 mailboxd_user_level_name(new_level));
        mailboxd_session_write_line(session, buf);
        return MAILBOXD_OK;
    }

    target.level = new_level;
    rc = mailboxd_storage_update_user(mailboxd_service_get_storage(service), &target);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    snprintf(buf, sizeof(buf), "Promoted '%s' to %s.",
             mailboxd_user_display_name(&target),
             mailboxd_user_level_name(new_level));
    mailboxd_session_write_line(session, buf);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_demote(mailboxd_service_t *service,
                                 mailboxd_session_t *session,
                                 const mailboxd_parsed_command_t *cmd)
{
    mailboxd_user_record_t target;
    mailboxd_user_level_t actor_level;
    mailboxd_user_level_t old_level;
    char buf[128];
    mailboxd_result_t rc;

    if (cmd->argc < 1 || cmd->argv[0] == NULL || cmd->argv[0][0] == '\0') {
        mailboxd_session_write_line(session, "Usage: /demote <username>");
        return MAILBOXD_OK;
    }

    actor_level = mailboxd_session_user_level(session);

    rc = cmd_lookup_user(service, cmd->argv[0], &target);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "Unknown user.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (str_ieq(target.username, mailboxd_session_username(session))) {
        mailboxd_session_write_line(session, "You cannot demote your own account.");
        return MAILBOXD_OK;
    }

    old_level = target.level;
    if (!mailboxd_commands_registry_may_demote(actor_level, target.level)) {
        if (actor_level == MAILBOXD_LEVEL_MOD || actor_level == MAILBOXD_LEVEL_USER) {
            cmd_deny_privilege(session);
        } else if (target.level == MAILBOXD_LEVEL_ADMIN) {
            mailboxd_session_write_line(session,
                "Only the Sysop may remove Admins.");
        } else if (target.level == MAILBOXD_LEVEL_MOD) {
            mailboxd_session_write_line(session,
                "Only Sysop or Admin may remove Mods.");
        } else {
            mailboxd_session_write_line(session,
                "That account is not an Admin or Mod.");
        }
        return MAILBOXD_ERR_DENIED;
    }

    target.level = MAILBOXD_LEVEL_USER;
    rc = mailboxd_storage_update_user(mailboxd_service_get_storage(service), &target);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    snprintf(buf, sizeof(buf), "Demoted '%s' from %s to user.",
             mailboxd_user_display_name(&target),
             mailboxd_user_level_name(old_level));
    mailboxd_session_write_line(session, buf);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_delete(mailboxd_service_t *service,
                                 mailboxd_session_t *session,
                                 const mailboxd_parsed_command_t *cmd)
{
    mailboxd_user_record_t target;
    mailboxd_user_level_t actor_level;
    char buf[128];
    mailboxd_result_t rc;

    if (cmd->argc < 1 || cmd->argv[0] == NULL || cmd->argv[0][0] == '\0') {
        mailboxd_session_write_line(session, "Usage: /delete <username>");
        return MAILBOXD_OK;
    }

    actor_level = mailboxd_session_user_level(session);

    rc = cmd_lookup_user(service, cmd->argv[0], &target);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "Unknown user.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (str_ieq(target.username, mailboxd_session_username(session))) {
        mailboxd_session_write_line(session, "You cannot delete your own account.");
        return MAILBOXD_OK;
    }

    if (mailboxd_user_level_is_sysop(target.level)) {
        mailboxd_session_write_line(session,
            "The Sysop account is permanent and cannot be deleted.");
        return MAILBOXD_OK;
    }

    if (!mailboxd_commands_registry_may_delete(actor_level, target.level)) {
        if (actor_level == MAILBOXD_LEVEL_MOD || actor_level == MAILBOXD_LEVEL_USER) {
            cmd_deny_privilege(session);
        } else if (target.level == MAILBOXD_LEVEL_ADMIN) {
            mailboxd_session_write_line(session,
                "Admins cannot delete other Admins.");
        } else {
            cmd_deny_privilege(session);
        }
        return MAILBOXD_ERR_DENIED;
    }

    rc = mailboxd_storage_delete_user(mailboxd_service_get_storage(service), target.id);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    snprintf(buf, sizeof(buf), "Deleted account '%s'.",
             mailboxd_user_display_name(&target));
    mailboxd_session_write_line(session, buf);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_deleteme(mailboxd_service_t *service,
                                   mailboxd_session_t *session,
                                   const mailboxd_parsed_command_t *cmd)
{
    const mailboxd_session_record_t *rec;
    mailboxd_user_level_t level;
    const char *arg;
    mailboxd_result_t rc;

    (void)service;

    level = mailboxd_session_user_level(session);
    if (mailboxd_user_level_is_sysop(level)) {
        mailboxd_session_write_line(session,
            "The Sysop account is permanent and cannot be deleted.");
        return MAILBOXD_OK;
    }

    arg = cmd->argc > 0 ? cmd->argv[0] : NULL;
    if (arg != NULL && mailboxd_bool_is_false(arg)) {
        mailboxd_session_write_line(session, "Account deletion cancelled.");
        return MAILBOXD_OK;
    }

    if (!cmd_deleteme_confirmed(arg)) {
        mailboxd_session_write_line(session,
            "Usage: /deleteme yes|no");
        return MAILBOXD_OK;
    }

    rec = mailboxd_session_record(session);
    if (rec == NULL || rec->user_id == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_delete_user(mailboxd_service_get_storage(service),
                                   rec->user_id);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "Account not found.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    mailboxd_session_write_line(session, "Account deleted. Goodbye.");
    return MAILBOXD_SESSION_END;
}

static mailboxd_result_t cmd_register_user(mailboxd_service_t *service,
                                        mailboxd_session_t *session,
                                        const mailboxd_parsed_command_t *cmd,
                                        int staff_created)
{
    mailboxd_storage_t *storage;
    mailboxd_user_registration_t reg;
    mailboxd_user_record_t user;
    char buf[MAILBOXD_USER_FULL_NAME_MAX + 64];
    mailboxd_result_t rc;

    if (staff_created) {
        if (cmd->argc < 5) {
            mailboxd_session_write_line(session,
                "Usage: /usercreate <username> <full-name> <country> <location> <email>");
            return MAILBOXD_OK;
        }
    } else if (cmd->argc < 6) {
        mailboxd_session_write_line(session,
            "Usage: /register <username> <full-name> <country> <location> <email> <password>");
        return MAILBOXD_OK;
    }

    rc = parse_registration(cmd, &reg, staff_created ? 0 : 1);
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Registration fields too long.");
        return MAILBOXD_OK;
    }

    if (!staff_created && !mailboxd_password_plain_valid(reg.password)) {
        mailboxd_session_write_line(session,
            "Password must be 8-24 characters (- not allowed).");
        return MAILBOXD_OK;
    }

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_register_user(storage, &reg, &user);
    if (rc == MAILBOXD_ERR_BUSY) {
        mailboxd_session_write_line(session, "Username or nickname already taken.");
        return MAILBOXD_OK;
    }
    if (rc == MAILBOXD_ERR_INVALID) {
        mailboxd_session_write_line(session,
            "Invalid username (4-12 chars, one _ or one -, max 4 digits) or bad profile.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (staff_created) {
        snprintf(buf, sizeof(buf), "User '%s' created.",
                 mailboxd_user_display_name(&user));
    } else {
        snprintf(buf, sizeof(buf), "Registration received for '%s'.",
                 mailboxd_user_display_name(&user));
    }
    mailboxd_session_write_line(session, buf);
    snprintf(buf, sizeof(buf), "Name: %s", user.full_name);
    mailboxd_session_write_line(session, buf);
    if (staff_created) {
        mailboxd_session_write_line(session,
            "Account is inactive. Use /activate before the user can log in.");
    } else {
        const mailboxd_mail_config_t *mail_cfg = mailboxd_service_get_mail(service);

        (void)mailboxd_mail_notify_staff_registration(service, &reg, &user);
        mailboxd_session_write_line(session,
            "A Sysop or Admin must activate your account before you can log in.");
        if (mail_cfg != NULL && mail_cfg->enabled) {
            mailboxd_session_write_line(session,
                "Sysop and Admin were notified by mail.");
        }
    }
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_register(mailboxd_service_t *service,
                                   mailboxd_session_t *session,
                                   const mailboxd_parsed_command_t *cmd)
{
    if (!mailboxd_commands_registry_verb_allowed(mailboxd_session_user_level(session),
                                            "register") &&
        !(mailboxd_session_login_prompt(session) &&
          !mailboxd_session_logged_in(session))) {
        mailboxd_session_write_line(session,
            "Only guests or login-prompt sessions may self-register with /register.");
        return MAILBOXD_ERR_DENIED;
    }

    return cmd_register_user(service, session, cmd, 0);
}

static mailboxd_result_t cmd_createuser(mailboxd_service_t *service,
                                     mailboxd_session_t *session,
                                     const mailboxd_parsed_command_t *cmd)
{
    return cmd_register_user(service, session, cmd, 1);
}

static mailboxd_result_t cmd_changeme(mailboxd_service_t *service,
                                   mailboxd_session_t *session,
                                   const mailboxd_parsed_command_t *cmd)
{
    mailboxd_storage_t *storage;
    mailboxd_user_registration_t reg;
    mailboxd_user_record_t user;
    const char *old_password;
    const char *new_password;
    const char *username;
    mailboxd_result_t rc;

    if (mailboxd_session_is_guest(session)) {
        mailboxd_session_write_line(session,
            "Only registered users may use /changeme.");
        return MAILBOXD_ERR_DENIED;
    }

    if (cmd->argc < 6) {
        mailboxd_session_write_line(session,
            "Usage: /changeme <oldpass> <newpass> <full-name> <country> <location> <email>");
        return MAILBOXD_OK;
    }

    username = mailboxd_session_username(session);
    if (username == NULL || username[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    rc = parse_changeme(cmd, username, &reg, &old_password, &new_password);
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Profile fields too long.");
        return MAILBOXD_OK;
    }

    if (!mailboxd_password_plain_valid(new_password)) {
        mailboxd_session_write_line(session,
            "New password must be 8-24 characters (- not allowed).");
        return MAILBOXD_OK;
    }

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_find_user(storage, reg.username, &user);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "Account not found.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    {
        const mailboxd_session_record_t *rec = mailboxd_session_record(session);

        if (rec == NULL || user.id != rec->user_id) {
            mailboxd_session_write_line(session,
                "You can only change your own account.");
            return MAILBOXD_OK;
        }

        if (!mailboxd_user_profile_valid(&reg)) {
            mailboxd_session_write_line(session,
                "Invalid profile (check name, country, location, email).");
            return MAILBOXD_OK;
        }
    }

    if (user.password[0] == '\0') {
        mailboxd_session_write_line(session,
            "Account has no password. Ask Sysop or Admin to set one.");
        return MAILBOXD_OK;
    }

    if (!mailboxd_password_match(user.password, old_password)) {
        mailboxd_session_write_line(session, "Old password incorrect.");
        return MAILBOXD_OK;
    }

    mailboxd_strlcpy(user.full_name, reg.full_name, sizeof(user.full_name));
    mailboxd_strlcpy(user.country, reg.country, sizeof(user.country));
    mailboxd_strlcpy(user.location, reg.location, sizeof(user.location));
    mailboxd_strlcpy(user.email, reg.email, sizeof(user.email));

    rc = mailboxd_password_hash(new_password, user.password, sizeof(user.password));
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Could not store new password.");
        return rc;
    }

    rc = mailboxd_storage_update_user(storage, &user);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    mailboxd_session_write_line(session, "Account updated.");
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_userchange(mailboxd_service_t *service,
                                       mailboxd_session_t *session,
                                       const mailboxd_parsed_command_t *cmd)
{
    mailboxd_storage_t *storage;
    mailboxd_user_registration_t reg;
    mailboxd_user_record_t user;
    const char *new_password;
    mailboxd_user_level_t actor_level;
    mailboxd_result_t rc;
    char buf[160];

    actor_level = mailboxd_session_user_level(session);
    if (!mailboxd_user_level_is_sysop_or_admin(actor_level)) {
        cmd_deny_privilege(session);
        return MAILBOXD_ERR_DENIED;
    }

    if (cmd->argc < 7) {
        mailboxd_session_write_line(session,
            "Usage: /changeuser <user> <newpass> <full-name> <country> <location> <email>");
        return MAILBOXD_OK;
    }

    rc = parse_userchange(cmd, &reg, &new_password);
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Profile fields too long.");
        return MAILBOXD_OK;
    }

    if (!mailboxd_password_plain_valid(new_password)) {
        mailboxd_session_write_line(session,
            "New password must be 8-24 characters (- not allowed).");
        return MAILBOXD_OK;
    }

    if (!mailboxd_user_profile_valid(&reg)) {
        mailboxd_session_write_line(session,
            "Invalid profile (check name, country, location, email).");
        return MAILBOXD_OK;
    }

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_find_user(storage, reg.username, &user);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "Unknown user.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (str_ieq(user.username, mailboxd_session_username(session))) {
        mailboxd_session_write_line(session,
            "Use /changeme to update your own account.");
        return MAILBOXD_OK;
    }

    if (mailboxd_user_level_is_sysop(user.level)) {
        mailboxd_session_write_line(session,
            "The Sysop account cannot be changed with /changeuser.");
        return MAILBOXD_OK;
    }

    if (!mailboxd_commands_registry_may_userchange(actor_level, user.level)) {
        if (actor_level == MAILBOXD_LEVEL_ADMIN &&
            user.level == MAILBOXD_LEVEL_ADMIN) {
            mailboxd_session_write_line(session,
                "Admins cannot change other Admins. Sysop only.");
        } else {
            cmd_deny_privilege(session);
        }
        return MAILBOXD_ERR_DENIED;
    }

    mailboxd_strlcpy(user.full_name, reg.full_name, sizeof(user.full_name));
    mailboxd_strlcpy(user.country, reg.country, sizeof(user.country));
    mailboxd_strlcpy(user.location, reg.location, sizeof(user.location));
    mailboxd_strlcpy(user.email, reg.email, sizeof(user.email));

    rc = mailboxd_password_hash(new_password, user.password, sizeof(user.password));
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Could not store new password.");
        return rc;
    }

    rc = mailboxd_storage_update_user(storage, &user);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    snprintf(buf, sizeof(buf), "Updated account '%s'.",
             mailboxd_user_display_name(&user));
    mailboxd_session_write_line(session, buf);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_userdelete(mailboxd_service_t *service,
                                       mailboxd_session_t *session,
                                       const mailboxd_parsed_command_t *cmd)
{
    mailboxd_user_record_t target;
    mailboxd_user_level_t actor_level;
    char buf[128];
    mailboxd_result_t rc;

    actor_level = mailboxd_session_user_level(session);
    if (!mailboxd_user_level_is_sysop(actor_level)) {
        cmd_deny_privilege(session);
        return MAILBOXD_ERR_DENIED;
    }

    if (cmd->argc < 1 || cmd->argv[0] == NULL || cmd->argv[0][0] == '\0') {
        mailboxd_session_write_line(session, "Usage: /deleteuser <username>");
        return MAILBOXD_OK;
    }

    rc = cmd_lookup_user(service, cmd->argv[0], &target);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "Unknown user.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (str_ieq(target.username, mailboxd_session_username(session))) {
        mailboxd_session_write_line(session, "You cannot delete your own account.");
        return MAILBOXD_OK;
    }

    if (!mailboxd_commands_registry_may_userdelete(actor_level, target.level)) {
        mailboxd_session_write_line(session,
            "The Sysop account is permanent and cannot be deleted.");
        return MAILBOXD_OK;
    }

    rc = mailboxd_storage_delete_user(mailboxd_service_get_storage(service), target.id);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    snprintf(buf, sizeof(buf), "Deleted account '%s'.",
             mailboxd_user_display_name(&target));
    mailboxd_session_write_line(session, buf);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_login(mailboxd_service_t *service,
                                mailboxd_session_t *session,
                                const mailboxd_parsed_command_t *cmd)
{
    mailboxd_storage_t *storage;
    mailboxd_user_record_t user;
    const mailboxd_auth_config_t *auth;
    const char *guest_prefix;
    char username[MAILBOXD_USER_NAME_MAX];
    unsigned guest_slot;
    mailboxd_result_t rc;

    if (cmd->argc < 1 || cmd->argv[0] == NULL || cmd->argv[0][0] == '\0') {
        mailboxd_session_write_line(session,
            "Usage: /login <username> <password>");
        return MAILBOXD_OK;
    }

    auth = mailboxd_service_get_auth(service);
    guest_prefix = auth != NULL ? auth->guest_prefix : MAILBOXD_AUTH_DEFAULT_GUEST_PREFIX;

    mailboxd_strlcpy(username, cmd->argv[0], sizeof(username));
    mailboxd_username_normalize(username);

    if (mailboxd_guest_slot_from_username(guest_prefix, username, &guest_slot)) {
        if (auth != NULL && auth->auto_login) {
            mailboxd_session_write_line(session,
                "Guest access is automatic on connect (auto_login). "
                "/login is for registered accounts only.");
        } else {
            mailboxd_session_write_line(session,
                "/login is for registered accounts only.");
        }
        return MAILBOXD_OK;
    }

    if (cmd->argc < 2 || cmd->argv[1] == NULL || cmd->argv[1][0] == '\0') {
        mailboxd_session_write_line(session,
            "Usage: /login <username> <password>");
        return MAILBOXD_OK;
    }

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_find_user(storage, username, &user);

    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "Unknown user.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (mailboxd_user_level_is_guest(user.level)) {
        mailboxd_session_write_line(session,
            "Guests are ephemeral and use auto-login on connect.");
        return MAILBOXD_OK;
    }

    if (!user.active) {
        mailboxd_session_write_line(session,
            "Account pending activation by Sysop or Admin.");
        return MAILBOXD_OK;
    }

    if (user.password[0] == '\0') {
        mailboxd_session_write_line(session,
            "Account has no password. Contact Sysop or Admin.");
        return MAILBOXD_OK;
    }

    if (!mailboxd_password_match(user.password, cmd->argv[1])) {
        const mailboxd_session_record_t *rec = mailboxd_session_record(session);

        mailboxd_security_log_write("login_fail ip=%s user=%s transport=%s",
                                 rec != NULL && rec->remote[0] != '\0' ?
                                 rec->remote : "?",
                                 username,
                                 rec != NULL && rec->transport[0] != '\0' ?
                                 rec->transport : "?");
        if (rec != NULL && rec->remote[0] != '\0') {
            mailboxd_security_ban_login_fail(rec->remote, rec->transport);
        }
        mailboxd_session_write_line(session, "Invalid password.");
        return MAILBOXD_OK;
    }

    if (mailboxd_service_find_registered_session(service, user.id, session) !=
        NULL) {
        mailboxd_session_write_line(session,
            "That account is already logged in elsewhere.");
        return MAILBOXD_OK;
    }

    rc = mailboxd_session_switch_user(session, &user);
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Login failed.");
        return rc;
    }

    {
        time_t since_login = user.last_login_at;
        const mailboxd_texts_config_t *texts;

        texts = mailboxd_service_get_texts(service);
        if (texts != NULL) {
            (void)mailboxd_texts_send_motd(texts, session);
        }

        mailboxd_mail_announce_since_last_login(service, session, since_login);

        user.last_login_at = time(NULL);
        (void)mailboxd_storage_update_user(storage, &user);
    }
    mailboxd_session_show_prompt(session);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_chat(mailboxd_service_t *service,
                               mailboxd_session_t *session,
                               const mailboxd_parsed_command_t *cmd)
{
    const mailboxd_chat_config_t *chat;
    unsigned channel_index;
    unsigned current;
    const char *name;
    char line[MAILBOXD_CHAT_CHANNEL_NAME_MAX + 48];
    mailboxd_result_t rc;

    if (mailboxd_session_is_guest(session)) {
        mailboxd_session_write_line(session, "Guests cannot use chat.");
        return MAILBOXD_ERR_DENIED;
    }

    chat = mailboxd_service_get_chat(service);
    if (chat == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (cmd->argc == 0) {
        mailboxd_chat_list_channels(session, chat);
        return MAILBOXD_OK;
    }

    if (str_ieq(cmd->argv[0], "show")) {
        if (cmd->argc != 1) {
            mailboxd_session_write_line(session, "Usage: /chat show");
            return MAILBOXD_OK;
        }
        mailboxd_chat_show_channel(service, session);
        return MAILBOXD_OK;
    }

    if (str_ieq(cmd->argv[0], "showall")) {
        if (cmd->argc != 1) {
            mailboxd_session_write_line(session, "Usage: /chat showall");
            return MAILBOXD_OK;
        }
        mailboxd_chat_show_all(service, session);
        return MAILBOXD_OK;
    }

    rc = mailboxd_chat_resolve_channel(chat, cmd->argv[0], &channel_index);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(session, "Unknown chat channel.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    current = mailboxd_session_chat_channel(session);
    if (current == channel_index) {
        snprintf(line, sizeof(line), "Already in channel %u %s.",
                 channel_index,
                 mailboxd_chat_channel_name(chat, channel_index));
        mailboxd_session_write_line(session, line);
        return MAILBOXD_OK;
    }

    rc = mailboxd_session_join_chat_channel(session, channel_index);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    name = mailboxd_chat_channel_name(chat, channel_index);
    snprintf(line, sizeof(line), "Channel %u: %s", channel_index, name);
    mailboxd_session_write_line(session, line);
    mailboxd_session_write_line(session,
        "Each line is a message; /leave or /main to exit.");
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_conference(mailboxd_service_t *service,
                                     mailboxd_session_t *session,
                                     const mailboxd_parsed_command_t *cmd)
{
    char topic[MAILBOXD_CONFERENCE_TOPIC_MAX];
    const char *user;
    size_t i;
    size_t pos = 0;

    if (mailboxd_session_is_guest(session)) {
        mailboxd_session_write_line(session, "Guests cannot use conference.");
        return MAILBOXD_ERR_DENIED;
    }

    if (cmd->argc < 2) {
        mailboxd_session_write_line(session,
            "Usage: /conference <topic> <user>");
        return MAILBOXD_OK;
    }

    user = cmd->argv[cmd->argc - 1];
    topic[0] = '\0';

    for (i = 0; i + 1 < cmd->argc; i++) {
        size_t part_len = strlen(cmd->argv[i]);

        if (i > 0) {
            if (pos + 1 >= sizeof(topic)) {
                mailboxd_session_write_line(session, "Topic too long.");
                return MAILBOXD_OK;
            }
            topic[pos++] = ' ';
            topic[pos] = '\0';
        }

        if (pos + part_len >= sizeof(topic)) {
            mailboxd_session_write_line(session, "Topic too long.");
            return MAILBOXD_OK;
        }

        memcpy(topic + pos, cmd->argv[i], part_len);
        pos += part_len;
        topic[pos] = '\0';
    }

    return mailboxd_conference_start(service, session, topic, user);
}

static void mail_join_subject(const mailboxd_parsed_command_t *cmd,
                              char *out, size_t out_len)
{
    size_t i;
    size_t pos = 0;

    if (out == NULL || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if (cmd == NULL || cmd->argc < 3) {
        return;
    }

    for (i = 2; i < cmd->argc; i++) {
        size_t part_len;

        if (cmd->argv[i] == NULL) {
            continue;
        }

        if (pos > 0) {
            if (pos + 1 >= out_len) {
                break;
            }
            out[pos++] = ' ';
            out[pos] = '\0';
        }

        part_len = strlen(cmd->argv[i]);
        if (pos + part_len >= out_len) {
            part_len = out_len - pos - 1;
        }
        memcpy(out + pos, cmd->argv[i], part_len);
        pos += part_len;
        out[pos] = '\0';
    }
}

static mailboxd_result_t cmd_mail(mailboxd_service_t *service,
                               mailboxd_session_t *session,
                               const mailboxd_parsed_command_t *cmd)
{
    char subject[MAILBOXD_MAIL_SUBJECT_MAX + 1];
    mailboxd_result_t rc;

    if (mailboxd_session_is_guest(session)) {
        mailboxd_session_write_line(session, "Guests cannot use mail.");
        return MAILBOXD_ERR_DENIED;
    }

    if (cmd->argc == 0) {
        mailboxd_mail_list_inbox(service, session);
        return MAILBOXD_OK;
    }

    if (str_ieq(cmd->argv[0], "list")) {
        unsigned from = 1;
        unsigned to = 0;

        if (cmd->argc >= 2) {
            if (!mailboxd_mail_parse_list_range(cmd->argv[1], &from, &to)) {
                mailboxd_session_write_line(session,
                    "Usage: /mail list [<from>-<to>]   e.g. list 1-15  list 5-20");
                return MAILBOXD_OK;
            }
        }

        mailboxd_mail_list_inbox_range(service, session, from, to);
        return MAILBOXD_OK;
    }

    if (str_ieq(cmd->argv[0], "delete") || str_ieq(cmd->argv[0], "del")) {
        unsigned from = 1;
        unsigned to = 0;

        if (cmd->argc < 2 || cmd->argv[1] == NULL) {
            mailboxd_session_write_line(session,
                "Usage: /mail delete <n|from-to>   e.g. delete 3  delete 5-20");
            return MAILBOXD_OK;
        }

        if (!mailboxd_mail_parse_list_range(cmd->argv[1], &from, &to)) {
            mailboxd_session_write_line(session,
                "Usage: /mail delete <n|from-to>   e.g. delete 3  delete 5-20");
            return MAILBOXD_OK;
        }

        return mailboxd_mail_delete_range(service, session, from, to);
    }

    if (str_ieq(cmd->argv[0], "recycle")) {
        return mailboxd_mail_recycle_empty(service, session);
    }

    if (str_ieq(cmd->argv[0], "read")) {
        unsigned index;

        if (cmd->argc < 2 || cmd->argv[1] == NULL) {
            mailboxd_session_write_line(session, "Usage: /mail read <n>");
            return MAILBOXD_OK;
        }

        index = (unsigned)strtoul(cmd->argv[1], NULL, 10);
        if (index == 0) {
            mailboxd_session_write_line(session, "Usage: /mail read <n>");
            return MAILBOXD_OK;
        }

        return mailboxd_mail_read(service, session, index);
    }

    if (str_ieq(cmd->argv[0], "send")) {
        if (cmd->argc < 3) {
            mailboxd_session_write_line(session,
                "Usage: /mail send <user> <subject>");
            return MAILBOXD_OK;
        }

        mail_join_subject(cmd, subject, sizeof(subject));
        if (subject[0] == '\0') {
            mailboxd_session_write_line(session,
                "Usage: /mail send <user> <subject>");
            return MAILBOXD_OK;
        }

        rc = mailboxd_session_mail_compose_start(session, cmd->argv[1], subject);
        if (rc == MAILBOXD_ERR_INVALID) {
            mailboxd_session_write_line(session, "Subject too long.");
            return MAILBOXD_OK;
        }
        if (rc != MAILBOXD_OK) {
            return rc;
        }

        mailboxd_session_write_line(session, "Compose body. /mail done to send.");
        return MAILBOXD_OK;
    }

    if (str_ieq(cmd->argv[0], "done")) {
        const char *body;
        const char *to_user;
        const char *mail_subject;

        if (!mailboxd_session_mail_composing(session)) {
            mailboxd_session_write_line(session, "Not composing mail.");
            return MAILBOXD_OK;
        }

        to_user = mailboxd_session_mail_compose_to(session);
        mail_subject = mailboxd_session_mail_compose_subject(session);
        body = mailboxd_session_mail_compose_body(session);

        rc = mailboxd_mail_deliver(service, mailboxd_session_display_name(session),
                                to_user, mail_subject, body);

        if (rc == MAILBOXD_ERR_NOT_FOUND) {
            mailboxd_session_write_line(session, "Unknown recipient.");
            return MAILBOXD_OK;
        }
        if (rc == MAILBOXD_ERR_DENIED) {
            mailboxd_session_write_line(session, "Cannot mail that user.");
            return MAILBOXD_OK;
        }
        if (rc != MAILBOXD_OK) {
            mailboxd_session_write_line(session, "Send failed.");
            return rc;
        }

        mailboxd_session_write_line(session, "Message sent.");
        mailboxd_session_leave_area(session);
        mailboxd_session_show_prompt(session);
        return MAILBOXD_OK;
    }

    if (str_ieq(cmd->argv[0], "cancel")) {
        if (mailboxd_session_mail_composing(session)) {
            mailboxd_session_leave_area(session);
            mailboxd_session_write_line(session, "Compose cancelled.");
        } else {
            mailboxd_session_write_line(session, "Not composing mail.");
        }
        return MAILBOXD_OK;
    }

    mailboxd_session_write_line(session, "Unknown /mail subcommand. Try /help mail.");
    return MAILBOXD_OK;
}

static void cmd_emit_current_area(mailboxd_session_t *session)
{
    const char *name = mailboxd_session_area_name(mailboxd_session_area(session));
    char line[64];

    snprintf(line, sizeof(line), "%c%s.",
             (char)toupper((unsigned char)name[0]), name + 1);
    mailboxd_session_write_line(session, line);
    mailboxd_session_show_prompt(session);
}

static mailboxd_result_t cmd_leave(mailboxd_session_t *session)
{
    mailboxd_session_area_t before = mailboxd_session_area(session);

    mailboxd_session_leave_area(session);
    if (mailboxd_session_area(session) != before) {
        cmd_emit_current_area(session);
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_main(mailboxd_session_t *session)
{
    mailboxd_session_area_t before = mailboxd_session_area(session);

    mailboxd_session_go_main(session);
    if (before != MAILBOXD_AREA_MAIN) {
        cmd_emit_current_area(session);
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_exit(mailboxd_session_t *session)
{
    mailboxd_session_write_line(session, "Goodbye.");
    return MAILBOXD_SESSION_END;
}

static mailboxd_result_t cmd_broadcast(mailboxd_service_t *service,
                                    mailboxd_session_t *session,
                                    const mailboxd_parsed_command_t *cmd)
{
    char message[MAILBOXD_BROADCAST_MESSAGE_MAX + 1];
    size_t off = 0;
    int i;
    mailboxd_result_t rc;

    if (cmd->argc < 1) {
        cmd_registry_usage(session, "broadcast");
        return MAILBOXD_OK;
    }

    message[0] = '\0';
    for (i = 0; i < (int)cmd->argc; i++) {
        size_t part_len = strlen(cmd->argv[i]);

        if (off > 0 && off < sizeof(message) - 1) {
            message[off++] = ' ';
        }
        if (off + part_len >= sizeof(message)) {
            part_len = sizeof(message) - off - 1;
        }
        if (part_len > 0) {
            memcpy(message + off, cmd->argv[i], part_len);
            off += part_len;
        }
    }
    message[off] = '\0';

    if (message[0] == '\0') {
        mailboxd_session_write_line(session, "Missing announce message.");
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_broadcast_announce(service, session, message);
    if (rc == MAILBOXD_ERR_UNSUPPORTED) {
        mailboxd_session_write_line(session, "Broadcast is disabled.");
        return MAILBOXD_OK;
    }
    if (rc == MAILBOXD_ERR_INVALID) {
        mailboxd_session_write_line(session, "Message too long.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Broadcast failed.");
        return MAILBOXD_ERR_IO;
    }

    mailboxd_session_write_line(session, "Broadcast sent.");
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_shutdown(mailboxd_service_t *service,
                                   mailboxd_session_t *session)
{
    if (!mailboxd_user_level_is_sysop(mailboxd_session_user_level(session))) {
        cmd_deny_privilege(session);
        return MAILBOXD_ERR_DENIED;
    }

    mailboxd_security_log_write("shutdown ip=%s user=%s transport=%s",
                             mailboxd_session_record(session) != NULL &&
                             mailboxd_session_record(session)->remote[0] != '\0' ?
                             mailboxd_session_record(session)->remote : "?",
                             mailboxd_session_display_name(session),
                             mailboxd_session_record(session) != NULL ?
                             mailboxd_session_record(session)->transport : "?");
    mailboxd_session_write_line(session, "Shutting down MailboxD.");
    mailboxd_service_request_shutdown(service, 0);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_restart(mailboxd_service_t *service,
                                  mailboxd_session_t *session)
{
    if (!mailboxd_user_level_is_sysop(mailboxd_session_user_level(session))) {
        cmd_deny_privilege(session);
        return MAILBOXD_ERR_DENIED;
    }

    mailboxd_security_log_write("restart ip=%s user=%s transport=%s",
                             mailboxd_session_record(session) != NULL &&
                             mailboxd_session_record(session)->remote[0] != '\0' ?
                             mailboxd_session_record(session)->remote : "?",
                             mailboxd_session_display_name(session),
                             mailboxd_session_record(session) != NULL ?
                             mailboxd_session_record(session)->transport : "?");
    mailboxd_session_write_line(session, "Restarting MailboxD.");
    mailboxd_service_request_shutdown(service, 1);
    return MAILBOXD_OK;
}

static mailboxd_result_t cmd_version(mailboxd_service_t *service,
                                  mailboxd_session_t *session)
{
    const mailboxd_texts_config_t *texts = mailboxd_service_get_texts(service);

    if (texts == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    return mailboxd_texts_send_version(texts, session);
}

static mailboxd_result_t cmd_clear(mailboxd_session_t *session)
{
    return mailboxd_session_clear_terminal(session);
}

static mailboxd_result_t cmd_echo(mailboxd_session_t *session,
                             const mailboxd_parsed_command_t *cmd)
{
    char buf[48];

    if (cmd->argc < 2) {
        snprintf(buf, sizeof(buf), "Input echo is %s.",
                 mailboxd_bool_to_string(mailboxd_session_input_echo(session)));
        mailboxd_session_write_line(session, buf);
        return MAILBOXD_OK;
    }

    if (mailboxd_bool_is_true(cmd->argv[1])) {
        return mailboxd_session_set_input_echo(session, 1);
    }

    if (mailboxd_bool_is_false(cmd->argv[1])) {
        return mailboxd_session_set_input_echo(session, 0);
    }

    mailboxd_session_write_line(session, "Usage: /echo yes|no");
    return MAILBOXD_ERR_INVALID;
}

typedef struct monitor_list_ctx {
    mailboxd_session_t *requester;
    unsigned count;
} monitor_list_ctx_t;

static void monitor_list_visitor(mailboxd_session_t *session, void *userdata)
{
    monitor_list_ctx_t *ctx = (monitor_list_ctx_t *)userdata;
    const mailboxd_session_record_t *rec;
    const char *user;
    const char *plugin;
    char line[96];

    if (ctx == NULL || session == NULL || !mailboxd_session_logged_in(session)) {
        return;
    }

    if (!mailboxd_session_is_interactive_user(session)) {
        return;
    }

    if (mailboxd_session_hidden_from_who(session)) {
        return;
    }

    user = mailboxd_session_display_name(session);
    rec = mailboxd_session_record(session);
    plugin = (rec != NULL && rec->transport[0] != '\0')
                 ? rec->transport
                 : (session->transport != NULL ? session->transport->name : "?");
    snprintf(line, sizeof(line), "  %s@%s", user, plugin);
    mailboxd_session_write_line(ctx->requester, line);
    ctx->count++;
}

static void monitor_show_cmds(mailboxd_session_t *session)
{
    mailboxd_session_write_line(session, "Monitor:");
    mailboxd_session_write_line(session,
        "  /monitor on|off     — start/stop live log follow");
    mailboxd_session_write_line(session,
        "  /monitor list       — status + visible online users");
    mailboxd_session_write_line(session,
        "  /monitor meet <user> [force]  — conference invite (or force)");
    mailboxd_session_write_line(session,
        "  /monitor cmds       — this menu + Sysop verbs");
    mailboxd_session_write_line(session,
        "While monitor is on (or invisible-sysop=yes), Sysop stays hidden from /who.");
    mailboxd_session_write_line(session,
        "Sysop verbs (use as normal /verb):");
    mailboxd_commands_registry_show_sysop_cmds(session);
}

static mailboxd_result_t cmd_monitor(mailboxd_service_t *service,
                                  mailboxd_session_t *session,
                                  const mailboxd_parsed_command_t *cmd)
{
    const char *sub;
    monitor_list_ctx_t list_ctx;
    const mailboxd_monitor_config_t *mcfg;
    char line[128];

    if (!mailboxd_monitor_enabled()) {
        mailboxd_session_write_line(session, "Monitor is disabled ([monitor] enabled=no).");
        return MAILBOXD_ERR_DENIED;
    }

    if (!mailboxd_monitor_session_may_use(session) &&
        !mailboxd_commands_registry_verb_allowed(
            mailboxd_session_user_level(session), "monitor")) {
        mailboxd_session_write_line(session, "Monitor access denied.");
        return MAILBOXD_ERR_DENIED;
    }

    sub = (cmd->argc >= 1) ? cmd->argv[0] : "list";

    if (str_ieq(sub, "on")) {
        return mailboxd_monitor_set_active(session, 1);
    }

    if (str_ieq(sub, "off")) {
        return mailboxd_monitor_set_active(session, 0);
    }

    if (str_ieq(sub, "cmd") || str_ieq(sub, "cmds") || str_ieq(sub, "commands") ||
        str_ieq(sub, "help") || str_ieq(sub, "?")) {
        monitor_show_cmds(session);
        return MAILBOXD_OK;
    }

    if (str_ieq(sub, "meet")) {
        int force = 0;
        const char *user;

        if (cmd->argc < 2) {
            mailboxd_session_write_line(session,
                "Usage: /monitor meet <user> [force]");
            return MAILBOXD_ERR_INVALID;
        }
        user = cmd->argv[1];
        if (cmd->argc >= 3 && str_ieq(cmd->argv[2], "force")) {
            force = 1;
        }
        if (!mailboxd_monitor_is_active(session)) {
            mailboxd_session_write_line(session,
                "Turn monitor on first: /monitor on");
            return MAILBOXD_ERR_DENIED;
        }
        return mailboxd_conference_monitor_meet(service, session, user, force);
    }

    if (!str_ieq(sub, "list") && cmd->argc >= 1) {
        mailboxd_session_write_line(session,
            "Usage: /monitor on|off|list|cmds|meet <user> [force]  — /help monitor");
        return MAILBOXD_ERR_INVALID;
    }

    mcfg = mailboxd_monitor_config_get();
    snprintf(line, sizeof(line), "Monitor: %s",
             mailboxd_monitor_is_active(session) ? "on" : "off");
    mailboxd_session_write_line(session, line);
    if (mcfg != NULL) {
        snprintf(line, sizeof(line),
                 "Follow: mailboxd=%s security=%s invisible-sysop=%s "
                 "invite_timeout=%us",
                 mcfg->follow_mailboxd ? "yes" : "no",
                 mcfg->follow_security ? "yes" : "no",
                 mcfg->invisible_sysop ? "yes" : "no",
                 mcfg->invite_timeout_sec);
        mailboxd_session_write_line(session, line);
    }
    if (mailboxd_session_hidden_from_who(session)) {
        mailboxd_session_write_line(session, "You are hidden from /who.");
    }
    mailboxd_session_write_line(session, "Online (visible):");
    list_ctx.requester = session;
    list_ctx.count = 0;
    mailboxd_service_visit_sessions(service, monitor_list_visitor, &list_ctx);
    if (list_ctx.count == 0) {
        mailboxd_session_write_line(session, "  (none)");
    }
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_command_dispatch(mailboxd_service_t *service,
                                        mailboxd_session_t *session,
                                        const mailboxd_parsed_command_t *cmd)
{
    if (cmd == NULL || service == NULL || session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (cmd->scope != MAILBOXD_CMD_SCOPE_MAILBOXD) {
        return MAILBOXD_ERR_INVALID;
    }

    (void)mailboxd_session_command_gap(session);

    if (cmd->verb == NULL || cmd->verb[0] == '\0') {
        return cmd_help(session, cmd);
    }

    if (str_ieq(cmd->verb, "help") || str_ieq(cmd->verb, "?")) {
        return cmd_help(session, cmd);
    }

    {
        mailboxd_result_t access = cmd_check_access(session, cmd->verb);
        if (access != MAILBOXD_OK) {
            return access;
        }
    }

    if (str_ieq(cmd->verb, "news")) {
        return cmd_news(service, session);
    }

    if (str_ieq(cmd->verb, "banner") || str_ieq(cmd->verb, "loginmsg")) {
        return cmd_banner(service, session);
    }

    if (str_ieq(cmd->verb, "motd")) {
        return cmd_motd(service, session);
    }

    if (str_ieq(cmd->verb, "rules") || str_ieq(cmd->verb, "legal")) {
        return cmd_rules(service, session);
    }

    /* Plugin command intercept — try all plugins first */
    if (mailboxd_service_try_plugin_command(service, session, cmd) == MAILBOXD_OK) {
        return MAILBOXD_OK;
    }

    if (str_ieq(cmd->verb, "who") || str_ieq(cmd->verb, "online")) {
        return cmd_who(service, session);
    }

    if (str_ieq(cmd->verb, "monitor") || str_ieq(cmd->verb, "mon")) {
        return cmd_monitor(service, session, cmd);
    }



    if (str_ieq(cmd->verb, "users")) {
        return cmd_users(service, session);
    }

    if (str_ieq(cmd->verb, "session") || str_ieq(cmd->verb, "info")) {
        return cmd_session(session);
    }

    if (str_ieq(cmd->verb, "version") || str_ieq(cmd->verb, "ver")) {
        return cmd_version(service, session);
    }

    if (str_ieq(cmd->verb, "leave") || str_ieq(cmd->verb, "back")) {
        return cmd_leave(session);
    }

    if (str_ieq(cmd->verb, "main")) {
        return cmd_main(session);
    }

    if (str_ieq(cmd->verb, "menu")) {
        if (cmd->argc == 0) {
            mailboxd_commands_registry_show_menu(session);
            return MAILBOXD_OK;
        }
        return cmd_help_topic(session, cmd->argv[0]);
    }

    if (str_ieq(cmd->verb, "index")) {
        if (cmd->argc == 0) {
            mailboxd_commands_registry_show_index(session);
            return MAILBOXD_OK;
        }
        return cmd_help_topic(session, cmd->argv[0]);
    }

    if (str_ieq(cmd->verb, "alias")) {
        if (cmd->argc == 0) {
            mailboxd_commands_registry_show_aliases(session);
            return MAILBOXD_OK;
        }
        return cmd_help_topic(session, cmd->argv[0]);
    }

    if (str_ieq(cmd->verb, "clear") || str_ieq(cmd->verb, "cls") ||
        str_ieq(cmd->verb, "reset")) {
        return cmd_clear(session);
    }

    if (str_ieq(cmd->verb, "echo")) {
        return cmd_echo(session, cmd);
    }

    if (str_ieq(cmd->verb, "chat")) {
        return cmd_chat(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "conference") || str_ieq(cmd->verb, "meeting")) {
        return cmd_conference(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "mail")) {
        return cmd_mail(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "login")) {
        return cmd_login(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "register")) {
        return cmd_register(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "changeme")) {
        return cmd_changeme(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "changeuser") || str_ieq(cmd->verb, "userchange")) {
        return cmd_userchange(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "deleteuser") || str_ieq(cmd->verb, "userdelete")) {
        return cmd_userdelete(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "usercreate") || str_ieq(cmd->verb, "createuser")) {
        return cmd_createuser(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "activate")) {
        return cmd_activate(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "promote")) {
        return cmd_promote(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "demote")) {
        return cmd_demote(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "delete") || str_ieq(cmd->verb, "del")) {
        return cmd_delete(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "deleteme")) {
        return cmd_deleteme(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "broadcast") || str_ieq(cmd->verb, "announce")) {
        return cmd_broadcast(service, session, cmd);
    }

    if (str_ieq(cmd->verb, "shutdown")) {
        return cmd_shutdown(service, session);
    }

    if (str_ieq(cmd->verb, "restart")) {
        return cmd_restart(service, session);
    }

    if (str_ieq(cmd->verb, "exit") || str_ieq(cmd->verb, "logout") ||
        str_ieq(cmd->verb, "bye") || str_ieq(cmd->verb, "quit")) {
        return cmd_exit(session);
    }

    mailboxd_session_write_line(session, cmd_help_unknown(session));
    return MAILBOXD_ERR_NOT_FOUND;
}
