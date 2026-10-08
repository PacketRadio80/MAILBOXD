/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/conference.h"
#include "mailboxd/chat.h"
#include "mailboxd/messages.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/storage.h"
#include "mailboxd/auth.h"
#include "mailboxd/monitor.h"
#include "mailboxd/traffic.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

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

static int invite_reply_yes(const char *line)
{
    return line != NULL &&
           (str_ieq(line, "y") || str_ieq(line, "yes"));
}

static int invite_reply_no(const char *line)
{
    return line != NULL &&
           (str_ieq(line, "n") || str_ieq(line, "no"));
}

static int topic_char_ok(char ch)
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == ' ' || ch == '-' || ch == '_';
}

static int conference_topic_valid(const char *topic)
{
    size_t len;
    size_t i;

    if (topic == NULL) {
        return 0;
    }

    len = strlen(topic);
    if (len < 2 || len >= MAILBOXD_CONFERENCE_TOPIC_MAX) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        if (!topic_char_ok(topic[i])) {
            return 0;
        }
    }

    return 1;
}

typedef struct find_user_session_ctx {
    const char *username;
    mailboxd_session_t *found;
} find_user_session_ctx_t;

static void find_user_session_visitor(mailboxd_session_t *session, void *userdata)
{
    find_user_session_ctx_t *ctx = (find_user_session_ctx_t *)userdata;

    if (ctx == NULL || session == NULL || ctx->found != NULL) {
        return;
    }

    if (!mailboxd_session_logged_in(session) || mailboxd_session_is_guest(session)) {
        return;
    }

    if (str_ieq(mailboxd_session_username(session), ctx->username)) {
        ctx->found = session;
    }
}

static mailboxd_session_t *conference_find_online_user(mailboxd_service_t *service,
                                                    const char *username)
{
    find_user_session_ctx_t ctx;

    if (service == NULL || username == NULL || username[0] == '\0') {
        return NULL;
    }

    ctx.username = username;
    ctx.found = NULL;
    mailboxd_service_visit_sessions(service, find_user_session_visitor, &ctx);
    return ctx.found;
}

static int conference_session_active(const mailboxd_session_t *session)
{
    return session != NULL &&
           mailboxd_session_area(session) == MAILBOXD_AREA_CONFERENCE;
}

typedef struct invite_cancel_ctx {
    const char *from_username;
} invite_cancel_ctx_t;

static void invite_cancel_visitor(mailboxd_session_t *session, void *userdata)
{
    invite_cancel_ctx_t *ctx = (invite_cancel_ctx_t *)userdata;

    if (session == NULL || ctx == NULL || ctx->from_username == NULL) {
        return;
    }

    if (!mailboxd_conference_invite_pending(session)) {
        return;
    }

    if (!str_ieq(mailboxd_session_conference_invite_from(session),
                 ctx->from_username)) {
        return;
    }

    mailboxd_session_clear_conference_invite(session);
    mailboxd_session_write_line(session, "*** Conference invite cancelled.");
}

static void conference_cancel_invites_from(mailboxd_service_t *service,
                                           const char *from_username)
{
    invite_cancel_ctx_t ctx;

    if (service == NULL || from_username == NULL || from_username[0] == '\0') {
        return;
    }

    ctx.from_username = from_username;
    mailboxd_service_visit_sessions(service, invite_cancel_visitor, &ctx);
}

static void conference_notify_partner_left(mailboxd_session_t *session,
                                         const char *partner_username)
{
    mailboxd_service_t *service;
    mailboxd_session_t *partner;
    char line[MAILBOXD_LINE_MAX];

    if (session == NULL || partner_username == NULL ||
        partner_username[0] == '\0') {
        return;
    }

    service = mailboxd_session_service(session);
    if (service == NULL) {
        return;
    }

    partner = conference_find_online_user(service, partner_username);
    if (partner == NULL || !conference_session_active(partner)) {
        return;
    }

    if (!str_ieq(mailboxd_session_conference_partner(partner),
                 mailboxd_session_username(session))) {
        return;
    }

    mailboxd_session_clear_conference(partner);
    snprintf(line, sizeof(line), "*** %s left the conference.",
             mailboxd_session_display_name(session));
    mailboxd_session_write_line(partner, line);
    if (mailboxd_session_area(partner) == MAILBOXD_AREA_CONFERENCE) {
        (void)mailboxd_session_leave_area(partner);
    }
}

static mailboxd_result_t conference_activate(mailboxd_service_t *service,
                                          mailboxd_session_t *initiator,
                                          mailboxd_session_t *partner,
                                          const char *topic)
{
    char line[MAILBOXD_LINE_MAX];
    mailboxd_result_t rc;

    if (service == NULL || initiator == NULL || partner == NULL ||
        topic == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_session_join_conference(initiator, topic,
                                       mailboxd_session_username(partner));
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    rc = mailboxd_session_join_conference(partner, topic,
                                       mailboxd_session_username(initiator));
    if (rc != MAILBOXD_OK) {
        mailboxd_session_clear_conference(initiator);
        (void)mailboxd_session_leave_area(initiator);
        return rc;
    }

    snprintf(line, sizeof(line), "Conference: %s", topic);
    mailboxd_session_write_line(initiator, line);
    mailboxd_session_write_line(initiator,
        "Each line is a message; /leave or /main to exit.");

    snprintf(line, sizeof(line), "Conference: %s", topic);
    mailboxd_session_write_line(partner, line);
    mailboxd_session_write_line(partner,
        "Each line is a message; /leave or /main to exit.");

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_conference_start(mailboxd_service_t *service,
                                      mailboxd_session_t *initiator,
                                      const char *topic,
                                      const char *partner_spec)
{
    mailboxd_storage_t *storage;
    mailboxd_user_record_t partner_user;
    mailboxd_session_t *partner_session;
    char line[MAILBOXD_LINE_MAX];
    mailboxd_result_t rc;

    if (service == NULL || initiator == NULL || topic == NULL ||
        partner_spec == NULL || topic[0] == '\0' || partner_spec[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_session_is_guest(initiator)) {
        mailboxd_session_write_line(initiator, "Guests cannot use conference.");
        return MAILBOXD_ERR_DENIED;
    }

    if (!conference_topic_valid(topic)) {
        mailboxd_session_write_line(initiator, "Invalid conference topic.");
        return MAILBOXD_ERR_INVALID;
    }

    if (conference_session_active(initiator)) {
        mailboxd_session_write_line(initiator, "Already in a conference.");
        return MAILBOXD_OK;
    }

    if (mailboxd_conference_invite_pending(initiator)) {
        mailboxd_session_write_line(initiator,
            "Answer the pending conference invite first (y/n).");
        return MAILBOXD_OK;
    }

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_resolve_user(storage, partner_spec, &partner_user);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(initiator, "Unknown user.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (!partner_user.active) {
        mailboxd_session_write_line(initiator, "User is not active.");
        return MAILBOXD_OK;
    }

    if (mailboxd_user_level_is_guest(partner_user.level)) {
        mailboxd_session_write_line(initiator, "Cannot conference with a guest.");
        return MAILBOXD_OK;
    }

    if (str_ieq(partner_user.username, mailboxd_session_username(initiator))) {
        mailboxd_session_write_line(initiator, "Cannot conference with yourself.");
        return MAILBOXD_OK;
    }

    if (!mailboxd_session_conference_may_invite(initiator, partner_user.username)) {
        mailboxd_session_write_line(initiator,
            "Invite limit: 2 per user per 30 minutes.");
        return MAILBOXD_OK;
    }

    partner_session = conference_find_online_user(service, partner_user.username);
    if (partner_session == NULL) {
        mailboxd_session_write_line(initiator, "User is not online.");
        return MAILBOXD_OK;
    }

    if (conference_session_active(partner_session)) {
        mailboxd_session_write_line(initiator, "User is already in a conference.");
        return MAILBOXD_OK;
    }

    if (mailboxd_conference_invite_pending(partner_session)) {
        mailboxd_session_write_line(initiator,
            "User has a pending conference invite.");
        return MAILBOXD_OK;
    }

    mailboxd_session_set_conference_invite(partner_session,
                                        mailboxd_session_username(initiator),
                                        topic);
    mailboxd_session_conference_invite_sent(initiator, partner_user.username);

    snprintf(line, sizeof(line), "Conference invite: %s", topic);
    (void)mailboxd_msg_send_private(partner_session,
                                 mailboxd_session_display_name(initiator), line);
    mailboxd_session_write_line(partner_session, "Accept? y/n or yes/no");

    snprintf(line, sizeof(line), "Conference invite sent to %s.",
             mailboxd_user_display_name(&partner_user));
    mailboxd_session_write_line(initiator, line);

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_conference_reply_invite(mailboxd_service_t *service,
                                             mailboxd_session_t *session,
                                             const char *line)
{
    char from_username[MAILBOXD_USER_NAME_MAX];
    char topic[MAILBOXD_CONFERENCE_TOPIC_MAX];
    mailboxd_session_t *initiator;
    char msg[MAILBOXD_LINE_MAX];

    if (service == NULL || session == NULL || line == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (!mailboxd_conference_invite_pending(session)) {
        return MAILBOXD_ERR_INVALID;
    }

    if (!invite_reply_yes(line) && !invite_reply_no(line)) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    {
        const char *from_ptr = mailboxd_session_conference_invite_from(session);
        const char *topic_ptr = mailboxd_session_conference_invite_topic(session);

        if (from_ptr == NULL || from_ptr[0] == '\0' ||
            topic_ptr == NULL || topic_ptr[0] == '\0') {
            mailboxd_session_clear_conference_invite(session);
            return MAILBOXD_ERR_INVALID;
        }

        mailboxd_strlcpy(from_username, from_ptr, sizeof(from_username));
        mailboxd_strlcpy(topic, topic_ptr, sizeof(topic));
    }

    mailboxd_session_clear_conference_invite(session);

    if (invite_reply_no(line)) {
        initiator = conference_find_online_user(service, from_username);
        if (initiator != NULL) {
            snprintf(msg, sizeof(msg),
                     "meet status=declined user=%s",
                     mailboxd_session_display_name(session));
            (void)mailboxd_msg_send_system(initiator, msg);
        }
        mailboxd_session_write_line(session, "*** Conference invite declined.");
        return MAILBOXD_OK;
    }

    if (conference_session_active(session)) {
        mailboxd_session_write_line(session, "Already in a conference.");
        return MAILBOXD_OK;
    }

    initiator = conference_find_online_user(service, from_username);
    if (initiator == NULL) {
        mailboxd_session_write_line(session, "Inviter is no longer online.");
        return MAILBOXD_OK;
    }

    if (conference_session_active(initiator)) {
        mailboxd_session_write_line(session, "Inviter is already in a conference.");
        return MAILBOXD_OK;
    }

    if (conference_activate(service, initiator, session, topic) != MAILBOXD_OK) {
        mailboxd_session_write_line(session, "Conference could not be started.");
        return MAILBOXD_OK;
    }

    snprintf(msg, sizeof(msg), "meet status=accepted user=%s",
             mailboxd_session_display_name(session));
    mailboxd_session_write_line(initiator, msg);
    if (mailboxd_monitor_is_active(initiator)) {
        mailboxd_monitor_broadcast(service, msg);
    }

    return MAILBOXD_OK;
}

void mailboxd_conference_invite_tick(mailboxd_service_t *service,
                                  mailboxd_session_t *session)
{
    time_t deadline;
    time_t now;
    char from_username[MAILBOXD_USER_NAME_MAX];
    const char *from_ptr;
    mailboxd_session_t *initiator;
    char msg[MAILBOXD_LINE_MAX];

    if (service == NULL || session == NULL) {
        return;
    }

    if (!mailboxd_conference_invite_pending(session)) {
        return;
    }

    deadline = mailboxd_session_conference_invite_deadline(session);
    if (deadline == 0) {
        return;
    }

    now = time(NULL);
    if (now < deadline) {
        return;
    }

    from_ptr = mailboxd_session_conference_invite_from(session);
    if (from_ptr != NULL) {
        mailboxd_strlcpy(from_username, from_ptr, sizeof(from_username));
    } else {
        from_username[0] = '\0';
    }

    mailboxd_session_clear_conference_invite(session);
    mailboxd_session_write_line(session, "*** Conference invite timed out.");

    if (from_username[0] != '\0') {
        initiator = conference_find_online_user(service, from_username);
        if (initiator != NULL) {
            snprintf(msg, sizeof(msg), "meet status=timeout user=%s",
                     mailboxd_session_display_name(session));
            mailboxd_session_write_line(initiator, msg);
            if (mailboxd_monitor_is_active(initiator)) {
                mailboxd_monitor_broadcast(service, msg);
            }
        }
    }
}

mailboxd_result_t mailboxd_conference_monitor_meet(mailboxd_service_t *service,
                                             mailboxd_session_t *initiator,
                                             const char *partner_spec,
                                             int force)
{
    const mailboxd_monitor_config_t *mcfg;
    mailboxd_storage_t *storage;
    mailboxd_user_record_t partner_user;
    mailboxd_session_t *partner_session;
    char line[MAILBOXD_LINE_MAX];
    mailboxd_result_t rc;
    const char *topic = "monitor";
    unsigned timeout_sec = 20u;

    if (service == NULL || initiator == NULL || partner_spec == NULL ||
        partner_spec[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    mcfg = mailboxd_monitor_config_get();
    if (mcfg != NULL) {
        timeout_sec = mcfg->invite_timeout_sec;
    }

    if (mailboxd_session_is_guest(initiator)) {
        mailboxd_session_write_line(initiator, "Guests cannot use meet.");
        return MAILBOXD_ERR_DENIED;
    }

    if (conference_session_active(initiator)) {
        mailboxd_session_write_line(initiator, "Already in a conference.");
        return MAILBOXD_OK;
    }

    if (mailboxd_conference_invite_pending(initiator)) {
        mailboxd_session_write_line(initiator,
            "Answer the pending conference invite first (y/n).");
        return MAILBOXD_OK;
    }

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_storage_resolve_user(storage, partner_spec, &partner_user);
    if (rc == MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(initiator, "Unknown user.");
        return MAILBOXD_OK;
    }
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (!partner_user.active) {
        mailboxd_session_write_line(initiator, "User is not active.");
        return MAILBOXD_OK;
    }

    if (mailboxd_user_level_is_guest(partner_user.level)) {
        mailboxd_session_write_line(initiator, "Cannot conference with a guest.");
        return MAILBOXD_OK;
    }

    if (str_ieq(partner_user.username, mailboxd_session_username(initiator))) {
        mailboxd_session_write_line(initiator, "Cannot conference with yourself.");
        return MAILBOXD_OK;
    }

    partner_session = conference_find_online_user(service, partner_user.username);
    if (partner_session == NULL) {
        mailboxd_session_write_line(initiator, "User is not online.");
        return MAILBOXD_OK;
    }

    if (conference_session_active(partner_session)) {
        mailboxd_session_write_line(initiator, "User is already in a conference.");
        return MAILBOXD_OK;
    }

    if (force) {
        if (conference_activate(service, initiator, partner_session,
                                topic) != MAILBOXD_OK) {
            mailboxd_session_write_line(initiator,
                "Conference could not be started.");
            return MAILBOXD_OK;
        }
        snprintf(line, sizeof(line), "meet status=forced user=%s",
                 mailboxd_user_display_name(&partner_user));
        mailboxd_session_write_line(initiator, line);
        mailboxd_monitor_broadcast(service, line);
        mailboxd_session_write_line(partner_session,
            "You were placed into a conference by Sysop.");
        return MAILBOXD_OK;
    }

    if (mailboxd_conference_invite_pending(partner_session)) {
        mailboxd_session_write_line(initiator,
            "User has a pending conference invite.");
        return MAILBOXD_OK;
    }

    mailboxd_session_set_conference_invite(partner_session,
                                        mailboxd_session_username(initiator),
                                        topic);
    mailboxd_session_set_conference_invite_deadline(
        partner_session, time(NULL) + (time_t)timeout_sec);

    snprintf(line, sizeof(line),
             "*** Conference invite from %s: %s (reply in %u s)",
             mailboxd_session_display_name(initiator), topic, timeout_sec);
    mailboxd_session_write_line(partner_session, line);
    mailboxd_session_write_line(partner_session, "*** Accept? y/n or yes/no");

    snprintf(line, sizeof(line),
             "meet status=pending user=%s timeout=%us",
             mailboxd_user_display_name(&partner_user), timeout_sec);
    mailboxd_session_write_line(initiator, line);
    mailboxd_monitor_broadcast(service, line);

    return MAILBOXD_OK;
}

typedef struct conference_post_ctx {
    mailboxd_session_t *from;
    const char *message;
    const char *from_user;
    const char *from_login;
} conference_post_ctx_t;

static void conference_post_visitor(mailboxd_session_t *session, void *userdata)
{
    conference_post_ctx_t *ctx = (conference_post_ctx_t *)userdata;
    char line[MAILBOXD_USER_NAME_MAX + MAILBOXD_LINE_MAX + 16];
    const char *session_login;

    if (session == NULL || ctx == NULL || ctx->message == NULL ||
        ctx->from == NULL || ctx->from_login == NULL) {
        return;
    }

    if (!conference_session_active(session)) {
        return;
    }

    if (session == ctx->from) {
        snprintf(line, sizeof(line), "ME: %s", ctx->message);
        mailboxd_session_write_line(session, line);
        return;
    }

    session_login = mailboxd_session_username(session);
    if (session_login == NULL ||
        !str_ieq(mailboxd_session_conference_partner(session), ctx->from_login) ||
        !str_ieq(mailboxd_session_conference_partner(ctx->from), session_login)) {
        return;
    }

    snprintf(line, sizeof(line), "%s: %s", ctx->from_user, ctx->message);
    mailboxd_session_write_line(session, line);
}

mailboxd_result_t mailboxd_conference_post(mailboxd_service_t *service,
                                     mailboxd_session_t *from,
                                     const char *message)
{
    conference_post_ctx_t ctx;
    const mailboxd_chat_config_t *chat;

    if (service == NULL || from == NULL || message == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (message[0] == '\0') {
        return MAILBOXD_OK;
    }

    if (!conference_session_active(from)) {
        return MAILBOXD_ERR_INVALID;
    }

    chat = mailboxd_service_get_chat(service);
    if (chat == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (strlen(message) > chat->message_max) {
        return MAILBOXD_ERR_INVALID;
    }

    ctx.from = from;
    ctx.message = message;
    ctx.from_user = mailboxd_session_display_name(from);
    ctx.from_login = mailboxd_session_username(from);
    if (ctx.from_login == NULL || ctx.from_login[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    mailboxd_service_visit_sessions(service, conference_post_visitor, &ctx);
    return MAILBOXD_OK;
}

void mailboxd_conference_area_leaving(mailboxd_session_t *session)
{
    const char *partner_name;

    if (session == NULL) {
        return;
    }

    partner_name = mailboxd_session_conference_partner(session);
    mailboxd_session_clear_conference(session);

    if (partner_name != NULL && partner_name[0] != '\0') {
        conference_notify_partner_left(session, partner_name);
    }
}

void mailboxd_conference_session_closed(mailboxd_session_t *session)
{
    mailboxd_service_t *service;
    const char *username;

    if (session == NULL) {
        return;
    }

    if (mailboxd_conference_invite_pending(session)) {
        mailboxd_session_clear_conference_invite(session);
    }

    if (!conference_session_active(session)) {
        service = mailboxd_session_service(session);
        username = mailboxd_session_username(session);
        if (service != NULL && username != NULL && username[0] != '\0') {
            conference_cancel_invites_from(service, username);
        }
        return;
    }

    mailboxd_conference_area_leaving(session);

    service = mailboxd_session_service(session);
    username = mailboxd_session_username(session);
    if (service != NULL && username != NULL && username[0] != '\0') {
        conference_cancel_invites_from(service, username);
    }
}
