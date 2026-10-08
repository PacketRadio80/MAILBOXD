/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include "mailboxd/security_ban.h"
#include "mailboxd/security.h"
#include "mailboxd/config.h"
#include "mailboxd/limits.h"
#include "mailboxd/socket.h"
#include "mailboxd/storage.h"
#include "mailboxd/util.h"
#include "mailboxd/log.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef struct mailboxd_security_cfg {
    int enabled;
    unsigned maxretry;
    unsigned findtime_sec;
    unsigned bantime_sec;
    unsigned abuse_maxretry;
    unsigned abuse_findtime_sec;
    int telnet;
    unsigned rate_limit;
    unsigned rate_window_sec;
    mailboxd_ban_backend_t backend;
} mailboxd_security_cfg_t;

typedef struct ban_entry {
    char ip[MAILBOXD_REMOTE_ADDR_MAX];
    time_t expire_at;
    int active;
} ban_entry_t;

typedef struct fail_entry {
    char ip[MAILBOXD_REMOTE_ADDR_MAX];
    time_t stamps[MAILBOXD_SECURITY_DEFAULT_MAXRETRY];
    unsigned count;
    int active;
} fail_entry_t;

typedef struct rate_entry {
    char ip[MAILBOXD_REMOTE_ADDR_MAX];
    time_t stamps[32];
    unsigned count;
    int active;
} rate_entry_t;

typedef struct ban_callid_entry {
    char callid[MAILBOXD_CALLID_MAX];
    time_t expire_at;
    int active;
    int permanent;
} ban_callid_entry_t;

typedef struct callid_track_entry {
    char callid[MAILBOXD_CALLID_MAX];
    time_t stamps[MAILBOXD_SECURITY_DEFAULT_ABUSE_MAXRETRY];
    unsigned count;
    int active;
} callid_track_entry_t;

static mailboxd_security_cfg_t g_cfg;
static ban_entry_t g_bans[MAILBOXD_SECURITY_BAN_MAX];
static ban_callid_entry_t g_callid_bans[MAILBOXD_SECURITY_BAN_MAX];
static fail_entry_t g_fails[MAILBOXD_SECURITY_TRACK_MAX];
static fail_entry_t g_abuse[MAILBOXD_SECURITY_TRACK_MAX];
static callid_track_entry_t g_callid_abuse[MAILBOXD_SECURITY_TRACK_MAX];
static rate_entry_t g_rates[MAILBOXD_SECURITY_TRACK_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void security_cfg_defaults(mailboxd_security_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    cfg->enabled = 1;
    cfg->maxretry = MAILBOXD_SECURITY_DEFAULT_MAXRETRY;
    cfg->findtime_sec = MAILBOXD_SECURITY_DEFAULT_FINDTIME_SEC;
    cfg->bantime_sec = MAILBOXD_SECURITY_DEFAULT_BANTIME_SEC;
    cfg->abuse_maxretry = MAILBOXD_SECURITY_DEFAULT_ABUSE_MAXRETRY;
    cfg->abuse_findtime_sec = MAILBOXD_SECURITY_DEFAULT_ABUSE_FINDTIME_SEC;
    cfg->telnet = 1;
    cfg->rate_limit = MAILBOXD_SECURITY_DEFAULT_RATE_LIMIT;
    cfg->rate_window_sec = MAILBOXD_SECURITY_DEFAULT_RATE_WINDOW_SEC;
    cfg->backend = MAILBOXD_BAN_BACKEND_INTERNAL;
}

static mailboxd_ban_backend_t parse_ban_backend(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return MAILBOXD_BAN_BACKEND_INTERNAL;
    }

    if (strcasecmp(value, "log") == 0) {
        return MAILBOXD_BAN_BACKEND_LOG;
    }
    if (strcasecmp(value, "iptables") == 0) {
        return MAILBOXD_BAN_BACKEND_IPTABLES;
    }
    if (strcasecmp(value, "nftables") == 0) {
        return MAILBOXD_BAN_BACKEND_NFTABLES;
    }
    if (strcasecmp(value, "hosts") == 0) {
        return MAILBOXD_BAN_BACKEND_HOSTS;
    }

    return MAILBOXD_BAN_BACKEND_INTERNAL;
}

static unsigned parse_uint_clamp(const char *value, unsigned default_value,
                               unsigned max_value)
{
    char *end = NULL;
    unsigned long n;

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }

    n = strtoul(value, &end, 10);
    if (end == value || (end != NULL && *end != '\0')) {
        return default_value;
    }

    if (n > max_value) {
        return max_value;
    }

    return (unsigned)n;
}

static int ip_valid(const char *ip)
{
    return ip != NULL && ip[0] != '\0' && strcmp(ip, "?") != 0;
}

int mailboxd_security_callid_normalize(const char *in, char *out, size_t out_cap)
{
    size_t len;
    size_t i;
    int has_alpha = 0;

    if (in == NULL || out == NULL || out_cap < 2u) {
        return 0;
    }

    while (*in == ' ' || *in == '\t') {
        in++;
    }

    len = 0;
    for (i = 0; in[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)in[i];

        if (ch == ' ' || ch == '\t') {
            break;
        }

        if (len + 1 >= out_cap) {
            return 0;
        }

        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
            out[len++] = (char)toupper(ch);
            has_alpha = 1;
        } else if (ch >= '0' && ch <= '9') {
            out[len++] = (char)ch;
        } else if (ch == '-' || ch == '_' || ch == '.') {
            out[len++] = (char)ch;
        } else {
            return 0;
        }
    }

    out[len] = '\0';
    return len > 0 && has_alpha;
}

static ban_callid_entry_t *callid_ban_find(const char *callid)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_BAN_MAX; i++) {
        if (g_callid_bans[i].active &&
            strcmp(g_callid_bans[i].callid, callid) == 0) {
            return &g_callid_bans[i];
        }
    }

    return NULL;
}

static ban_callid_entry_t *callid_ban_alloc(const char *callid)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_BAN_MAX; i++) {
        if (!g_callid_bans[i].active) {
            mailboxd_strlcpy(g_callid_bans[i].callid, callid,
                          sizeof(g_callid_bans[i].callid));
            g_callid_bans[i].active = 1;
            return &g_callid_bans[i];
        }
    }

    return NULL;
}

static callid_track_entry_t *callid_abuse_find(const char *callid)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_TRACK_MAX; i++) {
        if (g_callid_abuse[i].active &&
            strcmp(g_callid_abuse[i].callid, callid) == 0) {
            return &g_callid_abuse[i];
        }
    }

    return NULL;
}

static callid_track_entry_t *callid_abuse_alloc(const char *callid)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_TRACK_MAX; i++) {
        if (!g_callid_abuse[i].active) {
            mailboxd_strlcpy(g_callid_abuse[i].callid, callid,
                          sizeof(g_callid_abuse[i].callid));
            g_callid_abuse[i].count = 0;
            g_callid_abuse[i].active = 1;
            return &g_callid_abuse[i];
        }
    }

    return NULL;
}

static void prune_callid_abuse_window(callid_track_entry_t *entry, time_t now)
{
    unsigned i;
    unsigned kept = 0;

    if (entry == NULL) {
        return;
    }

    for (i = 0; i < entry->count; i++) {
        if ((time_t)(now - entry->stamps[i]) <=
            (time_t)g_cfg.abuse_findtime_sec) {
            entry->stamps[kept++] = entry->stamps[i];
        }
    }

    entry->count = kept;
    if (entry->count == 0) {
        entry->active = 0;
        entry->callid[0] = '\0';
    }
}

static void backend_apply_callid(const char *callid, const char *reason)
{
    mailboxd_security_log_write("ban callid=%s reason=%s backend=internal",
                             callid, reason != NULL ? reason : "abuse");
    (void)g_cfg.backend;
}

static void apply_callid_ban_locked(const char *callid, const char *reason,
                                    time_t now, int permanent)
{
    ban_callid_entry_t *ban;

    ban = callid_ban_find(callid);
    if (ban == NULL) {
        ban = callid_ban_alloc(callid);
    }

    if (ban == NULL) {
        return;
    }

    ban->permanent = permanent ? 1 : 0;
    ban->expire_at = permanent ? (time_t)0 :
                     now + (time_t)g_cfg.bantime_sec;
    backend_apply_callid(callid, reason);
}

static void config_clear_permanent_callid_bans_locked(void)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_BAN_MAX; i++) {
        if (g_callid_bans[i].active && g_callid_bans[i].permanent) {
            g_callid_bans[i].active = 0;
            g_callid_bans[i].callid[0] = '\0';
            g_callid_bans[i].permanent = 0;
        }
    }
}

static void config_load_callid_bans_locked(const char *list)
{
    char buf[MAILBOXD_PATH_MAX];
    char norm[MAILBOXD_CALLID_MAX];
    char *save = NULL;
    char *token;

    if (list == NULL || list[0] == '\0') {
        return;
    }

    mailboxd_strlcpy(buf, list, sizeof(buf));
    token = strtok_r(buf, ",", &save);
    while (token != NULL) {
        while (*token == ' ' || *token == '\t') {
            token++;
        }
        if (mailboxd_security_callid_normalize(token, norm, sizeof(norm))) {
            apply_callid_ban_locked(norm, "config", time(NULL), 1);
        }
        token = strtok_r(NULL, ",", &save);
    }
}

static void record_callid_abuse_locked(const char *callid, const char *category,
                                       time_t now)
{
    callid_track_entry_t *entry;
    char reason[64];

    entry = callid_abuse_find(callid);
    if (entry == NULL) {
        entry = callid_abuse_alloc(callid);
    }

    if (entry == NULL) {
        return;
    }

    prune_callid_abuse_window(entry, now);

    if (!entry->active) {
        entry = callid_abuse_alloc(callid);
        if (entry == NULL) {
            return;
        }
    }

    if (entry->count < MAILBOXD_SECURITY_DEFAULT_ABUSE_MAXRETRY) {
        entry->stamps[entry->count++] = now;
    } else {
        memmove(entry->stamps, entry->stamps + 1,
                (entry->count - 1) * sizeof(entry->stamps[0]));
        entry->stamps[entry->count - 1] = now;
    }

    if (entry->count >= g_cfg.abuse_maxretry) {
        snprintf(reason, sizeof(reason), "abuse:%s",
                 category != NULL && category[0] != '\0' ? category : "flood");
        apply_callid_ban_locked(callid, reason, now, 0);
        entry->active = 0;
        entry->count = 0;
        entry->callid[0] = '\0';
    }
}

static int callid_is_banned_locked(const char *callid, time_t now)
{
    ban_callid_entry_t *ban;

    ban = callid_ban_find(callid);
    if (ban == NULL) {
        return 0;
    }

    if (ban->permanent) {
        return 1;
    }

    if (now < ban->expire_at) {
        return 1;
    }

    ban->active = 0;
    ban->callid[0] = '\0';
    ban->permanent = 0;
    return 0;
}

static int transport_enabled(const char *transport)
{
    if (!g_cfg.enabled || transport == NULL || transport[0] == '\0') {
        return 0;
    }

    if (strcmp(transport, "telnet") == 0) {
        return g_cfg.telnet;
    }
    if (strcmp(transport, "ssh") == 0 || strcmp(transport, "websocket") == 0 ||
        strcmp(transport, "circuit") == 0) {
        return 0; /* transports removed */
    }

    return 1;
}

static ban_entry_t *ban_find(const char *ip)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_BAN_MAX; i++) {
        if (g_bans[i].active && strcmp(g_bans[i].ip, ip) == 0) {
            return &g_bans[i];
        }
    }

    return NULL;
}

static ban_entry_t *ban_alloc(const char *ip)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_BAN_MAX; i++) {
        if (!g_bans[i].active) {
            mailboxd_strlcpy(g_bans[i].ip, ip, sizeof(g_bans[i].ip));
            g_bans[i].active = 1;
            return &g_bans[i];
        }
    }

    return NULL;
}

static fail_entry_t *fail_find(const char *ip)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_TRACK_MAX; i++) {
        if (g_fails[i].active && strcmp(g_fails[i].ip, ip) == 0) {
            return &g_fails[i];
        }
    }

    return NULL;
}

static fail_entry_t *fail_alloc(const char *ip)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_TRACK_MAX; i++) {
        if (!g_fails[i].active) {
            mailboxd_strlcpy(g_fails[i].ip, ip, sizeof(g_fails[i].ip));
            g_fails[i].count = 0;
            g_fails[i].active = 1;
            return &g_fails[i];
        }
    }

    return NULL;
}

static fail_entry_t *abuse_find(const char *ip)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_TRACK_MAX; i++) {
        if (g_abuse[i].active && strcmp(g_abuse[i].ip, ip) == 0) {
            return &g_abuse[i];
        }
    }

    return NULL;
}

static fail_entry_t *abuse_alloc(const char *ip)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_TRACK_MAX; i++) {
        if (!g_abuse[i].active) {
            mailboxd_strlcpy(g_abuse[i].ip, ip, sizeof(g_abuse[i].ip));
            g_abuse[i].count = 0;
            g_abuse[i].active = 1;
            return &g_abuse[i];
        }
    }

    return NULL;
}

static rate_entry_t *rate_find(const char *ip)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_TRACK_MAX; i++) {
        if (g_rates[i].active && strcmp(g_rates[i].ip, ip) == 0) {
            return &g_rates[i];
        }
    }

    return NULL;
}

static rate_entry_t *rate_alloc(const char *ip)
{
    size_t i;

    for (i = 0; i < MAILBOXD_SECURITY_TRACK_MAX; i++) {
        if (!g_rates[i].active) {
            mailboxd_strlcpy(g_rates[i].ip, ip, sizeof(g_rates[i].ip));
            g_rates[i].count = 0;
            g_rates[i].active = 1;
            return &g_rates[i];
        }
    }

    return NULL;
}

static void prune_fail_window(fail_entry_t *entry, time_t now)
{
    unsigned i;
    unsigned kept = 0;

    if (entry == NULL) {
        return;
    }

    for (i = 0; i < entry->count; i++) {
        if ((time_t)(now - entry->stamps[i]) <= (time_t)g_cfg.findtime_sec) {
            entry->stamps[kept++] = entry->stamps[i];
        }
    }

    entry->count = kept;
    if (entry->count == 0) {
        entry->active = 0;
        entry->ip[0] = '\0';
    }
}

static void prune_abuse_window(fail_entry_t *entry, time_t now)
{
    unsigned i;
    unsigned kept = 0;

    if (entry == NULL) {
        return;
    }

    for (i = 0; i < entry->count; i++) {
        if ((time_t)(now - entry->stamps[i]) <=
            (time_t)g_cfg.abuse_findtime_sec) {
            entry->stamps[kept++] = entry->stamps[i];
        }
    }

    entry->count = kept;
    if (entry->count == 0) {
        entry->active = 0;
        entry->ip[0] = '\0';
    }
}

static void prune_rate_window(rate_entry_t *entry, time_t now)
{
    unsigned i;
    unsigned kept = 0;

    if (entry == NULL) {
        return;
    }

    for (i = 0; i < entry->count; i++) {
        if ((time_t)(now - entry->stamps[i]) <= (time_t)g_cfg.rate_window_sec) {
            entry->stamps[kept++] = entry->stamps[i];
        }
    }

    entry->count = kept;
    if (entry->count == 0) {
        entry->active = 0;
        entry->ip[0] = '\0';
    }
}

static int run_backend_cmd(const char *cmd)
{
    int rc;

    if (cmd == NULL || cmd[0] == '\0') {
        return -1;
    }

    rc = system(cmd);
    return rc;
}

static void backend_apply(const char *ip, const char *reason)
{
    char cmd[MAILBOXD_PATH_MAX];

    switch (g_cfg.backend) {
    case MAILBOXD_BAN_BACKEND_LOG:
        mailboxd_security_log_write("ban ip=%s reason=%s backend=log",
                               ip, reason != NULL ? reason : "abuse");
        break;

    case MAILBOXD_BAN_BACKEND_IPTABLES:
        snprintf(cmd, sizeof(cmd),
                 "iptables -I INPUT -s %s -j DROP 2>/dev/null", ip);
        if (run_backend_cmd(cmd) != 0) {
            mailboxd_security_log_write(
                "ban_backend_fail ip=%s backend=iptables", ip);
        } else {
            mailboxd_security_log_write(
                "ban ip=%s reason=%s backend=iptables",
                ip, reason != NULL ? reason : "abuse");
        }
        break;

    case MAILBOXD_BAN_BACKEND_NFTABLES:
        snprintf(cmd, sizeof(cmd),
                 "nft add rule inet filter input ip saddr %s drop "
                 "2>/dev/null",
                 ip);
        if (run_backend_cmd(cmd) != 0) {
            mailboxd_security_log_write(
                "ban_backend_fail ip=%s backend=nftables", ip);
        } else {
            mailboxd_security_log_write(
                "ban ip=%s reason=%s backend=nftables",
                ip, reason != NULL ? reason : "abuse");
        }
        break;

    case MAILBOXD_BAN_BACKEND_HOSTS:
        mailboxd_security_log_write(
            "ban ip=%s reason=%s backend=hosts (stub — internal only)",
            ip, reason != NULL ? reason : "abuse");
        break;

    case MAILBOXD_BAN_BACKEND_INTERNAL:
    default:
        mailboxd_security_log_write("ban ip=%s reason=%s backend=internal",
                               ip, reason != NULL ? reason : "abuse");
        break;
    }
}

static void apply_ban_locked(const char *ip, const char *reason, time_t now)
{
    ban_entry_t *ban;

    ban = ban_find(ip);
    if (ban == NULL) {
        ban = ban_alloc(ip);
    }

    if (ban == NULL) {
        return;
    }

    ban->expire_at = now + (time_t)g_cfg.bantime_sec;
    backend_apply(ip, reason);
}

static void record_failure_locked(const char *ip, time_t now)
{
    fail_entry_t *entry;

    entry = fail_find(ip);
    if (entry == NULL) {
        entry = fail_alloc(ip);
    }

    if (entry == NULL) {
        return;
    }

    prune_fail_window(entry, now);

    if (!entry->active) {
        entry = fail_alloc(ip);
        if (entry == NULL) {
            return;
        }
    }

    if (entry->count < MAILBOXD_SECURITY_DEFAULT_MAXRETRY) {
        entry->stamps[entry->count++] = now;
    } else {
        memmove(entry->stamps, entry->stamps + 1,
                (entry->count - 1) * sizeof(entry->stamps[0]));
        entry->stamps[entry->count - 1] = now;
    }

    if (entry->count >= g_cfg.maxretry) {
        apply_ban_locked(ip, "login_fail", now);
        entry->active = 0;
        entry->count = 0;
        entry->ip[0] = '\0';
    }
}

static void record_abuse_locked(const char *ip, const char *category, time_t now)
{
    fail_entry_t *entry;
    char reason[64];

    entry = abuse_find(ip);
    if (entry == NULL) {
        entry = abuse_alloc(ip);
    }

    if (entry == NULL) {
        return;
    }

    prune_abuse_window(entry, now);

    if (!entry->active) {
        entry = abuse_alloc(ip);
        if (entry == NULL) {
            return;
        }
    }

    if (entry->count < MAILBOXD_SECURITY_DEFAULT_ABUSE_MAXRETRY) {
        entry->stamps[entry->count++] = now;
    } else {
        memmove(entry->stamps, entry->stamps + 1,
                (entry->count - 1) * sizeof(entry->stamps[0]));
        entry->stamps[entry->count - 1] = now;
    }

    if (entry->count >= g_cfg.abuse_maxretry) {
        snprintf(reason, sizeof(reason), "abuse:%s",
                 category != NULL && category[0] != '\0' ? category : "flood");
        apply_ban_locked(ip, reason, now);
        entry->active = 0;
        entry->count = 0;
        entry->ip[0] = '\0';
    }
}

static void record_rate_locked(const char *ip, time_t now)
{
    rate_entry_t *entry;

    entry = rate_find(ip);
    if (entry == NULL) {
        entry = rate_alloc(ip);
    }

    if (entry == NULL) {
        return;
    }

    prune_rate_window(entry, now);

    if (!entry->active) {
        entry = rate_alloc(ip);
        if (entry == NULL) {
            return;
        }
    }

    if (entry->count < (unsigned)(sizeof(entry->stamps) / sizeof(entry->stamps[0]))) {
        entry->stamps[entry->count++] = now;
    }
}

void mailboxd_security_ban_config_apply(const struct mailboxd_config *config)
{
    const char *value;

    pthread_mutex_lock(&g_lock);

    security_cfg_defaults(&g_cfg);

    if (config != NULL) {
        value = mailboxd_config_get(config, "security", "enabled", NULL);
        if (value != NULL) {
            g_cfg.enabled = mailboxd_parse_bool(value, g_cfg.enabled);
        }

        value = mailboxd_config_get(config, "security", "maxretry", NULL);
        g_cfg.maxretry = parse_uint_clamp(value, g_cfg.maxretry, 100u);
        if (g_cfg.maxretry < 1u) {
            g_cfg.maxretry = 1u;
        }

        value = mailboxd_config_get(config, "security", "findtime", NULL);
        g_cfg.findtime_sec =
            parse_uint_clamp(value, g_cfg.findtime_sec, 86400u);

        value = mailboxd_config_get(config, "security", "bantime", NULL);
        g_cfg.bantime_sec =
            parse_uint_clamp(value, g_cfg.bantime_sec, 86400u);

        value = mailboxd_config_get(config, "security", "abuse_maxretry", NULL);
        g_cfg.abuse_maxretry =
            parse_uint_clamp(value, g_cfg.abuse_maxretry, 1000u);
        if (g_cfg.abuse_maxretry < 1u) {
            g_cfg.abuse_maxretry = 1u;
        }

        value = mailboxd_config_get(config, "security", "abuse_findtime", NULL);
        g_cfg.abuse_findtime_sec =
            parse_uint_clamp(value, g_cfg.abuse_findtime_sec, 86400u);

        value = mailboxd_config_get(config, "security", "telnet", NULL);
        if (value != NULL) {
            g_cfg.telnet = mailboxd_parse_bool(value, g_cfg.telnet);
        }

        /* ssh/websocket/circuit keys in [security] are silently ignored:
         * those transports are gone. Operators who still have the lines
         * in their INI get nothing broken - just a no-op read. */
        (void)mailboxd_config_get(config, "security", "ssh", NULL);
        (void)mailboxd_config_get(config, "security", "websocket", NULL);
        (void)mailboxd_config_get(config, "security", "circuit", NULL);

        value = mailboxd_config_get(config, "security", "rate_limit", NULL);
        g_cfg.rate_limit =
            parse_uint_clamp(value, g_cfg.rate_limit, 10000u);

        value = mailboxd_config_get(config, "security", "rate_window", NULL);
        g_cfg.rate_window_sec =
            parse_uint_clamp(value, g_cfg.rate_window_sec, 3600u);

        value = mailboxd_config_get(config, "security", "ban_backend", NULL);
        g_cfg.backend = parse_ban_backend(value);

        config_clear_permanent_callid_bans_locked();
        value = mailboxd_config_get(config, "security", "ban_callid", NULL);
        config_load_callid_bans_locked(value);
    }

    pthread_mutex_unlock(&g_lock);

    if (g_cfg.enabled) {
        mailboxd_log_info("[security] ban enabled maxretry=%u findtime=%us bantime=%us "
               "abuse_maxretry=%u abuse_findtime=%us "
               "rate_limit=%u/%us backend=%d",
               g_cfg.maxretry, g_cfg.findtime_sec, g_cfg.bantime_sec,
               g_cfg.abuse_maxretry, g_cfg.abuse_findtime_sec,
               g_cfg.rate_limit, g_cfg.rate_window_sec, (int)g_cfg.backend);
    }
}

void mailboxd_security_ban_shutdown(void)
{
    pthread_mutex_lock(&g_lock);

    memset(g_bans, 0, sizeof(g_bans));
    memset(g_callid_bans, 0, sizeof(g_callid_bans));
    memset(g_fails, 0, sizeof(g_fails));
    memset(g_abuse, 0, sizeof(g_abuse));
    memset(g_callid_abuse, 0, sizeof(g_callid_abuse));
    memset(g_rates, 0, sizeof(g_rates));
    security_cfg_defaults(&g_cfg);

    pthread_mutex_unlock(&g_lock);
}

void mailboxd_security_ban_tick(void)
{
    time_t now = time(NULL);
    size_t i;

    if (!g_cfg.enabled) {
        return;
    }

    pthread_mutex_lock(&g_lock);

    for (i = 0; i < MAILBOXD_SECURITY_BAN_MAX; i++) {
        if (g_bans[i].active && now >= g_bans[i].expire_at) {
            mailboxd_security_log_write("unban ip=%s", g_bans[i].ip);
            g_bans[i].active = 0;
            g_bans[i].ip[0] = '\0';
        }
        if (g_callid_bans[i].active && !g_callid_bans[i].permanent &&
            g_callid_bans[i].expire_at > (time_t)0 &&
            now >= g_callid_bans[i].expire_at) {
            mailboxd_security_log_write("unban callid=%s", g_callid_bans[i].callid);
            g_callid_bans[i].active = 0;
            g_callid_bans[i].callid[0] = '\0';
            g_callid_bans[i].permanent = 0;
        }
    }

    for (i = 0; i < MAILBOXD_SECURITY_TRACK_MAX; i++) {
        if (g_fails[i].active) {
            prune_fail_window(&g_fails[i], now);
        }
        if (g_abuse[i].active) {
            prune_abuse_window(&g_abuse[i], now);
        }
        if (g_callid_abuse[i].active) {
            prune_callid_abuse_window(&g_callid_abuse[i], now);
        }
        if (g_rates[i].active) {
            prune_rate_window(&g_rates[i], now);
        }
    }

    pthread_mutex_unlock(&g_lock);
}

int mailboxd_security_ban_is_banned(const char *ip)
{
    ban_entry_t *ban;
    time_t now = time(NULL);
    int banned = 0;

    if (!g_cfg.enabled || !ip_valid(ip)) {
        return 0;
    }

    pthread_mutex_lock(&g_lock);

    ban = ban_find(ip);
    if (ban != NULL) {
        if (now < ban->expire_at) {
            banned = 1;
        } else {
            ban->active = 0;
            ban->ip[0] = '\0';
        }
    }

    pthread_mutex_unlock(&g_lock);
    return banned;
}

int mailboxd_security_ban_accept(const char *ip)
{
    rate_entry_t *entry;
    ban_entry_t *ban;
    time_t now = time(NULL);
    int allow = 1;

    if (!g_cfg.enabled || !ip_valid(ip)) {
        return 1;
    }

    pthread_mutex_lock(&g_lock);

    ban = ban_find(ip);
    if (ban != NULL) {
        if (now < ban->expire_at) {
            allow = 0;
        } else {
            ban->active = 0;
            ban->ip[0] = '\0';
        }
    }

    if (allow && g_cfg.rate_limit > 0u && g_cfg.rate_window_sec > 0u) {
        entry = rate_find(ip);
        if (entry != NULL) {
            prune_rate_window(entry, now);
        }

        if (entry != NULL && entry->active &&
            entry->count >= g_cfg.rate_limit) {
            apply_ban_locked(ip, "rate_limit", now);
            allow = 0;
        } else {
            record_rate_locked(ip, now);
        }
    }

    pthread_mutex_unlock(&g_lock);
    return allow;
}

int mailboxd_security_ban_accept_fd(int fd)
{
    char ip[MAILBOXD_REMOTE_ADDR_MAX];

    if (fd < 0) {
        return 1;
    }

    if (mailboxd_socket_peer_name(fd, ip, sizeof(ip)) != MAILBOXD_OK) {
        return 1;
    }

    return mailboxd_security_ban_accept(ip);
}

void mailboxd_security_ban_login_fail(const char *ip, const char *transport)
{
    time_t now = time(NULL);

    if (!g_cfg.enabled || !ip_valid(ip) || !transport_enabled(transport)) {
        return;
    }

    pthread_mutex_lock(&g_lock);
    record_failure_locked(ip, now);
    pthread_mutex_unlock(&g_lock);
}

void mailboxd_security_ban_abuse_report(const char *ip, const char *category)
{
    time_t now = time(NULL);

    if (!g_cfg.enabled || !ip_valid(ip)) {
        return;
    }

    pthread_mutex_lock(&g_lock);
    record_abuse_locked(ip, category, now);
    pthread_mutex_unlock(&g_lock);
}

int mailboxd_security_ban_callid_is_banned(const char *callid)
{
    char norm[MAILBOXD_CALLID_MAX];
    time_t now = time(NULL);
    int banned = 0;

    if (!g_cfg.enabled || !mailboxd_security_callid_normalize(callid, norm,
                                                           sizeof(norm))) {
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    banned = callid_is_banned_locked(norm, now);
    pthread_mutex_unlock(&g_lock);
    return banned;
}

int mailboxd_security_ban_callid_accept(const char *callid)
{
    return !mailboxd_security_ban_callid_is_banned(callid);
}

void mailboxd_security_ban_callid_abuse_report(const char *callid,
                                            const char *category)
{
    char norm[MAILBOXD_CALLID_MAX];
    time_t now = time(NULL);

    if (!g_cfg.enabled || !mailboxd_security_callid_normalize(callid, norm,
                                                           sizeof(norm))) {
        return;
    }

    pthread_mutex_lock(&g_lock);
    record_callid_abuse_locked(norm, category, now);
    pthread_mutex_unlock(&g_lock);
}
