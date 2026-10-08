/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_SESSION_H
#define MAILBOXD_SESSION_H

#include "mailboxd/plugin.h"
#include "mailboxd/storage.h"
#include "mailboxd/mail.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_service;

typedef enum mailboxd_session_area {
    MAILBOXD_AREA_MAIN = 1,
    MAILBOXD_AREA_MAIL = 2,
    MAILBOXD_AREA_CHAT = 3,
    MAILBOXD_AREA_CONFERENCE = 4
} mailboxd_session_area_t;

/** Maximum nested area depth (main + sub-areas). */
#define MAILBOXD_AREA_STACK_MAX 8u

/**
 * Open a new connection session.
 * When auto-login is enabled (default), assigns the next free Guest1 … Guest25
 * slot in memory (not written to user files).
 * When auto-login is disabled, the session stays at a login prompt for
 * registered `/login` and `/register` only (no guest slots).
 */
mailboxd_result_t mailboxd_session_open(struct mailboxd_service *service,
                                  const mailboxd_transport_plugin_t *transport,
                                  void *transport_data,
                                  mailboxd_session_t **out);

void mailboxd_session_close(mailboxd_session_t *session);

mailboxd_result_t mailboxd_session_handle_input(mailboxd_session_t *session,
                                         const uint8_t *data, size_t len);

const char *mailboxd_session_username(const mailboxd_session_t *session);
/** User-facing name (nickname) for the logged-in account. */
const char *mailboxd_session_display_name(const mailboxd_session_t *session);
/**
 * Format `username@plugin` (transport name) into @p out.
 * Uses display name and session transport / record.
 */
mailboxd_result_t mailboxd_session_format_user_at_plugin(const mailboxd_session_t *session,
                                                   char *out, size_t out_len);
uint64_t mailboxd_session_id(const mailboxd_session_t *session);
const mailboxd_session_record_t *mailboxd_session_record(const mailboxd_session_t *session);
/** Store the client peer address on the session record (for security.log). */
mailboxd_result_t mailboxd_session_set_remote(mailboxd_session_t *session,
                                        const char *remote);
mailboxd_user_level_t mailboxd_session_user_level(const mailboxd_session_t *session);
int mailboxd_session_is_guest(const mailboxd_session_t *session);
/** Non-zero for telnet — not plugin/link transports. */
int mailboxd_session_is_interactive_user(const mailboxd_session_t *session);

/**
 * Non-zero when this Sysop is hidden from /who and /online:
 * [monitor] invisible-sysop=yes, or /monitor on.
 * Non-Sysop grant users are never hidden.
 */
int mailboxd_session_hidden_from_who(const mailboxd_session_t *session);

int mailboxd_session_monitor_active(const mailboxd_session_t *session);
void mailboxd_session_set_monitor_active(mailboxd_session_t *session, int on);
/** Non-zero when the session has completed login (guest or registered). */
int mailboxd_session_logged_in(const mailboxd_session_t *session);
/** Non-zero when auto_login is off: banner + prompt, registered login only. */
int mailboxd_session_login_prompt(const mailboxd_session_t *session);

/** Write raw text to the connected client (no line ending, no prompt). */
mailboxd_result_t mailboxd_session_write(mailboxd_session_t *session, const char *text);

/** Write a line and append CRLF. */
mailboxd_result_t mailboxd_session_write_line(mailboxd_session_t *session,
                                        const char *text);

/** One blank line before `/command` output (not chat/mail compose). */
mailboxd_result_t mailboxd_session_command_gap(mailboxd_session_t *session);

/**
 * Show the global input prompt when configured.
 * Default (empty prompt): no output — users type on a blank line.
 */
mailboxd_result_t mailboxd_session_show_prompt(mailboxd_session_t *session);

/** Clear screen and discard the current input line; show prompt when set. */
mailboxd_result_t mailboxd_session_clear_terminal(mailboxd_session_t *session);

/** Non-zero when typed characters are echoed back to the client. */
int mailboxd_session_input_echo(const mailboxd_session_t *session);

/** Enable or disable per-session input echo. */
mailboxd_result_t mailboxd_session_set_input_echo(mailboxd_session_t *session, int enabled);

/** Current session area (main, mail, chat, …). */
mailboxd_session_area_t mailboxd_session_area(const mailboxd_session_t *session);

/** Area name: main, mail, chat. */
const char *mailboxd_session_area_name(mailboxd_session_area_t area);

/** Parse area name (main, mail, chat). */
mailboxd_session_area_t mailboxd_session_area_parse(const char *name);

/** Go up one area level (alias: /back). Clears state for the area being left. */
mailboxd_result_t mailboxd_session_leave_area(mailboxd_session_t *session);

/** Return to main from any depth (alias: /menu). Clears all sub-area state. */
mailboxd_result_t mailboxd_session_go_main(mailboxd_session_t *session);

/** Enter an area (pushes onto the area stack). */
mailboxd_result_t mailboxd_session_enter_area(mailboxd_session_t *session,
                                        mailboxd_session_area_t area);

/** Switch the session to a registered (non-guest) account after login. */
mailboxd_result_t mailboxd_session_switch_user(mailboxd_session_t *session,
                                         const mailboxd_user_record_t *user);

/**
 * Periodic session maintenance (guest time limit, etc.).
 * @return MAILBOXD_SESSION_END when the session should be closed.
 */
mailboxd_result_t mailboxd_session_tick(mailboxd_session_t *session);

/** Join a chat channel (enters chat area). @p channel_index is 1-based. */
mailboxd_result_t mailboxd_session_join_chat_channel(mailboxd_session_t *session,
                                               unsigned channel_index);

/** Current chat channel (1-based), or 0 when not in chat. */
unsigned mailboxd_session_chat_channel(const mailboxd_session_t *session);

struct mailboxd_service *mailboxd_session_service(mailboxd_session_t *session);

/** Join a private conference (conference area). Internal — use mailboxd_conference_start. */
mailboxd_result_t mailboxd_session_join_conference(mailboxd_session_t *session,
                                             const char *topic,
                                             const char *partner_username);

void mailboxd_session_clear_conference(mailboxd_session_t *session);

int mailboxd_session_conference_may_invite(mailboxd_session_t *session,
                                        const char *target);
void mailboxd_session_conference_invite_sent(mailboxd_session_t *session,
                                          const char *target);
void mailboxd_session_set_conference_invite(mailboxd_session_t *session,
                                         const char *from_username,
                                         const char *topic);
/** Optional deadline (0 = none). Used by monitor-initiated invites. */
void mailboxd_session_set_conference_invite_deadline(mailboxd_session_t *session,
                                                  time_t deadline);
time_t mailboxd_session_conference_invite_deadline(const mailboxd_session_t *session);
void mailboxd_session_clear_conference_invite(mailboxd_session_t *session);
const char *mailboxd_session_conference_invite_from(const mailboxd_session_t *session);
const char *mailboxd_session_conference_invite_topic(const mailboxd_session_t *session);
const char *mailboxd_session_conference_partner(const mailboxd_session_t *session);

/** Enter local mail area. */
mailboxd_result_t mailboxd_session_enter_mail(mailboxd_session_t *session);

/** Enter local chat area without joining a channel. */
mailboxd_result_t mailboxd_session_enter_chat(mailboxd_session_t *session);

/** Non-zero when composing an outbound mail message. */
int mailboxd_session_mail_composing(const mailboxd_session_t *session);

/** Start mail compose to @p to_user with @p subject; enters mail area. */
mailboxd_result_t mailboxd_session_mail_compose_start(mailboxd_session_t *session,
                                                const char *to_user,
                                                const char *subject);

/** Cancel an in-progress mail compose. */
void mailboxd_session_mail_compose_cancel(mailboxd_session_t *session);

/** Body accumulated so far during compose (NUL-terminated). */
const char *mailboxd_session_mail_compose_body(const mailboxd_session_t *session);

const char *mailboxd_session_mail_compose_to(const mailboxd_session_t *session);
const char *mailboxd_session_mail_compose_subject(const mailboxd_session_t *session);

/** Wall-clock time when this session connected (for bandwidth policy ordering). */
time_t mailboxd_session_connected_at(const mailboxd_session_t *session);

/** Non-zero when the session is frozen by bandwidth limits. */
int mailboxd_session_bandwidth_paused(const mailboxd_session_t *session);

void mailboxd_session_set_bandwidth_paused(mailboxd_session_t *session, int paused);

/**
 * End the session after sending @ref MAILBOXD_BANDWIDTH_DISCONNECT_MSG.
 * Newest-first under low-bandwidth link pressure.
 */
void mailboxd_session_disconnect_bandwidth(mailboxd_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_SESSION_H */
