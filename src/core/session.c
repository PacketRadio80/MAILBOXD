/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/session.h"
#include "mailboxd/service.h"
#include "mailboxd/command.h"
#include "mailboxd/storage.h"
#include "mailboxd/texts.h"
#include "mailboxd/auth.h"
#include "mailboxd/chat.h"
#include "mailboxd/conference.h"
#include "mailboxd/mail.h"
#include "mailboxd/terminal.h"
#include "mailboxd/traffic.h"
#include "mailboxd/log.h"
#include "mailboxd/monitor.h"
#include "mailboxd/instance.h"
#include "mailboxd/mailboxd.h"
#include "mailboxd/util.h"
#include "mailboxd/bandwidth_policy.h"
#include "mailboxd/messages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static int session_str_ieq(const char *a, const char *b)
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

typedef struct mailboxd_session_core {
    mailboxd_session_t pub;
    mailboxd_session_record_t record;
    mailboxd_user_record_t user;
    mailboxd_service_t *service;
    char line_buf[MAILBOXD_LINE_MAX];
    size_t line_len;
    size_t line_cursor;
    char history[MAILBOXD_HISTORY_MAX][MAILBOXD_LINE_MAX];
    unsigned history_count;
    unsigned history_next;
    int history_view;
    char history_saved_line[MAILBOXD_LINE_MAX];
    int expect_lf;
    int logged_in;
    int login_prompt;
    unsigned guest_slot;
    time_t guest_expires_at;
    mailboxd_session_area_t area;
    mailboxd_session_area_t area_stack[MAILBOXD_AREA_STACK_MAX];
    unsigned area_depth;
    unsigned chat_channel;
    int chat_max_notice_shown;
    char conference_topic[MAILBOXD_CONFERENCE_TOPIC_MAX];
    char conference_partner[MAILBOXD_USER_NAME_MAX];
    int conference_invite_pending;
    char conference_invite_from[MAILBOXD_USER_NAME_MAX];
    char conference_invite_topic[MAILBOXD_CONFERENCE_TOPIC_MAX];
    time_t conference_invite_deadline;
    struct {
        char target[MAILBOXD_USER_NAME_MAX];
        time_t sent_at[MAILBOXD_CONFERENCE_INVITE_MAX_PER_TARGET];
    } conference_invite_rates[8];
    int monitor_active;
    int mail_composing;
    char mail_compose_to[MAILBOXD_USER_NAME_MAX];
    char mail_compose_subject[MAILBOXD_MAIL_SUBJECT_MAX + 1];
    char mail_compose_body[MAILBOXD_MAIL_BODY_MAX + 1];
    size_t mail_compose_body_len;
    unsigned out_col;
    int input_echo;
    int bandwidth_paused;
    int bandwidth_disconnect;
    unsigned char esc_state;
    char csi_buf[24];
    size_t csi_len;
} mailboxd_session_core_t;

#define SESSION_ESC_NONE 0u
#define SESSION_ESC_ESC  1u
#define SESSION_ESC_CSI  2u
#define SESSION_ESC_SS3 3u

#define SESSION_CSI_BUF_MAX 24u

static mailboxd_result_t session_handle_user_byte(mailboxd_session_core_t *core,
                                               unsigned char byte);

static size_t session_line_max_len(const mailboxd_session_core_t *core);
static unsigned session_chat_message_max(const mailboxd_session_core_t *core);
static mailboxd_result_t session_write_transport(mailboxd_session_core_t *core,
                                              const char *data, size_t len);
static mailboxd_result_t session_line_refresh_suffix(mailboxd_session_core_t *core);
static mailboxd_result_t session_line_cursor_left(mailboxd_session_core_t *core);
static mailboxd_result_t session_line_cursor_right(mailboxd_session_core_t *core);
static mailboxd_result_t session_line_cursor_home(mailboxd_session_core_t *core);
static mailboxd_result_t session_line_cursor_end(mailboxd_session_core_t *core);
static mailboxd_result_t session_line_backspace(mailboxd_session_core_t *core);
static mailboxd_result_t session_line_delete_forward(mailboxd_session_core_t *core);
static mailboxd_result_t session_line_insert_char(mailboxd_session_core_t *core,
                                               char ch);
static mailboxd_result_t session_history_up(mailboxd_session_core_t *core);
static mailboxd_result_t session_history_down(mailboxd_session_core_t *core);
static void session_maybe_announce_login(mailboxd_session_core_t *core);

static void session_escape_reset(mailboxd_session_core_t *core)
{
    if (core == NULL) {
        return;
    }

    core->esc_state = SESSION_ESC_NONE;
    core->csi_len = 0;
}

static int session_csi_is_final(unsigned char ch)
{
    return ch >= 0x40u && ch <= 0x7eu;
}

static size_t session_line_max_len(const mailboxd_session_core_t *core)
{
    size_t line_max = sizeof(core->line_buf) - 1;

    if (core == NULL) {
        return MAILBOXD_LINE_MAX - 1;
    }

    if (core->area == MAILBOXD_AREA_CHAT || core->area == MAILBOXD_AREA_CONFERENCE) {
        line_max = session_chat_message_max(core);
    } else if (core->area == MAILBOXD_AREA_MAIL && core->mail_composing) {
        line_max = MAILBOXD_LINE_MAX - 1;
    }

    return line_max;
}

static int session_char_echoable(char ch)
{
    unsigned char byte = (unsigned char)ch;

    return byte >= 0x20 && byte != 0x7f;
}

static mailboxd_result_t session_write_transport(mailboxd_session_core_t *core,
                                              const char *data, size_t len)
{
    if (core == NULL || data == NULL || len == 0) {
        return MAILBOXD_OK;
    }

    if (core->pub.transport != NULL && core->pub.transport->write != NULL) {
        return core->pub.transport->write(&core->pub, data, len);
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t session_line_move_back(mailboxd_session_core_t *core,
                                             size_t count)
{
    char buf[64];
    size_t chunk;
    size_t sent = 0;

    if (core == NULL || count == 0) {
        return MAILBOXD_OK;
    }

    memset(buf, '\b', sizeof(buf));

    while (sent < count) {
        mailboxd_result_t rc;

        chunk = count - sent;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }

        rc = session_write_transport(core, buf, chunk);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
        sent += chunk;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t session_line_refresh_suffix(mailboxd_session_core_t *core)
{
    size_t suffix_len;
    mailboxd_result_t rc;

    if (core == NULL || !core->input_echo) {
        return MAILBOXD_OK;
    }

    suffix_len = core->line_len - core->line_cursor;
    if (suffix_len > 0) {
        rc = session_write_transport(core, core->line_buf + core->line_cursor,
                                   suffix_len);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    rc = session_write_transport(core, "\x1b[K", 3);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    return session_line_move_back(core, suffix_len);
}

static mailboxd_result_t session_line_cursor_left(mailboxd_session_core_t *core)
{
    if (core == NULL || core->line_cursor == 0) {
        return MAILBOXD_OK;
    }

    core->line_cursor--;
    if (!core->input_echo) {
        return MAILBOXD_OK;
    }

    return session_write_transport(core, "\b", 1);
}

static mailboxd_result_t session_line_cursor_right(mailboxd_session_core_t *core)
{
    mailboxd_result_t rc;

    if (core == NULL || core->line_cursor >= core->line_len) {
        return MAILBOXD_OK;
    }

    if (core->input_echo) {
        char out[2];

        out[0] = core->line_buf[core->line_cursor];
        out[1] = '\0';
        rc = session_write_transport(core, out, 1);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    core->line_cursor++;
    return MAILBOXD_OK;
}

static mailboxd_result_t session_line_cursor_home(mailboxd_session_core_t *core)
{
    mailboxd_result_t rc = MAILBOXD_OK;

    if (core == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    while (core->line_cursor > 0) {
        rc = session_line_cursor_left(core);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t session_line_cursor_end(mailboxd_session_core_t *core)
{
    mailboxd_result_t rc = MAILBOXD_OK;

    if (core == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    while (core->line_cursor < core->line_len) {
        rc = session_line_cursor_right(core);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t session_line_backspace(mailboxd_session_core_t *core)
{
    mailboxd_result_t rc;

    if (core == NULL || core->line_cursor == 0) {
        return MAILBOXD_OK;
    }

    core->line_cursor--;
    memmove(core->line_buf + core->line_cursor,
            core->line_buf + core->line_cursor + 1,
            core->line_len - core->line_cursor - 1);
    core->line_len--;
    core->line_buf[core->line_len] = '\0';

    if (!core->input_echo) {
        return MAILBOXD_OK;
    }

    rc = session_write_transport(core, "\b", 1);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    return session_line_refresh_suffix(core);
}

static mailboxd_result_t session_line_delete_forward(mailboxd_session_core_t *core)
{
    if (core == NULL || core->line_cursor >= core->line_len) {
        return MAILBOXD_OK;
    }

    memmove(core->line_buf + core->line_cursor,
            core->line_buf + core->line_cursor + 1,
            core->line_len - core->line_cursor - 1);
    core->line_len--;
    core->line_buf[core->line_len] = '\0';

    if (!core->input_echo) {
        return MAILBOXD_OK;
    }

    return session_line_refresh_suffix(core);
}

static mailboxd_result_t session_line_insert_char(mailboxd_session_core_t *core,
                                               char ch)
{
    size_t line_max;
    mailboxd_result_t rc;
    char out[2];

    if (core == NULL || !session_char_echoable(ch)) {
        return MAILBOXD_OK;
    }

    line_max = session_line_max_len(core);
    if (core->line_len >= line_max) {
        return MAILBOXD_OK;
    }

    if (core->line_cursor < core->line_len) {
        memmove(core->line_buf + core->line_cursor + 1,
                core->line_buf + core->line_cursor,
                core->line_len - core->line_cursor);
    }

    core->line_buf[core->line_cursor] = ch;
    core->line_len++;
    core->line_cursor++;
    core->line_buf[core->line_len] = '\0';

    if (!core->input_echo) {
        return MAILBOXD_OK;
    }

    out[0] = ch;
    out[1] = '\0';
    rc = session_write_transport(core, out, 1);
    if (rc != MAILBOXD_OK) {
        core->line_cursor--;
        core->line_len--;
        core->line_buf[core->line_len] = '\0';
        return rc;
    }

    return session_line_refresh_suffix(core);
}

static int session_history_should_store(const char *line)
{
    if (line == NULL || line[0] == '\0') {
        return 0;
    }

    return strncmp(line, "/login ", 7) != 0 &&
           strncmp(line, "/register ", 10) != 0 &&
           strncmp(line, "/changeme ", 10) != 0 &&
           strncmp(line, "/userchange ", 12) != 0;
}

static void session_history_add(mailboxd_session_core_t *core, const char *line)
{
    if (core == NULL || !session_history_should_store(line)) {
        return;
    }

    mailboxd_strlcpy(core->history[core->history_next], line,
                  sizeof(core->history[0]));
    core->history_next = (core->history_next + 1u) % MAILBOXD_HISTORY_MAX;
    if (core->history_count < MAILBOXD_HISTORY_MAX) {
        core->history_count++;
    }
    core->history_view = -1;
    core->history_saved_line[0] = '\0';
}

static mailboxd_result_t session_line_replace(mailboxd_session_core_t *core,
                                           const char *line)
{
    size_t len;
    mailboxd_result_t rc;

    if (core == NULL || line == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    len = strlen(line);
    if (len >= sizeof(core->line_buf)) {
        len = sizeof(core->line_buf) - 1;
    }

    if (core->input_echo) {
        rc = session_line_move_back(core, core->line_cursor);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
        if (len > 0) {
            rc = session_write_transport(core, line, len);
            if (rc != MAILBOXD_OK) {
                return rc;
            }
        }
        rc = session_write_transport(core, "\x1b[K", 3);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    memcpy(core->line_buf, line, len);
    core->line_buf[len] = '\0';
    core->line_len = len;
    core->line_cursor = len;
    return MAILBOXD_OK;
}

static mailboxd_result_t session_history_up(mailboxd_session_core_t *core)
{
    unsigned slot;

    if (core == NULL || core->history_count == 0) {
        return MAILBOXD_OK;
    }

    if (core->history_view < 0) {
        mailboxd_strlcpy(core->history_saved_line, core->line_buf,
                      sizeof(core->history_saved_line));
        core->history_view = 0;
    } else if ((unsigned)(core->history_view + 1) < core->history_count) {
        core->history_view++;
    } else {
        return MAILBOXD_OK;
    }

    slot = (core->history_next + MAILBOXD_HISTORY_MAX - 1u -
            (unsigned)core->history_view) % MAILBOXD_HISTORY_MAX;
    return session_line_replace(core, core->history[slot]);
}

static mailboxd_result_t session_history_down(mailboxd_session_core_t *core)
{
    unsigned slot;

    if (core == NULL || core->history_view < 0) {
        return MAILBOXD_OK;
    }

    if (core->history_view == 0) {
        core->history_view = -1;
        return session_line_replace(core, core->history_saved_line);
    }

    core->history_view--;
    slot = (core->history_next + MAILBOXD_HISTORY_MAX - 1u -
            (unsigned)core->history_view) % MAILBOXD_HISTORY_MAX;
    return session_line_replace(core, core->history[slot]);
}

static mailboxd_result_t session_apply_csi(mailboxd_session_core_t *core,
                                        const char *params, char final_ch)
{
    if (core == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (final_ch == 'A') {
        return session_history_up(core);
    }

    if (final_ch == 'B') {
        return session_history_down(core);
    }

    if (final_ch == 'C') {
        return session_line_cursor_right(core);
    }

    if (final_ch == 'D') {
        return session_line_cursor_left(core);
    }

    if (final_ch == 'H') {
        return session_line_cursor_home(core);
    }

    if (final_ch == 'F') {
        return session_line_cursor_end(core);
    }

    if (final_ch == '~') {
        if (strcmp(params, "1") == 0 || strcmp(params, "7") == 0) {
            return session_line_cursor_home(core);
        }
        if (strcmp(params, "3") == 0) {
            return session_line_delete_forward(core);
        }
        if (strcmp(params, "4") == 0 || strcmp(params, "8") == 0) {
            return session_line_cursor_end(core);
        }
        return MAILBOXD_OK;
    }

    if (final_ch == 'K' || final_ch == 'P') {
        return MAILBOXD_OK;
    }

    return MAILBOXD_OK;
}

static int session_filter_escape_byte(mailboxd_session_core_t *core, unsigned char byte)
{
    if (core == NULL) {
        return 0;
    }

    if (core->esc_state == SESSION_ESC_NONE && byte == 0x9bu) {
        core->esc_state = SESSION_ESC_CSI;
        core->csi_len = 0;
        return 1;
    }

    if (core->esc_state == SESSION_ESC_NONE && byte == 0x1bu) {
        core->esc_state = SESSION_ESC_ESC;
        return 1;
    }

    if (core->esc_state == SESSION_ESC_ESC) {
        if (byte == '[') {
            core->esc_state = SESSION_ESC_CSI;
            core->csi_len = 0;
            return 1;
        }
        if (byte == 'O') {
            core->esc_state = SESSION_ESC_SS3;
            return 1;
        }

        session_escape_reset(core);
        return 0;
    }

    if (core->esc_state == SESSION_ESC_SS3) {
        mailboxd_result_t rc = MAILBOXD_OK;

        if (byte == 'C') {
            rc = session_line_cursor_right(core);
        } else if (byte == 'D') {
            rc = session_line_cursor_left(core);
        } else if (byte == 'H') {
            rc = session_line_cursor_home(core);
        } else if (byte == 'F') {
            rc = session_line_cursor_end(core);
        }

        session_escape_reset(core);
        (void)rc;
        return 1;
    }

    if (core->esc_state == SESSION_ESC_CSI) {
        if (core->csi_len + 1 >= SESSION_CSI_BUF_MAX) {
            session_escape_reset(core);
            return 1;
        }

        core->csi_buf[core->csi_len++] = (char)byte;

        if (session_csi_is_final(byte)) {
            char final_ch = (char)byte;
            char params[SESSION_CSI_BUF_MAX];

            if (core->csi_len > 1) {
                memcpy(params, core->csi_buf, core->csi_len - 1);
                params[core->csi_len - 1] = '\0';
            } else {
                params[0] = '\0';
            }

            session_escape_reset(core);
            (void)session_apply_csi(core, params, final_ch);
            return 1;
        }

        return 1;
    }

    return 0;
}

static unsigned session_chat_message_max(const mailboxd_session_core_t *core)
{
    const mailboxd_chat_config_t *chat;

    if (core == NULL || core->service == NULL) {
        return MAILBOXD_CHAT_MESSAGE_MAX;
    }

    chat = mailboxd_service_get_chat(core->service);
    if (chat == NULL) {
        return MAILBOXD_CHAT_MESSAGE_MAX;
    }

    return chat->message_max;
}

static void session_area_clear_state(mailboxd_session_core_t *core,
                                   mailboxd_session_area_t area)
{
    if (core == NULL) {
        return;
    }

    if (area == MAILBOXD_AREA_CHAT) {
        core->chat_channel = 0;
    }

    if (area == MAILBOXD_AREA_CONFERENCE) {
        mailboxd_conference_area_leaving(&core->pub);
    }

    if (area == MAILBOXD_AREA_MAIL) {
        core->mail_composing = 0;
        core->mail_compose_body[0] = '\0';
        core->mail_compose_body_len = 0;
        core->mail_compose_to[0] = '\0';
        core->mail_compose_subject[0] = '\0';
    }
}

static void session_area_sync(mailboxd_session_core_t *core)
{
    if (core == NULL || core->area_depth == 0) {
        return;
    }

    core->area = core->area_stack[core->area_depth - 1];
}

static mailboxd_result_t session_area_push(mailboxd_session_core_t *core,
                                        mailboxd_session_area_t area)
{
    if (core == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (core->area_depth >= MAILBOXD_AREA_STACK_MAX) {
        return MAILBOXD_ERR_BUSY;
    }

    if (core->area_depth > 0 &&
        core->area_stack[core->area_depth - 1] == area) {
        core->area = area;
        return MAILBOXD_OK;
    }

    core->area_stack[core->area_depth] = area;
    core->area_depth++;
    core->area = area;
    return MAILBOXD_OK;
}

static void session_release_guest_slot(mailboxd_session_core_t *core)
{
    if (core == NULL || core->guest_slot == 0 || core->service == NULL) {
        return;
    }

    mailboxd_service_guest_release(core->service, core->guest_slot);
    core->guest_slot = 0;
}

static void session_arm_guest_timer(mailboxd_session_core_t *core)
{
    unsigned timeout_sec;

    if (core == NULL || !mailboxd_user_level_is_guest(core->user.level)) {
        if (core != NULL) {
            core->guest_expires_at = 0;
        }
        return;
    }

    timeout_sec = mailboxd_service_guest_timeout_seconds(core->service);
    if (timeout_sec == 0) {
        core->guest_expires_at = 0;
        return;
    }

    core->guest_expires_at = time(NULL) + (time_t)timeout_sec;
}

static mailboxd_result_t session_activate_guest(mailboxd_session_core_t *core,
                                             const mailboxd_user_record_t *guest,
                                             unsigned guest_slot)
{
    mailboxd_storage_t *storage;
    const char *transport_name;
    mailboxd_result_t rc;

    if (core == NULL || guest == NULL || guest_slot < 1) {
        return MAILBOXD_ERR_INVALID;
    }

    storage = mailboxd_service_get_storage(core->service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (core->guest_slot != 0 && core->guest_slot != guest_slot) {
        session_release_guest_slot(core);
    }

    transport_name = core->pub.transport != NULL ?
                     core->pub.transport->name : "unknown";

    if (core->record.session_id != 0) {
        mailboxd_storage_session_end(storage, core->record.session_id);
        memset(&core->record, 0, sizeof(core->record));
    }

    rc = mailboxd_storage_session_begin(storage, guest, transport_name,
                                     &core->record);
    if (rc != MAILBOXD_OK) {
        mailboxd_service_guest_release(core->service, guest_slot);
        return rc;
    }

    core->user = *guest;
    core->guest_slot = guest_slot;
    core->logged_in = 1;
    core->login_prompt = 0;
    session_arm_guest_timer(core);
    return MAILBOXD_OK;
}

static mailboxd_result_t session_check_guest_expiry(mailboxd_session_core_t *core)
{
    time_t now;

    if (core == NULL || core->guest_expires_at == 0) {
        return MAILBOXD_OK;
    }

    now = time(NULL);
    if (now < core->guest_expires_at) {
        return MAILBOXD_OK;
    }

    (void)mailboxd_msg_send_system(&core->pub, "Guest time limit. Goodbye.");
    return MAILBOXD_SESSION_END;
}

static void session_process_line(mailboxd_session_core_t *core, const char *line);

mailboxd_result_t mailboxd_session_write(mailboxd_session_t *session, const char *text)
{
    mailboxd_session_core_t *core;

    if (session == NULL || text == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core != NULL && core->bandwidth_paused) {
        return MAILBOXD_OK;
    }

    return mailboxd_traffic_emit(session,
                                core != NULL ? &core->out_col : NULL,
                                text, strlen(text));
}

mailboxd_result_t mailboxd_session_write_line(mailboxd_session_t *session,
                                        const char *text)
{
    mailboxd_result_t rc;

    if (text != NULL) {
        rc = mailboxd_session_write(session, text);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    return mailboxd_session_write(session, "\n");
}

mailboxd_result_t mailboxd_session_command_gap(mailboxd_session_t *session)
{
    return mailboxd_session_write(session, "\n");
}

static int session_accepts_input(const mailboxd_session_core_t *core)
{
    return core != NULL && (core->logged_in != 0 || core->login_prompt != 0);
}

mailboxd_result_t mailboxd_session_show_prompt(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;
    const char *prompt;

    if (session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (!core->logged_in && !core->login_prompt) {
        return MAILBOXD_ERR_INVALID;
    }

    prompt = mailboxd_service_get_prompt(core->service);
    if (prompt == NULL || prompt[0] == '\0') {
        return MAILBOXD_OK;
    }

    return mailboxd_session_write(session, prompt);
}

mailboxd_result_t mailboxd_session_clear_terminal(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;
    mailboxd_result_t rc;

    if (session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !session_accepts_input(core)) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = mailboxd_term_clear_screen(session);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    core->line_len = 0;
    core->line_cursor = 0;
    core->line_buf[0] = '\0';
    core->out_col = 0;
    core->expect_lf = 0;

    return mailboxd_session_show_prompt(session);
}

int mailboxd_session_input_echo(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return 0;
    }

    return core->input_echo != 0;
}

mailboxd_result_t mailboxd_session_set_input_echo(mailboxd_session_t *session, int enabled)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !session_accepts_input(core)) {
        return MAILBOXD_ERR_INVALID;
    }

    core->input_echo = enabled ? 1 : 0;
    return MAILBOXD_OK;
}

static void session_submit_line(mailboxd_session_core_t *core)
{
    core->line_buf[core->line_len] = '\0';
    session_history_add(core, core->line_buf);
    session_process_line(core, core->line_buf);
    core->line_len = 0;
    core->line_cursor = 0;
    core->history_view = -1;
    if ((core->logged_in || core->login_prompt) &&
        core->area != MAILBOXD_AREA_CHAT &&
        core->area != MAILBOXD_AREA_CONFERENCE &&
        !(core->area == MAILBOXD_AREA_MAIL && core->mail_composing)) {
        mailboxd_session_show_prompt(&core->pub);
    }
}

static mailboxd_result_t session_echo_newline(mailboxd_session_core_t *core)
{
    if (core == NULL || !core->input_echo) {
        return MAILBOXD_OK;
    }

    return mailboxd_session_write(&core->pub, "\n");
}

static mailboxd_result_t session_handle_user_byte(mailboxd_session_core_t *core,
                                               unsigned char byte)
{
    char ch = (char)byte;

    if (ch == '\0') {
        return MAILBOXD_OK;
    }

    if (ch == '\b' || ch == 127) {
        return session_line_backspace(core);
    }

    if (ch == '\r') {
        {
            mailboxd_result_t rc = session_echo_newline(core);

            if (rc != MAILBOXD_OK) {
                return rc;
            }
        }
        session_submit_line(core);
        if (!core->logged_in && !core->login_prompt) {
            return MAILBOXD_SESSION_END;
        }
        core->expect_lf = 1;
        return MAILBOXD_OK;
    }

    if (ch == '\n') {
        if (core->expect_lf) {
            core->expect_lf = 0;
            return MAILBOXD_OK;
        }

        {
            mailboxd_result_t rc = session_echo_newline(core);

            if (rc != MAILBOXD_OK) {
                return rc;
            }
        }

        session_submit_line(core);
        if (!core->logged_in && !core->login_prompt) {
            return MAILBOXD_SESSION_END;
        }
        return MAILBOXD_OK;
    }

    core->expect_lf = 0;

    return session_line_insert_char(core, ch);
}

static void session_send_banner(mailboxd_session_core_t *core)
{
    const mailboxd_texts_config_t *texts;
    const char *service_name;

    texts = mailboxd_service_get_texts(core->service);
    if (texts == NULL) {
        return;
    }

    service_name = mailboxd_service_get_name(core->service);
    mailboxd_texts_send_banner(texts, &core->pub, MAILBOXD_VERSION_STRING, service_name);
}

static void session_process_line(mailboxd_session_core_t *core, const char *line)
{
    mailboxd_parsed_command_t cmd;
    mailboxd_command_scope_t scope;
    mailboxd_result_t rc;

    if (line[0] == '\0') {
        return;
    }

    scope = mailboxd_command_classify(line);
    if (scope == MAILBOXD_CMD_SCOPE_COMMENT) {
        return;
    }

    if (mailboxd_conference_invite_pending(&core->pub) &&
        scope == MAILBOXD_CMD_SCOPE_LOCAL) {
        if (mailboxd_conference_reply_invite(core->service, &core->pub, line) ==
            MAILBOXD_OK) {
            return;
        }
        mailboxd_session_write_line(&core->pub, "Reply y/n or yes/no.");
        return;
    }

    if (core->area == MAILBOXD_AREA_MAIL && core->mail_composing &&
        scope == MAILBOXD_CMD_SCOPE_LOCAL) {
        const mailboxd_mail_config_t *mail;
        size_t line_len;
        size_t body_max;

        mail = mailboxd_service_get_mail(core->service);
        body_max = mail != NULL ? mail->body_max : MAILBOXD_MAIL_BODY_MAX;
        line_len = strlen(line);

        if (core->mail_compose_body_len + line_len + 2 > body_max) {
            mailboxd_session_write_line(&core->pub, "Message body too long.");
            return;
        }

        if (core->mail_compose_body_len > 0) {
            core->mail_compose_body[core->mail_compose_body_len++] = '\n';
        }
        memcpy(core->mail_compose_body + core->mail_compose_body_len,
               line, line_len + 1);
        core->mail_compose_body_len += line_len;
        return;
    }

    if (core->area == MAILBOXD_AREA_CHAT && scope == MAILBOXD_CMD_SCOPE_LOCAL) {
        if (mailboxd_session_is_guest(&core->pub)) {
            mailboxd_session_write_line(&core->pub, "Guests cannot use chat.");
            return;
        }

        if (strlen(line) > session_chat_message_max(core)) {
            mailboxd_session_write_line(&core->pub, "Message too long.");
            return;
        }

        (void)mailboxd_chat_post(core->service, &core->pub, line);
        return;
    }

    if (core->area == MAILBOXD_AREA_CONFERENCE && scope == MAILBOXD_CMD_SCOPE_LOCAL) {
        if (mailboxd_session_is_guest(&core->pub)) {
            mailboxd_session_write_line(&core->pub, "Guests cannot use conference.");
            return;
        }

        if (strlen(line) > session_chat_message_max(core)) {
            mailboxd_session_write_line(&core->pub, "Message too long.");
            return;
        }

        (void)mailboxd_conference_post(core->service, &core->pub, line);
        return;
    }

    if (scope == MAILBOXD_CMD_SCOPE_LOCAL) {
        return;
    }

    rc = mailboxd_command_parse(line, &cmd);
    if (rc != MAILBOXD_OK) {
        mailboxd_session_write_line(&core->pub, "Command parse error.");
        return;
    }

    rc = mailboxd_command_dispatch(core->service, &core->pub, &cmd);
    if (rc == MAILBOXD_SESSION_END) {
        core->logged_in = 0;
    } else if (rc == MAILBOXD_ERR_DENIED) {
        /* access message already sent */
    } else if (rc != MAILBOXD_OK && rc != MAILBOXD_ERR_NOT_FOUND) {
        mailboxd_session_write_line(&core->pub, "Command failed.");
    }

    mailboxd_command_free(&cmd);
}

mailboxd_result_t mailboxd_session_open(mailboxd_service_t *service,
                                  const mailboxd_transport_plugin_t *transport,
                                  void *transport_data,
                                  mailboxd_session_t **out)
{
    mailboxd_session_core_t *core;
    mailboxd_storage_t *storage;
    const mailboxd_auth_config_t *auth;
    mailboxd_result_t rc;
    int suppress_startup_text;
    int interactive;

    if (service == NULL || transport == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    interactive = (transport->kind == MAILBOXD_TRANSPORT_TELNET);

    if (interactive && !mailboxd_instance_offers_user_bbx()) {
        mailboxd_log_warn("[session] reject %s on %s (no user BBX on this instance)",
                       transport->name, mailboxd_instance_role_name());
        return MAILBOXD_ERR_DENIED;
    }

    rc = mailboxd_service_acquire_node(service);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    storage = mailboxd_service_get_storage(service);
    if (storage == NULL) {
        mailboxd_service_release_node(service);
        return MAILBOXD_ERR_INVALID;
    }

    auth = mailboxd_service_get_auth(service);
    if (auth == NULL) {
        mailboxd_service_release_node(service);
        return MAILBOXD_ERR_INVALID;
    }

    core = calloc(1, sizeof(*core));
    if (core == NULL) {
        mailboxd_service_release_node(service);
        return MAILBOXD_ERR_NOMEM;
    }

    core->service = service;
    core->pub.transport = transport;
    core->pub.transport_data = transport_data;
    core->pub.core_data = core;
    core->area = MAILBOXD_AREA_MAIN;
    core->area_stack[0] = MAILBOXD_AREA_MAIN;
    core->area_depth = 1;

    {
        const mailboxd_traffic_config_t *traffic = mailboxd_traffic_config_get();

        core->input_echo = traffic != NULL && traffic->input_echo;
    }

    suppress_startup_text = (transport->kind == MAILBOXD_TRANSPORT_INTERNAL);

    (void)mailboxd_term_init_session(&core->pub);

    if (!suppress_startup_text) {
        session_send_banner(core);
    }

    if (auth->auto_login) {
        unsigned guest_slot = 0;

        rc = mailboxd_service_guest_assign(service, auth->guest_prefix,
                                        &core->user, &guest_slot);
        if (rc != MAILBOXD_OK) {
            free(core);
            mailboxd_service_release_node(service);
            return rc;
        }

        rc = session_activate_guest(core, &core->user, guest_slot);
        if (rc != MAILBOXD_OK) {
            free(core);
            mailboxd_service_release_node(service);
            return rc;
        }

        if (!suppress_startup_text) {
            mailboxd_log_info("%s@%s connected (session %llu)",
                           core->record.username, transport->name,
                           (unsigned long long)core->record.session_id);
            mailboxd_monitor_event(service, core->record.username, transport->name,
                                "connected");
        }

        if (!suppress_startup_text) {
            mailboxd_session_show_prompt(&core->pub);
        }
    } else {
        core->login_prompt = 1;
        if (!suppress_startup_text) {
            mailboxd_session_show_prompt(&core->pub);
        }
    }

    *out = &core->pub;

    if (mailboxd_service_attach_session(service, &core->pub) != MAILBOXD_OK) {
        if (core->record.session_id != 0) {
            mailboxd_storage_session_end(storage, core->record.session_id);
        }
        session_release_guest_slot(core);
        free(core);
        mailboxd_service_release_node(service);
        return MAILBOXD_ERR_NOMEM;
    }

    return MAILBOXD_OK;
}

void mailboxd_session_close(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;
    mailboxd_storage_t *storage;

    if (session == NULL) {
        return;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return;
    }

    if (mailboxd_conference_invite_pending(session) ||
        mailboxd_session_area(session) == MAILBOXD_AREA_CONFERENCE) {
        mailboxd_conference_session_closed(session);
    }

    storage = mailboxd_service_get_storage(core->service);
    if (storage != NULL && core->record.session_id != 0) {
        const char *plugin = core->record.transport[0] != '\0'
                                 ? core->record.transport
                                 : (session->transport != NULL
                                        ? session->transport->name
                                        : "?");
        mailboxd_storage_session_end(storage, core->record.session_id);
        mailboxd_log_info("%s@%s disconnected (session %llu)",
                       core->record.username, plugin,
                       (unsigned long long)core->record.session_id);
        mailboxd_monitor_event(core->service, core->record.username, plugin,
                            "disconnected");
    }

    session_release_guest_slot(core);

    if (core->service != NULL) {
        mailboxd_service_detach_session(core->service, session);
        mailboxd_service_release_node(core->service);
    }

    free(core);
}

mailboxd_result_t mailboxd_session_switch_user(mailboxd_session_t *session,
                                         const mailboxd_user_record_t *user)
{
    mailboxd_session_core_t *core;
    mailboxd_storage_t *storage;
    mailboxd_session_record_t new_record;
    const char *transport_name;
    mailboxd_result_t rc;

    if (session == NULL || user == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_user_level_is_guest(user->level)) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL || core->service == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    session_release_guest_slot(core);

    storage = mailboxd_service_get_storage(core->service);
    if (storage == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    transport_name = core->record.transport;
    if (transport_name[0] == '\0' && session->transport != NULL) {
        transport_name = session->transport->name;
    }
    if (transport_name == NULL || transport_name[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    if (core->record.session_id != 0) {
        mailboxd_storage_session_end(storage, core->record.session_id);
    }

    rc = mailboxd_storage_session_begin(storage, user, transport_name, &new_record);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    core->user = *user;
    core->record = new_record;
    core->guest_expires_at = 0;
    core->logged_in = 1;
    core->login_prompt = 0;

    mailboxd_log_info("%s@%s login (session %llu)",
                   core->record.username, transport_name,
                   (unsigned long long)core->record.session_id);
    mailboxd_monitor_event(core->service, core->record.username, transport_name,
                        "login");

    session_maybe_announce_login(core);

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_session_tick(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !core->logged_in) {
        return MAILBOXD_ERR_INVALID;
    }

    if (core->bandwidth_disconnect) {
        return MAILBOXD_SESSION_END;
    }

    if (core->conference_invite_pending &&
        core->conference_invite_deadline > 0) {
        mailboxd_conference_invite_tick(core->service, session);
    }

    return session_check_guest_expiry(core);
}

mailboxd_result_t mailboxd_session_handle_input(mailboxd_session_t *session,
                                         const uint8_t *data, size_t len)
{
    mailboxd_session_core_t *core;
    size_t i;
    mailboxd_result_t expiry_rc;

    if (session == NULL || data == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !session_accepts_input(core)) {
        return MAILBOXD_ERR_INVALID;
    }

    if (core->bandwidth_paused) {
        return MAILBOXD_OK;
    }

    if (core->logged_in) {
        expiry_rc = session_check_guest_expiry(core);
        if (expiry_rc != MAILBOXD_OK) {
            return expiry_rc;
        }
    }

    for (i = 0; i < len; i++) {
        mailboxd_result_t rc;

        if (session_filter_escape_byte(core, data[i])) {
            continue;
        }

        rc = session_handle_user_byte(core, data[i]);

        if (rc == MAILBOXD_SESSION_END) {
            return MAILBOXD_SESSION_END;
        }
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    return MAILBOXD_OK;
}

const char *mailboxd_session_display_name(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return "";
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return "";
    }

    return mailboxd_user_display_name(&core->user);
}

mailboxd_result_t mailboxd_session_format_user_at_plugin(const mailboxd_session_t *session,
                                                   char *out, size_t out_len)
{
    const mailboxd_session_record_t *rec;
    const char *user;
    const char *plugin;

    if (session == NULL || out == NULL || out_len == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    user = mailboxd_session_display_name(session);
    if (user == NULL || user[0] == '\0') {
        user = mailboxd_session_username(session);
    }
    if (user == NULL || user[0] == '\0') {
        user = "?";
    }

    rec = mailboxd_session_record(session);
    plugin = (rec != NULL && rec->transport[0] != '\0') ? rec->transport : NULL;
    if (plugin == NULL || plugin[0] == '\0') {
        plugin = (session->transport != NULL && session->transport->name != NULL)
                     ? session->transport->name
                     : "?";
    }

    snprintf(out, out_len, "%s@%s", user, plugin);
    return MAILBOXD_OK;
}

typedef struct login_announce_ctx {
    const char *line;
} login_announce_ctx_t;

static void login_announce_visitor(mailboxd_session_t *session, void *userdata)
{
    login_announce_ctx_t *ctx = (login_announce_ctx_t *)userdata;

    if (session == NULL || ctx == NULL || ctx->line == NULL) {
        return;
    }

    if (!mailboxd_session_is_interactive_user(session) ||
        !mailboxd_session_logged_in(session)) {
        return;
    }

    mailboxd_session_write_line(session, ctx->line);
}

static void session_maybe_announce_login(mailboxd_session_core_t *core)
{
    mailboxd_service_t *service;
    char user_at[96];
    char line[128];
    login_announce_ctx_t ctx;

    if (core == NULL || core->service == NULL) {
        return;
    }

    service = core->service;
    if (!mailboxd_service_login_announce(service)) {
        return;
    }

    /* Guests: never announce. Hidden Sysop (invisible-sysop / monitor): skip. */
    if (mailboxd_user_level_is_guest(core->user.level) ||
        mailboxd_session_hidden_from_who(&core->pub)) {
        return;
    }

    if (mailboxd_session_format_user_at_plugin(&core->pub, user_at,
                                           sizeof(user_at)) != MAILBOXD_OK) {
        return;
    }

    snprintf(line, sizeof(line), "*** User login: %s", user_at);
    ctx.line = line;
    mailboxd_service_visit_sessions(service, login_announce_visitor, &ctx);
}

const char *mailboxd_session_username(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return "";
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return "";
    }

    return core->record.username;
}

uint64_t mailboxd_session_id(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return 0;
    }

    return core->record.session_id;
}

const mailboxd_session_record_t *mailboxd_session_record(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return NULL;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return NULL;
    }

    return &core->record;
}

mailboxd_result_t mailboxd_session_set_remote(mailboxd_session_t *session,
                                        const char *remote)
{
    mailboxd_session_core_t *core;

    if (session == NULL || remote == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    mailboxd_strlcpy(core->record.remote, remote, sizeof(core->record.remote));
    return MAILBOXD_OK;
}

mailboxd_user_level_t mailboxd_session_user_level(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return MAILBOXD_LEVEL_GUEST;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return MAILBOXD_LEVEL_GUEST;
    }

    return core->user.level;
}

int mailboxd_session_is_guest(const mailboxd_session_t *session)
{
    return mailboxd_user_level_is_guest(mailboxd_session_user_level(session));
}

int mailboxd_session_is_interactive_user(const mailboxd_session_t *session)
{
    mailboxd_transport_kind_t kind;

    if (session == NULL || session->transport == NULL) {
        return 0;
    }

    kind = session->transport->kind;
    return kind == MAILBOXD_TRANSPORT_TELNET;
}

int mailboxd_session_monitor_active(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    return core != NULL && core->monitor_active != 0;
}

void mailboxd_session_set_monitor_active(mailboxd_session_t *session, int on)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return;
    }

    core->monitor_active = on ? 1 : 0;
}

int mailboxd_session_hidden_from_who(const mailboxd_session_t *session)
{
    if (session == NULL) {
        return 0;
    }

    if (!mailboxd_user_level_is_sysop(mailboxd_session_user_level(session))) {
        return 0;
    }

    if (mailboxd_monitor_invisible_sysop()) {
        return 1;
    }

    return mailboxd_session_monitor_active(session) ? 1 : 0;
}

int mailboxd_session_logged_in(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return 0;
    }

    return core->logged_in != 0;
}

int mailboxd_session_login_prompt(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return 0;
    }

    return core->login_prompt != 0;
}

mailboxd_session_area_t mailboxd_session_area(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return MAILBOXD_AREA_MAIN;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return MAILBOXD_AREA_MAIN;
    }

    return core->area;
}

const char *mailboxd_session_area_name(mailboxd_session_area_t area)
{
    switch (area) {
    case MAILBOXD_AREA_MAIN:
        return "main";
    case MAILBOXD_AREA_MAIL:
        return "mail";
    case MAILBOXD_AREA_CHAT:
        return "chat";
    case MAILBOXD_AREA_CONFERENCE:
        return "conference";
    default:
        return "main";
    }
}

mailboxd_session_area_t mailboxd_session_area_parse(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return MAILBOXD_AREA_MAIN;
    }

    if (session_str_ieq(name, "main")) {
        return MAILBOXD_AREA_MAIN;
    }

    if (session_str_ieq(name, "mail")) {
        return MAILBOXD_AREA_MAIL;
    }

    if (session_str_ieq(name, "chat")) {
        return MAILBOXD_AREA_CHAT;
    }

    if (session_str_ieq(name, "conference")) {
        return MAILBOXD_AREA_CONFERENCE;
    }

    return MAILBOXD_AREA_MAIN;
}

mailboxd_result_t mailboxd_session_enter_area(mailboxd_session_t *session,
                                        mailboxd_session_area_t area)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    return session_area_push(core, area);
}

mailboxd_result_t mailboxd_session_leave_area(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;
    mailboxd_session_area_t leaving;

    if (session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (core->area_depth <= 1) {
        return MAILBOXD_OK;
    }

    leaving = core->area_stack[core->area_depth - 1];
    session_area_clear_state(core, leaving);
    core->area_depth--;
    session_area_sync(core);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_session_go_main(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    while (core->area_depth > 1) {
        mailboxd_session_area_t leaving = core->area_stack[core->area_depth - 1];

        session_area_clear_state(core, leaving);
        core->area_depth--;
    }

    core->area_stack[0] = MAILBOXD_AREA_MAIN;
    core->area_depth = 1;
    core->area = MAILBOXD_AREA_MAIN;
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_session_join_chat_channel(mailboxd_session_t *session,
                                               unsigned channel_index)
{
    mailboxd_session_core_t *core;
    const mailboxd_chat_config_t *chat;
    const char *name;

    if (session == NULL || channel_index == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL || core->service == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    chat = mailboxd_service_get_chat(core->service);
    name = mailboxd_chat_channel_name(chat, channel_index);
    if (name == NULL) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    if (core->area != MAILBOXD_AREA_CHAT) {
        mailboxd_result_t push_rc = session_area_push(core, MAILBOXD_AREA_CHAT);

        if (push_rc != MAILBOXD_OK) {
            return push_rc;
        }
    }

    core->chat_channel = channel_index;

    if (!core->chat_max_notice_shown) {
        char notice[96];

        snprintf(notice, sizeof(notice), "Max %u chars per message.",
                 chat->message_max);
        mailboxd_session_write_line(session, notice);
        core->chat_max_notice_shown = 1;
    }

    return MAILBOXD_OK;
}

unsigned mailboxd_session_chat_channel(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL || core->area != MAILBOXD_AREA_CHAT) {
        return 0;
    }

    return core->chat_channel;
}

mailboxd_service_t *mailboxd_session_service(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return NULL;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return NULL;
    }

    return core->service;
}

static int session_str_ieq_local(const char *a, const char *b)
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

int mailboxd_session_conference_may_invite(mailboxd_session_t *session,
                                        const char *target)
{
    mailboxd_session_core_t *core;
    size_t s;
    size_t k;
    time_t now;
    unsigned count;

    if (session == NULL || target == NULL || target[0] == '\0') {
        return 0;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return 0;
    }

    now = time(NULL);
    for (s = 0; s < sizeof(core->conference_invite_rates) /
                    sizeof(core->conference_invite_rates[0]); s++) {
        if (!session_str_ieq_local(core->conference_invite_rates[s].target,
                                   target)) {
            continue;
        }

        count = 0;
        for (k = 0; k < MAILBOXD_CONFERENCE_INVITE_MAX_PER_TARGET; k++) {
            time_t sent = core->conference_invite_rates[s].sent_at[k];

            if (sent != 0 &&
                (now - sent) < (time_t)MAILBOXD_CONFERENCE_INVITE_WINDOW_SEC) {
                count++;
            }
        }

        return count < MAILBOXD_CONFERENCE_INVITE_MAX_PER_TARGET;
    }

    return 1;
}

void mailboxd_session_conference_invite_sent(mailboxd_session_t *session,
                                          const char *target)
{
    mailboxd_session_core_t *core;
    size_t s;
    size_t slot = (size_t)-1;
    size_t empty = (size_t)-1;
    size_t k;
    time_t now;

    if (session == NULL || target == NULL || target[0] == '\0') {
        return;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return;
    }

    now = time(NULL);

    for (s = 0; s < sizeof(core->conference_invite_rates) /
                    sizeof(core->conference_invite_rates[0]); s++) {
        if (core->conference_invite_rates[s].target[0] == '\0') {
            if (empty == (size_t)-1) {
                empty = s;
            }
            continue;
        }

        if (session_str_ieq_local(core->conference_invite_rates[s].target,
                                  target)) {
            slot = s;
            break;
        }
    }

    if (slot == (size_t)-1) {
        slot = empty;
    }

    if (slot == (size_t)-1) {
        slot = 0;
    }

    if (core->conference_invite_rates[slot].target[0] == '\0') {
        mailboxd_strlcpy(core->conference_invite_rates[slot].target, target,
                      sizeof(core->conference_invite_rates[slot].target));
    }

    for (k = 0; k < MAILBOXD_CONFERENCE_INVITE_MAX_PER_TARGET - 1u; k++) {
        core->conference_invite_rates[slot].sent_at[k] =
            core->conference_invite_rates[slot].sent_at[k + 1];
    }

    core->conference_invite_rates[slot].sent_at[
        MAILBOXD_CONFERENCE_INVITE_MAX_PER_TARGET - 1u] = now;
}

void mailboxd_session_set_conference_invite(mailboxd_session_t *session,
                                         const char *from_username,
                                         const char *topic)
{
    mailboxd_session_core_t *core;

    if (session == NULL || from_username == NULL || topic == NULL) {
        return;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return;
    }

    core->conference_invite_pending = 1;
    core->conference_invite_deadline = 0;
    mailboxd_strlcpy(core->conference_invite_from, from_username,
                  sizeof(core->conference_invite_from));
    mailboxd_strlcpy(core->conference_invite_topic, topic,
                  sizeof(core->conference_invite_topic));
}

void mailboxd_session_set_conference_invite_deadline(mailboxd_session_t *session,
                                                  time_t deadline)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return;
    }

    core->conference_invite_deadline = deadline;
}

time_t mailboxd_session_conference_invite_deadline(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !core->conference_invite_pending) {
        return 0;
    }

    return core->conference_invite_deadline;
}

void mailboxd_session_clear_conference_invite(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return;
    }

    core->conference_invite_pending = 0;
    core->conference_invite_deadline = 0;
    core->conference_invite_from[0] = '\0';
    core->conference_invite_topic[0] = '\0';
}

int mailboxd_conference_invite_pending(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return 0;
    }

    return core->conference_invite_pending != 0;
}

const char *mailboxd_session_conference_invite_from(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return NULL;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !core->conference_invite_pending) {
        return NULL;
    }

    return core->conference_invite_from;
}

const char *mailboxd_session_conference_invite_topic(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return NULL;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !core->conference_invite_pending) {
        return NULL;
    }

    return core->conference_invite_topic;
}

mailboxd_result_t mailboxd_session_join_conference(mailboxd_session_t *session,
                                             const char *topic,
                                             const char *partner_username)
{
    mailboxd_session_core_t *core;
    const mailboxd_chat_config_t *chat;

    if (session == NULL || topic == NULL || partner_username == NULL ||
        topic[0] == '\0' || partner_username[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL || core->service == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (core->area != MAILBOXD_AREA_CONFERENCE) {
        mailboxd_result_t push_rc = session_area_push(core, MAILBOXD_AREA_CONFERENCE);

        if (push_rc != MAILBOXD_OK) {
            return push_rc;
        }
    }

    mailboxd_strlcpy(core->conference_topic, topic, sizeof(core->conference_topic));
    mailboxd_strlcpy(core->conference_partner, partner_username,
                  sizeof(core->conference_partner));

    chat = mailboxd_service_get_chat(core->service);
    if (chat != NULL && !core->chat_max_notice_shown) {
        char notice[96];

        snprintf(notice, sizeof(notice), "Max %u chars per message.",
                 chat->message_max);
        mailboxd_session_write_line(session, notice);
        core->chat_max_notice_shown = 1;
    }

    return MAILBOXD_OK;
}

void mailboxd_session_clear_conference(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return;
    }

    core->conference_topic[0] = '\0';
    core->conference_partner[0] = '\0';
}

const char *mailboxd_session_conference_partner(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return NULL;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL || core->conference_partner[0] == '\0') {
        return NULL;
    }

    return core->conference_partner;
}

int mailboxd_session_mail_composing(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return 0;
    }

    return core->mail_composing;
}

mailboxd_result_t mailboxd_session_mail_compose_start(mailboxd_session_t *session,
                                                const char *to_user,
                                                const char *subject)
{
    mailboxd_session_core_t *core;
    const mailboxd_mail_config_t *mail;
    mailboxd_user_record_t recipient;
    mailboxd_storage_t *storage;
    size_t subject_len;

    if (session == NULL || to_user == NULL || subject == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    mail = mailboxd_service_get_mail(core->service);
    subject_len = strlen(subject);
    if (mail != NULL && subject_len > mail->subject_max) {
        return MAILBOXD_ERR_INVALID;
    }

    storage = mailboxd_service_get_storage(core->service);
    if (storage != NULL &&
        mailboxd_storage_resolve_user(storage, to_user, &recipient) == MAILBOXD_OK) {
        mailboxd_strlcpy(core->mail_compose_to, recipient.username,
                      sizeof(core->mail_compose_to));
    } else {
        mailboxd_strlcpy(core->mail_compose_to, to_user,
                      sizeof(core->mail_compose_to));
        mailboxd_username_normalize(core->mail_compose_to);
    }
    mailboxd_strlcpy(core->mail_compose_subject, subject,
                  sizeof(core->mail_compose_subject));
    core->mail_compose_body[0] = '\0';
    core->mail_compose_body_len = 0;
    core->mail_composing = 1;

    if (core->area != MAILBOXD_AREA_MAIL) {
        return session_area_push(core, MAILBOXD_AREA_MAIL);
    }

    return MAILBOXD_OK;
}

void mailboxd_session_mail_compose_cancel(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return;
    }

    core->mail_composing = 0;
    core->mail_compose_body[0] = '\0';
    core->mail_compose_body_len = 0;
    core->mail_compose_to[0] = '\0';
    core->mail_compose_subject[0] = '\0';
}

const char *mailboxd_session_mail_compose_body(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return "";
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !core->mail_composing) {
        return "";
    }

    return core->mail_compose_body;
}

const char *mailboxd_session_mail_compose_to(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return "";
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !core->mail_composing) {
        return "";
    }

    return core->mail_compose_to;
}

const char *mailboxd_session_mail_compose_subject(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return "";
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !core->mail_composing) {
        return "";
    }

    return core->mail_compose_subject;
}

mailboxd_result_t mailboxd_session_enter_mail(mailboxd_session_t *session)
{
    return mailboxd_session_enter_area(session, MAILBOXD_AREA_MAIL);
}

mailboxd_result_t mailboxd_session_enter_chat(mailboxd_session_t *session)
{
    return mailboxd_session_enter_area(session, MAILBOXD_AREA_CHAT);
}

time_t mailboxd_session_connected_at(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return 0;
    }

    return core->record.connected_at;
}

int mailboxd_session_bandwidth_paused(const mailboxd_session_t *session)
{
    const mailboxd_session_core_t *core;

    if (session == NULL) {
        return 0;
    }

    core = (const mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return 0;
    }

    return core->bandwidth_paused;
}

void mailboxd_session_set_bandwidth_paused(mailboxd_session_t *session, int paused)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL) {
        return;
    }

    core->bandwidth_paused = paused ? 1 : 0;
}

void mailboxd_session_disconnect_bandwidth(mailboxd_session_t *session)
{
    mailboxd_session_core_t *core;

    if (session == NULL) {
        return;
    }

    core = (mailboxd_session_core_t *)session->core_data;
    if (core == NULL || !core->logged_in) {
        return;
    }

    core->bandwidth_paused = 0;
    (void)mailboxd_traffic_emit(session, &core->out_col,
                               MAILBOXD_BANDWIDTH_DISCONNECT_MSG,
                               strlen(MAILBOXD_BANDWIDTH_DISCONNECT_MSG));
    (void)mailboxd_traffic_emit(session, &core->out_col, "\n", 1);
    core->bandwidth_disconnect = 1;
}
