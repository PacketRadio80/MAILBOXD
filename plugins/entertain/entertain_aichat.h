/*
 * Entertain plugin — aichat (internal)
 * /aichat · /ai · /ask — OpenRouter AI chat for MailboxD Entertain area.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ENTERTAIN_AICHAT_H
#define ENTERTAIN_AICHAT_H

#include "mailboxd/types.h"

struct mailboxd_session;
struct mailboxd_service;
struct mailboxd_parsed_command;
struct mailboxd_config;

#define AICHAT_MAX_USERS 64
#define AICHAT_WRAP_COL  78

typedef struct entertain_aichat_config {
    int      enabled;
    char     provider[32];
    char     api_key[256];
    char     api_url[256];
    char     model[128];
    unsigned max_tokens;
    double   temperature;
    unsigned rate_limit_per_user_hour;
    unsigned timeout_sec;
    char     site_url[128];
    char     site_name[128];
} entertain_aichat_config_t;

typedef struct entertain_aichat_user_rate {
    char     username[64];
    unsigned request_count;
    long     window_start;
} entertain_aichat_user_rate_t;

/* Lifecycle */
void aichat_init(const struct mailboxd_config *config);
void aichat_shutdown(void);

/* Command handler */
mailboxd_result_t aichat_cmd(struct mailboxd_service *service,
                           struct mailboxd_session *session,
                           const struct mailboxd_parsed_command *cmd);

#endif /* ENTERTAIN_AICHAT_H */
