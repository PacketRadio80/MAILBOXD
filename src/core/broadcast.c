/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/broadcast.h"
#include "mailboxd/messages.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/traffic.h"
#include "mailboxd/util.h"
#include "mailboxd/log.h"

#include <string.h>

void mailboxd_broadcast_config_defaults(mailboxd_broadcast_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 1;
}

void mailboxd_broadcast_config_apply(mailboxd_broadcast_config_t *cfg,
                                  const mailboxd_config_t *config)
{
    if (cfg == NULL || config == NULL) {
        return;
    }

    mailboxd_broadcast_config_defaults(cfg);
    cfg->enabled = mailboxd_config_get_bool(config, "broadcast", "enabled", 1);

    mailboxd_log_info("[broadcast] announce=%s",
                   cfg->enabled ? "yes" : "no");
}

typedef struct broadcast_announce_ctx {
    mailboxd_session_t *from;
    const char *from_name;
    const char *message;
} broadcast_announce_ctx_t;

static void broadcast_announce_visitor(mailboxd_session_t *session, void *userdata)
{
    broadcast_announce_ctx_t *ctx = (broadcast_announce_ctx_t *)userdata;
    char line[MAILBOXD_LINE_MAX];

    if (session == NULL || ctx == NULL || ctx->message == NULL) {
        return;
    }

    /*
     * Local /broadcast is for online human (telnet) users only. The
     * mailboxd_prterm link session is not interactive and must not
     * receive terminal fan-out.
     */
    if (!mailboxd_session_is_interactive_user(session)) {
        return;
    }
    if (!mailboxd_session_logged_in(session)) {
        return;
    }

    if (mailboxd_msg_format_sysop(line, sizeof(line),
                             ctx->from_name != NULL ? ctx->from_name : "Announce",
                             ctx->message) != MAILBOXD_OK) {
        return;
    }
    if (ctx->from == NULL || session != ctx->from) {
        (void)mailboxd_session_command_gap(session);
    }
    mailboxd_session_write_line(session, line);
}

mailboxd_result_t mailboxd_broadcast_announce(mailboxd_service_t *service,
                                        mailboxd_session_t *from,
                                        const char *message)
{
    broadcast_announce_ctx_t ctx;
    const mailboxd_broadcast_config_t *cfg;
    size_t msg_len;

    if (service == NULL || message == NULL || message[0] == '\0') {
        return MAILBOXD_ERR_INVALID;
    }

    cfg = mailboxd_service_get_broadcast(service);
    if (cfg == NULL || !cfg->enabled) {
        return MAILBOXD_ERR_UNSUPPORTED;
    }

    msg_len = strlen(message);
    if (msg_len > MAILBOXD_BROADCAST_MESSAGE_MAX) {
        return MAILBOXD_ERR_INVALID;
    }

    ctx.from = from;
    ctx.from_name = (from != NULL) ? mailboxd_session_display_name(from) : "Announce";
    ctx.message = message;
    mailboxd_service_visit_sessions(service, broadcast_announce_visitor, &ctx);

    mailboxd_log_stats("[broadcast] announce to local Main (%zu bytes): %s",
           msg_len, message);
    return MAILBOXD_OK;
}
