/*
 * Entertain plugin — entertain.c
 * Feature plugin (not a transport): init / tick / on_command.
 * Chess is the first Entertain feature. Main / standalone Main only.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mailboxd/plugin.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/command.h"
#include "mailboxd/log.h"
#include "mailboxd/config.h"
#include "entertain_chess.h"

/* aichat is frozen — the source is kept but neither built nor called. Set this
 * to 1 and add aichat.c back to plugins/entertain/CMakeLists.txt to rebuild it. */
#ifndef MAILBOXD_ENTERTAIN_AICHAT
#define MAILBOXD_ENTERTAIN_AICHAT 0
#endif

#if MAILBOXD_ENTERTAIN_AICHAT
#include "entertain_aichat.h"
#endif

#include <string.h>

static struct mailboxd_service *g_service;

extern mailboxd_result_t entertain_cmd_chess(struct mailboxd_service *service,
                                          struct mailboxd_session *session,
                                          const struct mailboxd_parsed_command *cmd);

static int verb_is_chess(const char *verb)
{
    if (verb == NULL) {
        return 0;
    }
    return strcmp(verb, "chess") == 0 ||
           strcmp(verb, "play") == 0 ||
           strcmp(verb, "mv") == 0;
}

static mailboxd_result_t entertain_init(struct mailboxd_service *service)
{
    g_service = service;
    chess_init(CHESS_DEFAULT_MAX);
#if MAILBOXD_ENTERTAIN_AICHAT
    {
        const char *cfg_path = mailboxd_service_config_path(service);
        mailboxd_config_t cfg_obj;
        if (cfg_path && cfg_path[0] && mailboxd_config_load(&cfg_obj, cfg_path) == MAILBOXD_OK) {
            aichat_init(&cfg_obj);
            mailboxd_config_free(&cfg_obj);
        } else {
            aichat_init(NULL);
        }
    }
#endif
    mailboxd_log_info("[entertain] plugin loaded — chess_max_games=%u",
                   (unsigned)CHESS_DEFAULT_MAX);
    return MAILBOXD_OK;
}

static void entertain_shutdown(void)
{
#if MAILBOXD_ENTERTAIN_AICHAT
    aichat_shutdown();
#endif
    chess_shutdown();
    g_service = NULL;
    mailboxd_log_info("[entertain] plugin unloaded");
}

static void entertain_tick(struct mailboxd_service *service)
{
    chess_tick(service);
}

static mailboxd_result_t entertain_on_command(struct mailboxd_service *service,
                                           struct mailboxd_session *session,
                                           const struct mailboxd_parsed_command *cmd)
{
    if (cmd == NULL || cmd->verb == NULL) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    /* Future: chess over mains_proxy mesh — stub only. */
    if (strcmp(cmd->verb, "proxychess") == 0) {
        if (session != NULL) {
            mailboxd_session_write_line(session,
                "proxychess: not implemented yet (Main mesh stub).");
        }
        return MAILBOXD_OK;
    }

#if MAILBOXD_ENTERTAIN_AICHAT
    if (strcmp(cmd->verb, "aichat") == 0 ||
        strcmp(cmd->verb, "ai") == 0 ||
        strcmp(cmd->verb, "ask") == 0) {
        return aichat_cmd(service, session, cmd);
    }
#endif

    if (verb_is_chess(cmd->verb)) {
        return entertain_cmd_chess(service, session, cmd);
    }

    return MAILBOXD_ERR_NOT_FOUND;
}

const mailboxd_transport_plugin_t mailboxd_plugin_entertain = {
    .name = "entertain",
    .kind = 0, /* feature plugin — not a wire transport */
    .version = 1,
    .init = entertain_init,
    .shutdown = entertain_shutdown,
    .start = NULL,
    .stop = NULL,
    .write = NULL,
    .tick = entertain_tick,
    .on_command = entertain_on_command,
};
