/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/bandwidth_policy.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/plugin.h"
#include "mailboxd/log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define BW_USER_MAX 128u

typedef struct bw_user_entry {
    mailboxd_session_t *session;
    time_t connected_at;
    int sacrifice_priority;
} bw_user_entry_t;

typedef struct bw_collect_ctx {
    bw_user_entry_t entries[BW_USER_MAX];
    unsigned count;
} bw_collect_ctx_t;

static int session_sacrifice_priority(const mailboxd_session_t *session)
{
    const mailboxd_transport_plugin_t *transport;

    if (session == NULL) {
        return -1;
    }

    transport = session->transport;
    if (transport == NULL) {
        return -1;
    }

    switch (transport->kind) {
    case MAILBOXD_TRANSPORT_TELNET:
        return 0;               /* interactive users, newest first */
    case MAILBOXD_TRANSPORT_INTERNAL:
    default:
        return -1;
    }
}

static void bw_collect_visitor(mailboxd_session_t *session, void *userdata)
{
    bw_collect_ctx_t *ctx = (bw_collect_ctx_t *)userdata;
    int priority;

    if (ctx == NULL || session == NULL) {
        return;
    }

    priority = session_sacrifice_priority(session);
    if (priority < 0 || !mailboxd_session_logged_in(session)) {
        return;
    }

    if (ctx->count >= BW_USER_MAX) {
        return;
    }

    ctx->entries[ctx->count].session = session;
    ctx->entries[ctx->count].connected_at = mailboxd_session_connected_at(session);
    ctx->entries[ctx->count].sacrifice_priority = priority;
    ctx->count++;
}

static void bw_sort_victims_first(bw_collect_ctx_t *ctx)
{
    unsigned i;
    unsigned j;

    if (ctx == NULL) {
        return;
    }

    /* Single user class: newest connected_at first. */
    for (i = 0; i + 1 < ctx->count; i++) {
        for (j = i + 1; j < ctx->count; j++) {
            int swap = 0;

            if (ctx->entries[j].sacrifice_priority <
                ctx->entries[i].sacrifice_priority) {
                swap = 1;
            } else if (ctx->entries[j].sacrifice_priority ==
                       ctx->entries[i].sacrifice_priority) {
                if (ctx->entries[j].connected_at >
                    ctx->entries[i].connected_at) {
                    swap = 1;
                } else if (ctx->entries[j].connected_at ==
                           ctx->entries[i].connected_at &&
                           mailboxd_session_id(ctx->entries[j].session) >
                           mailboxd_session_id(ctx->entries[i].session)) {
                    swap = 1;
                }
            }

            if (swap) {
                bw_user_entry_t tmp = ctx->entries[i];

                ctx->entries[i] = ctx->entries[j];
                ctx->entries[j] = tmp;
            }
        }
    }
}

static mailboxd_session_t *bw_first_unpaused(const bw_collect_ctx_t *ctx)
{
    unsigned i;

    if (ctx == NULL) {
        return NULL;
    }

    for (i = 0; i < ctx->count; i++) {
        if (!mailboxd_session_bandwidth_paused(ctx->entries[i].session)) {
            return ctx->entries[i].session;
        }
    }

    return NULL;
}

static mailboxd_session_t *bw_first_paused(const bw_collect_ctx_t *ctx)
{
    unsigned i;

    if (ctx == NULL) {
        return NULL;
    }

    for (i = 0; i < ctx->count; i++) {
        if (mailboxd_session_bandwidth_paused(ctx->entries[i].session)) {
            return ctx->entries[i].session;
        }
    }

    return NULL;
}

unsigned mailboxd_bandwidth_policy_user_count(mailboxd_service_t *service)
{
    bw_collect_ctx_t ctx;

    if (service == NULL) {
        return 0;
    }

    memset(&ctx, 0, sizeof(ctx));
    mailboxd_service_visit_sessions(service, bw_collect_visitor, &ctx);
    return ctx.count;
}

unsigned mailboxd_bandwidth_policy_apply(mailboxd_service_t *service,
                                      mailboxd_bandwidth_action_t action)
{
    bw_collect_ctx_t ctx;
    unsigned i;
    unsigned affected = 0;

    if (service == NULL) {
        return 0;
    }

    memset(&ctx, 0, sizeof(ctx));
    mailboxd_service_visit_sessions(service, bw_collect_visitor, &ctx);
    if (ctx.count == 0) {
        return 0;
    }

    bw_sort_victims_first(&ctx);

    switch (action) {
    case MAILBOXD_BANDWIDTH_PAUSE: {
        mailboxd_session_t *target = bw_first_unpaused(&ctx);

        if (target != NULL) {
            mailboxd_session_set_bandwidth_paused(target, 1);
            mailboxd_log_stats("[bandwidth] paused user %s",
                   mailboxd_session_display_name(target));
            affected = 1;
        }
        break;
    }
    case MAILBOXD_BANDWIDTH_BREAK: {
        mailboxd_session_t *target = bw_first_paused(&ctx);

        if (target == NULL && ctx.count > 0) {
            target = ctx.entries[0].session;
        }

        if (target != NULL) {
            mailboxd_log_stats("[bandwidth] disconnecting user %s (break)",
                   mailboxd_session_display_name(target));
            mailboxd_session_disconnect_bandwidth(target);
            affected = 1;
        }
        break;
    }
    case MAILBOXD_BANDWIDTH_CANCEL:
        for (i = 0; i + 1 < ctx.count; i++) {
            mailboxd_log_stats("[bandwidth] disconnecting user %s (cancel)",
                   mailboxd_session_display_name(ctx.entries[i].session));
            mailboxd_session_disconnect_bandwidth(ctx.entries[i].session);
            affected++;
        }
        break;
    case MAILBOXD_BANDWIDTH_RESUME:
        for (i = 0; i < ctx.count; i++) {
            mailboxd_session_set_bandwidth_paused(ctx.entries[i].session, 0);
        }
        affected = ctx.count;
        break;
    case MAILBOXD_BANDWIDTH_NONE:
    default:
        break;
    }

    return affected;
}
