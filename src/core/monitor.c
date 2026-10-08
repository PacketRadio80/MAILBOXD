/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include "mailboxd/monitor.h"
#include "mailboxd/auth.h"
#include "mailboxd/config.h"
#include "mailboxd/log.h"
#include "mailboxd/security.h"
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MONITOR_LINE_MAX 512u
#define MONITOR_READ_CHUNK 4096u

typedef struct monitor_tail {
    char path[MAILBOXD_PATH_MAX];
    off_t offset;
    int active;
    char carry[MONITOR_LINE_MAX];
    size_t carry_len;
} monitor_tail_t;

static mailboxd_monitor_config_t g_mon_cfg;
static int g_mon_ready;
static monitor_tail_t g_tail_mailboxd;
static monitor_tail_t g_tail_security;
static int g_any_monitor_active;

void mailboxd_monitor_config_defaults(mailboxd_monitor_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 1;
    cfg->follow_mailboxd = 1;
    cfg->follow_security = 1;
    cfg->invisible_sysop = 0;
    cfg->invite_timeout_sec = 20u;
}

static void monitor_parse_allow(mailboxd_monitor_config_t *cfg, const char *raw)
{
    char buf[MAILBOXD_CONFIG_VALUE_MAX];
    char *p;
    char *comma;

    cfg->allow_count = 0;
    if (raw == NULL || raw[0] == '\0') {
        return;
    }

    mailboxd_strlcpy(buf, raw, sizeof(buf));
    p = buf;
    while (p != NULL && *p != '\0' &&
           cfg->allow_count < MAILBOXD_MONITOR_ALLOW_MAX) {
        char *tok = p;
        comma = strchr(p, ',');
        if (comma != NULL) {
            *comma = '\0';
            p = comma + 1;
        } else {
            p = NULL;
        }
        while (*tok == ' ' || *tok == '\t') {
            tok++;
        }
        if (*tok == '\0') {
            continue;
        }
        {
            char *end = tok + strlen(tok);
            while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) {
                *--end = '\0';
            }
        }
        if (tok[0] == '\0') {
            continue;
        }
        mailboxd_strlcpy(cfg->allow[cfg->allow_count], tok,
                      sizeof(cfg->allow[cfg->allow_count]));
        cfg->allow_count++;
    }
}

void mailboxd_monitor_config_apply(const struct mailboxd_config *config)
{
    const char *allow_raw;

    mailboxd_monitor_shutdown();
    mailboxd_monitor_config_defaults(&g_mon_cfg);

    if (config != NULL) {
        g_mon_cfg.enabled =
            mailboxd_config_get_bool(config, "monitor", "enabled", 1);
        g_mon_cfg.follow_mailboxd =
            mailboxd_config_get_bool(config, "monitor", "follow_mailboxd", 1);
        g_mon_cfg.follow_security =
            mailboxd_config_get_bool(config, "monitor", "follow_security", 1);
        /* Prefer hyphen key; accept underscore alias. */
        if (mailboxd_config_get(config, "monitor", "invisible-sysop", NULL) !=
            NULL) {
            g_mon_cfg.invisible_sysop = mailboxd_config_get_bool(
                config, "monitor", "invisible-sysop", 0);
        } else {
            g_mon_cfg.invisible_sysop = mailboxd_config_get_bool(
                config, "monitor", "invisible_sysop", 0);
        }
        g_mon_cfg.invite_timeout_sec = mailboxd_config_get_uint(
            config, "monitor", "invite_timeout_sec", 20u, 5u, 300u);
        allow_raw = mailboxd_config_get(config, "monitor", "allow", NULL);
        monitor_parse_allow(&g_mon_cfg, allow_raw);
    }

    g_mon_ready = 1;
    mailboxd_log_info("[monitor] enabled=%s follow_mailboxd=%s follow_security=%s "
                   "invisible-sysop=%s invite_timeout_sec=%u allow=%u",
                   g_mon_cfg.enabled ? "yes" : "no",
                   g_mon_cfg.follow_mailboxd ? "yes" : "no",
                   g_mon_cfg.follow_security ? "yes" : "no",
                   g_mon_cfg.invisible_sysop ? "yes" : "no",
                   g_mon_cfg.invite_timeout_sec,
                   g_mon_cfg.allow_count);
}

const mailboxd_monitor_config_t *mailboxd_monitor_config_get(void)
{
    return g_mon_ready ? &g_mon_cfg : NULL;
}

int mailboxd_monitor_enabled(void)
{
    return g_mon_ready && g_mon_cfg.enabled;
}

int mailboxd_monitor_invisible_sysop(void)
{
    return g_mon_ready && g_mon_cfg.invisible_sysop;
}

int mailboxd_monitor_user_allowed(const char *username)
{
    unsigned i;

    if (!g_mon_ready || username == NULL || username[0] == '\0') {
        return 0;
    }

    for (i = 0; i < g_mon_cfg.allow_count; i++) {
        if (strcasecmp(g_mon_cfg.allow[i], username) == 0) {
            return 1;
        }
    }
    return 0;
}

int mailboxd_monitor_session_may_use(const struct mailboxd_session *session)
{
    const char *user;

    if (!mailboxd_monitor_enabled() || session == NULL) {
        return 0;
    }

    if (mailboxd_user_level_is_sysop(mailboxd_session_user_level(session))) {
        return 1;
    }

    user = mailboxd_session_username(session);
    return mailboxd_monitor_user_allowed(user);
}

typedef struct mon_count_ctx {
    unsigned count;
} mon_count_ctx_t;

static void mon_count_visitor(mailboxd_session_t *session, void *userdata)
{
    mon_count_ctx_t *ctx = (mon_count_ctx_t *)userdata;

    if (session != NULL && mailboxd_monitor_is_active(session)) {
        ctx->count++;
    }
}

static void monitor_refresh_active_flag(mailboxd_service_t *service)
{
    mon_count_ctx_t ctx;

    ctx.count = 0;
    if (service != NULL) {
        mailboxd_service_visit_sessions(service, mon_count_visitor, &ctx);
    }
    g_any_monitor_active = ctx.count > 0;
}

static void monitor_tail_seek_end(monitor_tail_t *tail, const char *path)
{
    struct stat st;

    tail->path[0] = '\0';
    tail->offset = 0;
    tail->active = 0;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    mailboxd_strlcpy(tail->path, path, sizeof(tail->path));
    if (stat(tail->path, &st) != 0) {
        return;
    }

    tail->offset = st.st_size;
    tail->active = 1;
}

static void monitor_reset_tails(void)
{
    char path[MAILBOXD_PATH_MAX];

    memset(&g_tail_mailboxd, 0, sizeof(g_tail_mailboxd));
    memset(&g_tail_security, 0, sizeof(g_tail_security));

    if (g_mon_cfg.follow_mailboxd &&
        mailboxd_log_current_path(path, sizeof(path)) == MAILBOXD_OK) {
        monitor_tail_seek_end(&g_tail_mailboxd, path);
    }
    if (g_mon_cfg.follow_security &&
        mailboxd_security_log_current_path(path, sizeof(path)) == MAILBOXD_OK) {
        monitor_tail_seek_end(&g_tail_security, path);
    }
}

mailboxd_result_t mailboxd_monitor_set_active(mailboxd_session_t *session, int on)
{
    mailboxd_service_t *service;

    if (session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (!mailboxd_monitor_enabled()) {
        mailboxd_session_write_line(session, "Monitor is disabled in config.");
        return MAILBOXD_ERR_DENIED;
    }

    mailboxd_session_set_monitor_active(session, on ? 1 : 0);
    service = mailboxd_session_service(session);
    monitor_refresh_active_flag(service);

    if (on) {
        /* Fresh follow from current EOF — no backlog. */
        monitor_reset_tails();
        mailboxd_session_write_line(session, "Monitor on.");
        if (mailboxd_user_level_is_sysop(mailboxd_session_user_level(session))) {
            if (mailboxd_monitor_invisible_sysop()) {
                mailboxd_session_write_line(session,
                    "Sysop: hidden from /who (/monitor invisible-sysop=yes).");
            } else {
                mailboxd_session_write_line(session,
                    "Sysop: hidden from /who and /online while monitor is on.");
            }
        }
    } else {
        mailboxd_session_write_line(session, "Monitor off.");
        monitor_refresh_active_flag(service);
        if (!g_any_monitor_active) {
            memset(&g_tail_mailboxd, 0, sizeof(g_tail_mailboxd));
            memset(&g_tail_security, 0, sizeof(g_tail_security));
        }
    }

    return MAILBOXD_OK;
}

int mailboxd_monitor_is_active(const mailboxd_session_t *session)
{
    return mailboxd_session_monitor_active(session);
}

typedef struct mon_bcast_ctx {
    const char *line;
    mailboxd_session_t *skip;
} mon_bcast_ctx_t;

static void mon_bcast_visitor(mailboxd_session_t *session, void *userdata)
{
    mon_bcast_ctx_t *ctx = (mon_bcast_ctx_t *)userdata;
    char out[MONITOR_LINE_MAX + 16];

    if (session == NULL || ctx == NULL || ctx->line == NULL) {
        return;
    }
    if (session == ctx->skip) {
        return;
    }
    if (!mailboxd_monitor_is_active(session)) {
        return;
    }

    snprintf(out, sizeof(out), "[monitor] %s", ctx->line);
    mailboxd_session_write_line(session, out);
}

void mailboxd_monitor_broadcast(mailboxd_service_t *service, const char *line)
{
    mon_bcast_ctx_t ctx;

    if (service == NULL || line == NULL || line[0] == '\0') {
        return;
    }

    ctx.line = line;
    ctx.skip = NULL;
    mailboxd_service_visit_sessions(service, mon_bcast_visitor, &ctx);
}

void mailboxd_monitor_event(mailboxd_service_t *service,
                         const char *username,
                         const char *plugin,
                         const char *detail)
{
    char line[MONITOR_LINE_MAX];
    const char *user = (username != NULL && username[0] != '\0') ? username : "?";
    const char *plug = (plugin != NULL && plugin[0] != '\0') ? plugin : "?";

    if (service == NULL || !g_any_monitor_active) {
        /* Still compute flag lazily */
        monitor_refresh_active_flag(service);
        if (!g_any_monitor_active) {
            return;
        }
    }

    if (detail != NULL && detail[0] != '\0') {
        snprintf(line, sizeof(line), "%s@%s %s", user, plug, detail);
    } else {
        snprintf(line, sizeof(line), "%s@%s", user, plug);
    }

    mailboxd_monitor_broadcast(service, line);
}

static void monitor_push_raw_line(mailboxd_service_t *service, const char *tag,
                                  const char *text)
{
    char line[MONITOR_LINE_MAX];

    if (text == NULL) {
        return;
    }
    while (*text == '\r' || *text == '\n') {
        text++;
    }
    if (*text == '\0') {
        return;
    }

    snprintf(line, sizeof(line), "%s %s", tag, text);
    mailboxd_monitor_broadcast(service, line);
}

static void monitor_tail_read(mailboxd_service_t *service, monitor_tail_t *tail,
                              const char *tag)
{
    FILE *fp;
    struct stat st;
    char chunk[MONITOR_READ_CHUNK];
    size_t n;
    char *p;
    char *nl;

    if (tail == NULL || !tail->active || tail->path[0] == '\0') {
        return;
    }

    if (stat(tail->path, &st) != 0) {
        return;
    }

    if (st.st_size < tail->offset) {
        /* Truncated / rotated — follow from start of new content. */
        tail->offset = 0;
        tail->carry_len = 0;
    }

    if (st.st_size == tail->offset) {
        return;
    }

    fp = fopen(tail->path, "r");
    if (fp == NULL) {
        return;
    }

    if (fseeko(fp, tail->offset, SEEK_SET) != 0) {
        fclose(fp);
        return;
    }

    while ((n = fread(chunk, 1, sizeof(chunk) - 1, fp)) > 0) {
        chunk[n] = '\0';
        p = chunk;
        while (*p != '\0') {
            nl = strchr(p, '\n');
            if (nl == NULL) {
                size_t left = strlen(p);
                if (tail->carry_len + left >= sizeof(tail->carry)) {
                    tail->carry_len = 0;
                }
                memcpy(tail->carry + tail->carry_len, p, left);
                tail->carry_len += left;
                tail->carry[tail->carry_len] = '\0';
                break;
            }
            *nl = '\0';
            if (tail->carry_len > 0) {
                size_t room = sizeof(tail->carry) - 1u - tail->carry_len;
                size_t take = strlen(p);
                if (take > room) {
                    take = room;
                }
                memcpy(tail->carry + tail->carry_len, p, take);
                tail->carry_len += take;
                tail->carry[tail->carry_len] = '\0';
                monitor_push_raw_line(service, tag, tail->carry);
                tail->carry_len = 0;
            } else {
                monitor_push_raw_line(service, tag, p);
            }
            p = nl + 1;
        }
        tail->offset = ftello(fp);
    }

    fclose(fp);

    /* Refresh path if weekly rotate changed mailboxd log name. */
    if (tail == &g_tail_mailboxd) {
        char path[MAILBOXD_PATH_MAX];
        if (mailboxd_log_current_path(path, sizeof(path)) == MAILBOXD_OK &&
            strcmp(path, tail->path) != 0) {
            monitor_tail_seek_end(tail, path);
        }
    }
}

void mailboxd_monitor_tick(mailboxd_service_t *service)
{
    if (!g_mon_ready || !g_mon_cfg.enabled || service == NULL) {
        return;
    }

    monitor_refresh_active_flag(service);
    if (!g_any_monitor_active) {
        return;
    }

    if (!g_tail_mailboxd.active && g_mon_cfg.follow_mailboxd) {
        char path[MAILBOXD_PATH_MAX];
        if (mailboxd_log_current_path(path, sizeof(path)) == MAILBOXD_OK) {
            monitor_tail_seek_end(&g_tail_mailboxd, path);
        }
    }
    if (!g_tail_security.active && g_mon_cfg.follow_security) {
        char path[MAILBOXD_PATH_MAX];
        if (mailboxd_security_log_current_path(path, sizeof(path)) == MAILBOXD_OK) {
            monitor_tail_seek_end(&g_tail_security, path);
        }
    }

    if (g_mon_cfg.follow_mailboxd) {
        monitor_tail_read(service, &g_tail_mailboxd, "mailboxd:");
    }
    if (g_mon_cfg.follow_security) {
        monitor_tail_read(service, &g_tail_security, "security:");
    }
}

void mailboxd_monitor_shutdown(void)
{
    memset(&g_mon_cfg, 0, sizeof(g_mon_cfg));
    memset(&g_tail_mailboxd, 0, sizeof(g_tail_mailboxd));
    memset(&g_tail_security, 0, sizeof(g_tail_security));
    g_mon_ready = 0;
    g_any_monitor_active = 0;
}
