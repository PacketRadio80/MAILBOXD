/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_COMMANDS_REGISTRY_H
#define MAILBOXD_COMMANDS_REGISTRY_H

#include "mailboxd/auth.h"
#include "mailboxd/limits.h"
#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_session;

typedef struct mailboxd_command_def {
    char verb[MAILBOXD_CMD_VERB_MAX];
    char group[16];
    mailboxd_user_level_t min_level;
    mailboxd_user_level_t max_level; /* 0 = no upper cap */
    int only_level; /* MAILBOXD_LEVEL_* when set; else 0 */
    char line1[MAILBOXD_COMMANDS_HELP_LINE_MAX];
    char line2[MAILBOXD_COMMANDS_HELP_LINE_MAX];
} mailboxd_command_def_t;

/** Load areas.yaml then commands.yaml from install root. Call once at service startup. */
mailboxd_result_t mailboxd_commands_registry_init(void);

void mailboxd_commands_registry_shutdown(void);

const mailboxd_command_def_t *mailboxd_commands_registry_find(const char *verb);

/** Map alias or topic name to canonical verb; returns @p topic if unknown. */
const char *mailboxd_commands_registry_canonical(const char *topic);

int mailboxd_commands_registry_verb_allowed(mailboxd_user_level_t level,
                                         const char *verb);

int mailboxd_commands_registry_help_allowed(mailboxd_user_level_t level,
                                         const char *verb);

int mailboxd_commands_registry_may_userchange(mailboxd_user_level_t actor,
                                           mailboxd_user_level_t target);

int mailboxd_commands_registry_may_userdelete(mailboxd_user_level_t actor,
                                           mailboxd_user_level_t target);

int mailboxd_commands_registry_may_promote(mailboxd_user_level_t actor,
                                        mailboxd_user_level_t target,
                                        int target_active,
                                        mailboxd_user_level_t new_level);

int mailboxd_commands_registry_may_demote(mailboxd_user_level_t actor,
                                         mailboxd_user_level_t target);

int mailboxd_commands_registry_may_delete(mailboxd_user_level_t actor,
                                       mailboxd_user_level_t target);

void mailboxd_commands_registry_show_menu(struct mailboxd_session *session);
void mailboxd_commands_registry_show_index(struct mailboxd_session *session);
void mailboxd_commands_registry_show_aliases(struct mailboxd_session *session);
void mailboxd_commands_registry_show_help(struct mailboxd_session *session,
                                       const char *canonical);

/**
 * List line1 for every registry command with min Admin or Sysop
 * (the admin set a Sysop may run while staying invisible via /monitor).
 */
void mailboxd_commands_registry_show_sysop_cmds(struct mailboxd_session *session);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_COMMANDS_REGISTRY_H */
