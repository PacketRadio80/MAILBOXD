/*
 * Centralized daemon: INI apply, plugin lifecycle, sessions. Wire
 * protocols stay in plugins/ (telnet, mailboxd_prterm — the WebSocket
 * transport was removed in 2026-10-08; PRTERM is the only web
 * front-end; the HBX circuit hub was removed 2026-10-08, telnet is
 * the only CLI transport now).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mailboxd/service.h"
#include "mailboxd/session.h"
#include "storage_private.h"
#include "mailboxd/registry.h"
#include "mailboxd/config.h"
#include "mailboxd/command.h"
#include "mailboxd/crypto_config.h"
#include "mailboxd/traffic.h"
#include "mailboxd/auth.h"
#include "mailboxd/storage.h"
#include "mailboxd/texts.h"
#include "mailboxd/chat.h"
#include "mailboxd/mail.h"
#include "mailboxd/broadcast.h"
#include "mailboxd/networks.h"
#include "mailboxd/instance.h"
#include "mailboxd/log.h"
#include "mailboxd/monitor.h"
#include "mailboxd/entertain_config.h"
#include <errno.h>
#include "mailboxd/security.h"
#include "mailboxd/security_ban.h"
#include "mailboxd/util.h"
#include "mailboxd/limits.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define MAILBOXD_MAX_ACTIVE_TRANSPORTS 8

static char *mailboxd_strdup(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }

    len = strlen(s) + 1;
    copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

typedef struct active_transport {
    const mailboxd_transport_plugin_t *plugin;
    int running;
} active_transport_t;

typedef struct mailboxd_attached_session {
    struct mailboxd_session *session;
    struct mailboxd_attached_session *next;
} mailboxd_attached_session_t;

#define MAILBOXD_DEFAULT_DATA_PATH "data"

struct mailboxd_service_internal {
    char *name;
    char config_path[MAILBOXD_PATH_MAX];
    const mailboxd_config_t *config; /* live config; plugins read "enabled" etc. */
    char prompt[MAILBOXD_PROMPT_MAX];
    unsigned max_online;
    unsigned guest_timeout_minutes;
    int login_announce;
    unsigned active_nodes;
    pthread_mutex_t node_lock;
    pthread_mutex_t session_lock;
    mailboxd_attached_session_t *sessions;
    mailboxd_storage_t *storage;
    mailboxd_auth_config_t auth;
    mailboxd_texts_config_t texts;
    mailboxd_chat_config_t chat;
    mailboxd_mail_config_t mail;
    mailboxd_broadcast_config_t broadcast;
    mailboxd_networks_config_t networks;
    active_transport_t transports[MAILBOXD_MAX_ACTIVE_TRANSPORTS];
    size_t transport_count;
    int running;
    mailboxd_shutdown_mode_t shutdown_mode;
    char launch_binary[MAILBOXD_PATH_MAX];
    pthread_mutex_t guest_lock;
    unsigned char guest_in_use[MAILBOXD_GUEST_NUMBER_MAX + 1];
};

mailboxd_service_t *mailboxd_service_create(const char *name)
{
    struct mailboxd_service_internal *svc;

    svc = calloc(1, sizeof(*svc));
    if (svc == NULL) {
        return NULL;
    }

    mailboxd_auth_config_defaults(&svc->auth);
    mailboxd_texts_config_defaults(&svc->texts);
    mailboxd_chat_config_defaults(&svc->chat);
    mailboxd_mail_config_defaults(&svc->mail);
    mailboxd_networks_config_defaults(&svc->networks);
    svc->max_online = MAILBOXD_DEFAULT_MAX_ONLINE;
    svc->guest_timeout_minutes = MAILBOXD_DEFAULT_GUEST_TIMEOUT_MINUTES;
    svc->login_announce = 0;
    svc->active_nodes = 0;
    pthread_mutex_init(&svc->node_lock, NULL);
    pthread_mutex_init(&svc->session_lock, NULL);
    pthread_mutex_init(&svc->guest_lock, NULL);

    svc->name = mailboxd_strdup(name != NULL && name[0] != '\0' ?
                             name : MAILBOXD_DEFAULT_SERVICE_NAME);
    if (svc->name == NULL) {
        free(svc);
        return NULL;
    }

    return (mailboxd_service_t *)svc;
}

void mailboxd_service_destroy(mailboxd_service_t *service)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;
    size_t i;

    if (svc == NULL) {
        return;
    }

    mailboxd_service_stop(service);

    for (i = 0; i < svc->transport_count; i++) {
        if (svc->transports[i].running &&
            svc->transports[i].plugin->stop != NULL) {
            svc->transports[i].plugin->stop();
        }
        if (svc->transports[i].plugin->shutdown != NULL) {
            svc->transports[i].plugin->shutdown();
        }
    }

    if (svc->storage != NULL) {
        mailboxd_storage_close(svc->storage);
        svc->storage = NULL;
    }

    mailboxd_monitor_shutdown();
    mailboxd_log_shutdown();
    mailboxd_security_log_shutdown();
    mailboxd_security_ban_shutdown();

    {
        mailboxd_attached_session_t *node = svc->sessions;

        while (node != NULL) {
            mailboxd_attached_session_t *next = node->next;

            free(node);
            node = next;
        }
        svc->sessions = NULL;
    }

    pthread_mutex_destroy(&svc->session_lock);
    pthread_mutex_destroy(&svc->guest_lock);
    pthread_mutex_destroy(&svc->node_lock);
    free(svc->name);
    free(svc);
}

mailboxd_storage_t *mailboxd_service_get_storage(mailboxd_service_t *service)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return NULL;
    }

    return svc->storage;
}

const mailboxd_config_t *mailboxd_service_get_config(mailboxd_service_t *service)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return NULL;
    }

    return svc->config;
}

const mailboxd_auth_config_t *mailboxd_service_get_auth(mailboxd_service_t *service)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return NULL;
    }

    return &svc->auth;
}

const mailboxd_texts_config_t *mailboxd_service_get_texts(mailboxd_service_t *service)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return NULL;
    }

    return &svc->texts;
}

const mailboxd_chat_config_t *mailboxd_service_get_chat(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return NULL;
    }

    return &svc->chat;
}

const mailboxd_mail_config_t *mailboxd_service_get_mail(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return NULL;
    }

    return &svc->mail;
}

const mailboxd_broadcast_config_t *mailboxd_service_get_broadcast(
    const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return NULL;
    }

    return &svc->broadcast;
}


const char *mailboxd_service_get_prompt(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return "";
    }

    return svc->prompt;
}

const char *mailboxd_service_get_name(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;

    if (svc == NULL || svc->name == NULL || svc->name[0] == '\0') {
        return MAILBOXD_DEFAULT_SERVICE_NAME;
    }

    return svc->name;
}

unsigned mailboxd_service_max_online(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return MAILBOXD_DEFAULT_MAX_ONLINE;
    }

    return svc->max_online;
}

int mailboxd_service_login_announce(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;

    return svc != NULL && svc->login_announce != 0;
}

unsigned mailboxd_service_active_nodes(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;
    unsigned count;

    if (svc == NULL) {
        return 0;
    }

    pthread_mutex_lock((pthread_mutex_t *)&svc->node_lock);
    count = svc->active_nodes;
    pthread_mutex_unlock((pthread_mutex_t *)&svc->node_lock);

    return count;
}

unsigned mailboxd_service_guest_timeout_seconds(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return MAILBOXD_DEFAULT_GUEST_TIMEOUT_MINUTES * 60u;
    }

    return svc->guest_timeout_minutes * 60u;
}

mailboxd_result_t mailboxd_service_guest_assign(mailboxd_service_t *service,
                                          const char *guest_prefix,
                                          mailboxd_user_record_t *out,
                                          unsigned *slot_out)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;
    unsigned slot;

    if (svc == NULL || out == NULL || slot_out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    pthread_mutex_lock(&svc->guest_lock);
    for (slot = 1; slot <= MAILBOXD_GUEST_NUMBER_MAX; slot++) {
        if (!svc->guest_in_use[slot]) {
            svc->guest_in_use[slot] = 1;
            pthread_mutex_unlock(&svc->guest_lock);

            mailboxd_guest_fill_record(guest_prefix, slot, out);
            *slot_out = slot;
            return MAILBOXD_OK;
        }
    }
    pthread_mutex_unlock(&svc->guest_lock);

    return MAILBOXD_ERR_BUSY;
}

void mailboxd_service_guest_release(mailboxd_service_t *service, unsigned slot)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc == NULL || slot < 1 || slot > MAILBOXD_GUEST_NUMBER_MAX) {
        return;
    }

    pthread_mutex_lock(&svc->guest_lock);
    svc->guest_in_use[slot] = 0;
    pthread_mutex_unlock(&svc->guest_lock);
}

mailboxd_result_t mailboxd_service_acquire_node(mailboxd_service_t *service)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    pthread_mutex_lock(&svc->node_lock);
    if (svc->active_nodes >= svc->max_online) {
        pthread_mutex_unlock(&svc->node_lock);
        return MAILBOXD_ERR_BUSY;
    }

    svc->active_nodes++;
    pthread_mutex_unlock(&svc->node_lock);
    return MAILBOXD_OK;
}

void mailboxd_service_release_node(mailboxd_service_t *service)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return;
    }

    pthread_mutex_lock(&svc->node_lock);
    if (svc->active_nodes > 0) {
        svc->active_nodes--;
    }
    pthread_mutex_unlock(&svc->node_lock);
}

mailboxd_result_t mailboxd_service_attach_session(mailboxd_service_t *service,
                                            mailboxd_session_t *session)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;
    mailboxd_attached_session_t *node;

    if (svc == NULL || session == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    node = calloc(1, sizeof(*node));
    if (node == NULL) {
        return MAILBOXD_ERR_NOMEM;
    }

    node->session = session;

    pthread_mutex_lock(&svc->session_lock);
    node->next = svc->sessions;
    svc->sessions = node;
    pthread_mutex_unlock(&svc->session_lock);

    return MAILBOXD_OK;
}

void mailboxd_service_detach_session(mailboxd_service_t *service,
                                  mailboxd_session_t *session)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;
    mailboxd_attached_session_t *prev;
    mailboxd_attached_session_t *node;

    if (svc == NULL || session == NULL) {
        return;
    }

    pthread_mutex_lock(&svc->session_lock);
    prev = NULL;
    node = svc->sessions;
    while (node != NULL) {
        if (node->session == session) {
            if (prev != NULL) {
                prev->next = node->next;
            } else {
                svc->sessions = node->next;
            }
            free(node);
            break;
        }
        prev = node;
        node = node->next;
    }
    pthread_mutex_unlock(&svc->session_lock);
}

void mailboxd_service_visit_sessions(mailboxd_service_t *service,
                                  mailboxd_service_session_visit_fn fn,
                                  void *userdata)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;
    mailboxd_attached_session_t *node;
    mailboxd_session_t **snapshot = NULL;
    size_t count = 0;
    size_t i;

    if (svc == NULL || fn == NULL) {
        return;
    }

    /*
     * Snapshot session pointers under session_lock, then invoke callbacks
     * without holding the lock. Visitors may write to sessions, which can
     * re-enter via the bandwidth policy.
     */
    pthread_mutex_lock(&svc->session_lock);
    for (node = svc->sessions; node != NULL; node = node->next) {
        if (node->session != NULL) {
            count++;
        }
    }

    if (count > 0) {
        snapshot = calloc(count, sizeof(*snapshot));
        if (snapshot == NULL) {
            mailboxd_log_warn("[service] session snapshot OOM (%zu sessions) — skipped",
                           count);
        } else {
            i = 0;
            for (node = svc->sessions; node != NULL; node = node->next) {
                if (node->session != NULL) {
                    snapshot[i++] = node->session;
                }
            }
        }
    }
    pthread_mutex_unlock(&svc->session_lock);

    if (snapshot == NULL) {
        return;
    }

    for (i = 0; i < count; i++) {
        fn(snapshot[i], userdata);
    }
    free(snapshot);
}

typedef struct find_registered_session_ctx {
    uint64_t user_id;
    mailboxd_session_t *exclude;
    mailboxd_session_t *found;
} find_registered_session_ctx_t;

static void find_registered_session_visitor(mailboxd_session_t *session,
                                            void *userdata)
{
    find_registered_session_ctx_t *ctx = (find_registered_session_ctx_t *)userdata;
    const mailboxd_session_record_t *rec;

    if (ctx == NULL || ctx->found != NULL || session == NULL ||
        session == ctx->exclude) {
        return;
    }

    if (!mailboxd_session_logged_in(session) || mailboxd_session_is_guest(session)) {
        return;
    }

    rec = mailboxd_session_record(session);
    if (rec == NULL || rec->user_id != ctx->user_id) {
        return;
    }

    ctx->found = session;
}

mailboxd_session_t *mailboxd_service_find_registered_session(
    mailboxd_service_t *service,
    uint64_t user_id,
    mailboxd_session_t *exclude)
{
    find_registered_session_ctx_t ctx;

    if (service == NULL || user_id == 0) {
        return NULL;
    }

    ctx.user_id = user_id;
    ctx.exclude = exclude;
    ctx.found = NULL;
    mailboxd_service_visit_sessions(service, find_registered_session_visitor, &ctx);
    return ctx.found;
}

static void service_apply_service(struct mailboxd_service_internal *svc,
                                  const mailboxd_config_t *config)
{
    const char *prompt;
    const char *max_online_value;
    const char *nodes_value;

    svc->prompt[0] = '\0';

    prompt = mailboxd_config_get(config, "service", "prompt", NULL);
    if (prompt != NULL && prompt[0] != '\0') {
        mailboxd_strlcpy(svc->prompt, prompt, sizeof(svc->prompt));
    }

    max_online_value = mailboxd_config_get(config, "service", "max_online", NULL);
    nodes_value = mailboxd_config_get(config, "service", "nodes", NULL);
    if (max_online_value != NULL && max_online_value[0] != '\0') {
        svc->max_online = mailboxd_config_get_uint(config, "service", "max_online",
                                                MAILBOXD_DEFAULT_MAX_ONLINE, 1u,
                                                999u);
    } else if (nodes_value != NULL && nodes_value[0] != '\0') {
        svc->max_online = mailboxd_config_get_uint(config, "service", "nodes",
                                                MAILBOXD_DEFAULT_MAX_ONLINE, 1u,
                                                999u);
    } else {
        svc->max_online = MAILBOXD_DEFAULT_MAX_ONLINE;
    }

    svc->login_announce =
        mailboxd_config_get_bool(config, "service", "login_announce", 0);

    mailboxd_log_info("[service] max_online=%u login_announce=%s",
                   svc->max_online,
                   svc->login_announce ? "yes" : "no");
}

static int str_ieq_local(const char *a, const char *b)
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

static void service_apply_texts(struct mailboxd_service_internal *svc,
                                const mailboxd_config_t *config)
{
    const char *path;
    char resolved[MAILBOXD_PATH_MAX];

    mailboxd_texts_config_defaults(&svc->texts);
    path = mailboxd_config_get(config, "texts", "path", MAILBOXD_DIR_TEXT);
    if (mailboxd_path_resolve(resolved, sizeof(resolved), path) == MAILBOXD_OK) {
        mailboxd_strlcpy(svc->texts.path, resolved, sizeof(svc->texts.path));
    } else if (path != NULL && path[0] != '\0') {
        mailboxd_strlcpy(svc->texts.path, path, sizeof(svc->texts.path));
    }
}

static mailboxd_storage_backend_kind_t parse_storage_backend(const char *value)
{
    if (value == NULL || str_ieq_local(value, "flatfile") ||
        str_ieq_local(value, "flat") || str_ieq_local(value, "files")) {
        return MAILBOXD_STORAGE_FLATFILE;
    }

    if (str_ieq_local(value, "sqlite")) {
        return MAILBOXD_STORAGE_SQLITE;
    }

    if (str_ieq_local(value, "mysql")) {
        return MAILBOXD_STORAGE_MYSQL;
    }

    if (str_ieq_local(value, "mariadb")) {
        return MAILBOXD_STORAGE_MARIADB;
    }

    return MAILBOXD_STORAGE_FLATFILE;
}

static mailboxd_result_t service_open_storage(struct mailboxd_service_internal *svc,
                                           const mailboxd_config_t *config)
{
    mailboxd_storage_options_t options;
    mailboxd_storage_sql_config_t sql_cfg;
    const char *backend_str;
    const char *path_raw;
    char path_resolved[MAILBOXD_PATH_MAX];
    const char *guest_prefix;
    mailboxd_result_t rc;

    if (svc->storage != NULL) {
        mailboxd_storage_close(svc->storage);
        svc->storage = NULL;
    }

    backend_str = mailboxd_config_get(config, "storage", "backend", "flatfile");
    path_raw = mailboxd_config_get(config, "storage", "path", MAILBOXD_DEFAULT_DATA_PATH);

    rc = mailboxd_path_resolve(path_resolved, sizeof(path_resolved), path_raw);
    if (rc != MAILBOXD_OK) {
        mailboxd_log_warn("[storage] invalid path '%s'",
                path_raw != NULL ? path_raw : "");
        return MAILBOXD_ERR_IO;
    }

    guest_prefix = mailboxd_config_get(config, "auth", "guest_prefix", NULL);

    options.backend = parse_storage_backend(backend_str);
    options.path = path_resolved;
    options.guest_prefix = guest_prefix != NULL ? guest_prefix :
                                                  svc->auth.guest_prefix;
    options.sql_cfg = NULL;

    if (options.backend == MAILBOXD_STORAGE_SQLITE) {
        mailboxd_storage_sql_config_apply(&sql_cfg, config, path_resolved);
        options.sql_cfg = &sql_cfg;
    }

    svc->storage = mailboxd_storage_open(&options);
    if (svc->storage == NULL) {
        mailboxd_log_warn("[storage] cannot open '%s' (writable?)",
                path_resolved);
        return MAILBOXD_ERR_IO;
    }

    mailboxd_log_info("[storage] backend=%s path=%s", backend_str, path_resolved);
    return MAILBOXD_OK;
}

static void service_apply_auth(struct mailboxd_service_internal *svc,
                               const mailboxd_config_t *config)
{
    const char *prefix;

    mailboxd_auth_config_defaults(&svc->auth);
    svc->auth.auto_login =
        mailboxd_config_get_bool(config, "auth", "auto_login", 1);

    prefix = mailboxd_config_get(config, "auth", "guest_prefix", NULL);
    if (prefix != NULL && prefix[0] != '\0') {
        mailboxd_strlcpy(svc->auth.guest_prefix, prefix,
                      sizeof(svc->auth.guest_prefix));
    }

    svc->guest_timeout_minutes = mailboxd_config_get_uint(
        config, "auth", "guest_timeout_minutes",
        MAILBOXD_DEFAULT_GUEST_TIMEOUT_MINUTES, 1u, 24u * 60u);

    mailboxd_log_info("[service] guest_timeout_minutes=%u", svc->guest_timeout_minutes);
}

mailboxd_result_t mailboxd_service_load_transport(mailboxd_service_t *service,
                                            const char *plugin_name)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;
    const mailboxd_transport_plugin_t *plugin;
    size_t i;

    if (svc == NULL || plugin_name == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    plugin = mailboxd_registry_find(plugin_name);
    if (plugin == NULL) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    for (i = 0; i < svc->transport_count; i++) {
        if (svc->transports[i].plugin == plugin) {
            return MAILBOXD_OK;
        }
    }

    if (svc->transport_count >= MAILBOXD_MAX_ACTIVE_TRANSPORTS) {
        return MAILBOXD_ERR_NOMEM;
    }

    if (plugin->init != NULL) {
        mailboxd_result_t rc = plugin->init(service);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    svc->transports[svc->transport_count].plugin = plugin;
    svc->transports[svc->transport_count].running = 0;
    svc->transport_count++;
    return MAILBOXD_OK;
}

static active_transport_t *find_active(struct mailboxd_service_internal *svc,
                                       const char *plugin_name)
{
    size_t i;

    for (i = 0; i < svc->transport_count; i++) {
        if (strcmp(svc->transports[i].plugin->name, plugin_name) == 0) {
            return &svc->transports[i];
        }
    }
    return NULL;
}

mailboxd_result_t mailboxd_service_start_transport(mailboxd_service_t *service,
                                               const char *plugin_name,
                                               const char *config)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;
    active_transport_t *active;

    if (svc == NULL || plugin_name == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    active = find_active(svc, plugin_name);
    if (active == NULL) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    if (active->running) {
        return MAILBOXD_ERR_BUSY;
    }

    /* Feature plugins (e.g. entertain) may have init/tick/on_command only. */
    if (active->plugin->start != NULL) {
        mailboxd_result_t rc = active->plugin->start(config);
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    active->running = 1;
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_service_stop_transport(mailboxd_service_t *service,
                                              const char *plugin_name)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;
    active_transport_t *active;

    if (svc == NULL || plugin_name == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    active = find_active(svc, plugin_name);
    if (active == NULL) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    if (!active->running) {
        return MAILBOXD_OK;
    }

    if (active->plugin->stop != NULL) {
        mailboxd_result_t rc = active->plugin->stop();
        if (rc != MAILBOXD_OK) {
            return rc;
        }
    }

    active->running = 0;
    return MAILBOXD_OK;
}

static volatile sig_atomic_t g_stop_requested;

void mailboxd_service_request_stop_async(void)
{
    g_stop_requested = 1;
}

mailboxd_result_t mailboxd_service_run(mailboxd_service_t *service)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    svc->running = 1;
    unsigned health_tick = 0;

    while (svc->running && !g_stop_requested) {
#if defined(_WIN32)
        Sleep(1000);
#else
        sleep(1);

        /* Periodic health log — every 300s (5 min) */
        health_tick++;
        if (health_tick >= 300u) {
            health_tick = 0;
            mailboxd_log_info("[service] health: running=%d transports=%zu sessions_active (tick)",
                           svc->running, svc->transport_count);
        }

        mailboxd_security_ban_tick();
        mailboxd_monitor_tick(service);
        /* Plugin ticks — iterate all registered plugins */
        {
            size_t pi;
            for (pi = 0; pi < svc->transport_count; pi++) {
                const mailboxd_transport_plugin_t *p = svc->transports[pi].plugin;
                if (p != NULL && p->tick != NULL) {
                    p->tick(service);
                }
            }
        }
        if (svc->storage != NULL) {
            mailboxd_storage_backup_tick(svc->storage);
        }
#endif
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_service_try_plugin_command(mailboxd_service_t *service,
                                                struct mailboxd_session *session,
                                                const struct mailboxd_parsed_command *cmd)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;
    size_t i;

    if (svc == NULL || cmd == NULL) {
        return MAILBOXD_ERR_NOT_FOUND;
    }

    for (i = 0; i < svc->transport_count; i++) {
        const mailboxd_transport_plugin_t *p = svc->transports[i].plugin;
        if (p != NULL && p->on_command != NULL) {
            mailboxd_result_t rc = p->on_command(service, session, cmd);
            /* NOT_FOUND = not this plugin; any other result = handled. */
            if (rc != MAILBOXD_ERR_NOT_FOUND) {
                return rc;
            }
        }
    }

    return MAILBOXD_ERR_NOT_FOUND;
}

void mailboxd_service_stop(mailboxd_service_t *service)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc != NULL) {
        svc->running = 0;
    }
}

void mailboxd_service_set_launch_binary(mailboxd_service_t *service, const char *argv0)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return;
    }

    svc->launch_binary[0] = '\0';
    if (argv0 != NULL && argv0[0] != '\0') {
        mailboxd_strlcpy(svc->launch_binary, argv0, sizeof(svc->launch_binary));
    }
}

void mailboxd_service_request_shutdown(mailboxd_service_t *service, int restart)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return;
    }

    svc->shutdown_mode = restart ? MAILBOXD_SHUTDOWN_RESTART : MAILBOXD_SHUTDOWN_STOP;
    mailboxd_service_stop(service);
}

mailboxd_shutdown_mode_t mailboxd_service_shutdown_mode(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;

    if (svc == NULL) {
        return MAILBOXD_SHUTDOWN_NONE;
    }

    return svc->shutdown_mode;
}

const char *mailboxd_service_config_path(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;

    if (svc == NULL || svc->config_path[0] == '\0') {
        return NULL;
    }

    return svc->config_path;
}

void mailboxd_service_restart_exec(const mailboxd_service_t *service)
{
    const struct mailboxd_service_internal *svc =
        (const struct mailboxd_service_internal *)service;
    const char *binary;
    const char *config_path;
    char *argv[4];
    char opt_c[] = "-c";

    if (svc == NULL) {
        return;
    }

    binary = svc->launch_binary[0] != '\0' ? svc->launch_binary : MAILBOXD_DAEMON_BINARY;
    config_path = svc->config_path[0] != '\0' ? svc->config_path : NULL;

    argv[0] = (char *)binary;
    if (config_path != NULL) {
        argv[1] = opt_c;
        argv[2] = (char *)config_path;
        argv[3] = NULL;
    } else {
        argv[1] = NULL;
    }

    fflush(NULL);
    execv(binary, argv);
    perror("mailboxd restart failed");
}

typedef struct apply_transport_ctx {
    mailboxd_service_t *service;
    const mailboxd_config_t *config;
    const mailboxd_networks_config_t *networks;
    mailboxd_result_t last_error;
} apply_transport_ctx_t;

static int transport_start_failure_is_fatal(const char *plugin_name)
{
    /* Role static transport must start; others may fail independently. */
    return mailboxd_networks_is_static_transport(plugin_name);
}

static void apply_transport_cb(const mailboxd_transport_plugin_t *plugin,
                               void *userdata)
{
    apply_transport_ctx_t *ctx = (apply_transport_ctx_t *)userdata;
    char section[128];
    char *transport_config;
    mailboxd_result_t rc;
    int transport_enabled;

    if (ctx->networks == NULL ||
        !mailboxd_networks_transport_wanted(plugin->name, ctx->networks)) {
        return;
    }

    if (!mailboxd_config_resolve_transport_section(ctx->config, plugin->name,
                                                section, sizeof(section))) {
        snprintf(section, sizeof(section), "transport.%s", plugin->name);
    }

    if (mailboxd_networks_is_static_transport(plugin->name)) {
        transport_enabled = 1;
    } else {
        transport_enabled = mailboxd_config_get_bool(ctx->config, section,
                                                  "enabled", 1);
    }

    if (!transport_enabled) {
        return;
    }

    rc = mailboxd_service_load_transport(ctx->service, plugin->name);
    if (rc != MAILBOXD_OK) {
        mailboxd_log_warn("[service] %s: plugin load failed (%s)",
                plugin->name, mailboxd_result_name(rc));
        if (transport_start_failure_is_fatal(plugin->name)) {
            ctx->last_error = rc;
        }
        return;
    }

    transport_config = mailboxd_config_format_section(ctx->config, section);
    rc = mailboxd_service_start_transport(ctx->service, plugin->name,
                                       transport_config);
    free(transport_config);

    if (rc != MAILBOXD_OK) {
        mailboxd_log_warn("[service] %s: start failed (%s)", plugin->name,
                mailboxd_result_name(rc));
        if (transport_start_failure_is_fatal(plugin->name)) {
            ctx->last_error = rc;
        }
    }
}

mailboxd_result_t mailboxd_service_apply_config(mailboxd_service_t *service,
                                            const mailboxd_config_t *config,
                                            const char *config_path)
{
    struct mailboxd_service_internal *svc =
        (struct mailboxd_service_internal *)service;
    const char *service_name;
    apply_transport_ctx_t ctx;
    char *new_name;
    mailboxd_result_t rc;

    if (svc == NULL || config == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    svc->config_path[0] = '\0';
    if (config_path != NULL && config_path[0] != '\0') {
        mailboxd_strlcpy(svc->config_path, config_path, sizeof(svc->config_path));
    }

    service_name = mailboxd_config_get(config, "service", "name", NULL);
    if (service_name != NULL) {
        new_name = mailboxd_strdup(service_name);
        if (new_name == NULL) {
            return MAILBOXD_ERR_NOMEM;
        }
        free(svc->name);
        svc->name = new_name;
    }

    service_apply_auth(svc, config);
    mailboxd_time_config_apply(config);
    mailboxd_log_config_apply(config);
    mailboxd_security_log_config_apply(config);
    mailboxd_monitor_config_apply(config);
    mailboxd_security_ban_config_apply(config);
    service_apply_texts(svc, config);
    mailboxd_chat_config_apply(&svc->chat, config);
    mailboxd_broadcast_config_apply(&svc->broadcast, config);
    service_apply_service(svc, config);
    mailboxd_crypto_config_apply(config);
    mailboxd_traffic_config_apply(config);

    rc = service_open_storage(svc, config);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    {
        const char *storage_path = MAILBOXD_DEFAULT_DATA_PATH;
        char storage_resolved[MAILBOXD_PATH_MAX];

        if (svc->storage != NULL && svc->storage->path != NULL &&
            svc->storage->path[0] != '\0') {
            storage_path = svc->storage->path;
        } else if (mailboxd_path_resolve(storage_resolved, sizeof(storage_resolved),
                                      storage_path) == MAILBOXD_OK) {
            storage_path = storage_resolved;
        }
        mailboxd_mail_config_apply(&svc->mail, config, storage_path);
    }

    mailboxd_networks_config_apply(&svc->networks, config);

    {
        int standalone = mailboxd_config_get_bool(config, "instance", "standalone", 0);

        mailboxd_instance_set_standalone(standalone);
    }

    rc = mailboxd_networks_enforce_instance(&svc->networks);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    mailboxd_entertain_config_apply(config);

    ctx.service = service;
    ctx.config = config;
    svc->config = config;
    ctx.networks = &svc->networks;
    ctx.last_error = MAILBOXD_OK;

    mailboxd_registry_foreach(apply_transport_cb, &ctx);
    return ctx.last_error;
}
