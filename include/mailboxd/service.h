/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_SERVICE_H
#define MAILBOXD_SERVICE_H

/**
 * Centralized MailboxD daemon API (`mailboxd` binary).
 *
 * Loads INI ([service], [storage], [auth], [transport.*]) and starts
 * transport plugins (telnet, mailboxd_prterm). See
 * share/mailboxd.ini.example and docs/TOPOLOGY.md.
 */

#include "mailboxd/config.h"
#include "mailboxd/plugin.h"
#include "mailboxd/auth.h"
#include "mailboxd/storage.h"
#include "mailboxd/texts.h"

struct mailboxd_parsed_command;

#ifdef __cplusplus
extern "C" {
#endif

/** Default global service name (`[service] name` in INI). */
#define MAILBOXD_DEFAULT_SERVICE_NAME "MailboxD"

typedef struct mailboxd_service {
    const char *name;
    void *userdata;
} mailboxd_service_t;

/** Create the main MailboxD service instance. */
mailboxd_service_t *mailboxd_service_create(const char *name);

/** Destroy the service and shut down all active transports. */
void mailboxd_service_destroy(mailboxd_service_t *service);

/**
 * Apply settings from an INI configuration (service name, enabled transports).
 */
mailboxd_result_t mailboxd_service_apply_config(mailboxd_service_t *service,
                                          const mailboxd_config_t *config,
                                          const char *config_path);

/**
 * Load a transport plugin by name (e.g. "telnet", "websocket").
 * Built-in plugins are linked statically; dynamic loading comes later.
 */
mailboxd_result_t mailboxd_service_load_transport(mailboxd_service_t *service,
                                            const char *plugin_name);

/** Start a loaded transport with transport-specific configuration. */
mailboxd_result_t mailboxd_service_start_transport(mailboxd_service_t *service,
                                               const char *plugin_name,
                                               const char *config);

/** Stop a running transport. */
mailboxd_result_t mailboxd_service_stop_transport(mailboxd_service_t *service,
                                            const char *plugin_name);

/** Run the main event loop until mailboxd_service_stop() is called. */
mailboxd_result_t mailboxd_service_run(mailboxd_service_t *service);

/** Request the main loop to exit. */
void mailboxd_service_stop(mailboxd_service_t *service);

/**
 * Async-signal-safe: request mailboxd_service_run()'s loop to exit on
 * its next iteration. Call this (and only this) from a signal handler
 * (e.g. SIGTERM/SIGINT) - it just sets a sig_atomic_t flag.
 */
void mailboxd_service_request_stop_async(void);

typedef enum mailboxd_shutdown_mode {
    MAILBOXD_SHUTDOWN_NONE = 0,
    MAILBOXD_SHUTDOWN_STOP = 1,
    MAILBOXD_SHUTDOWN_RESTART = 2
} mailboxd_shutdown_mode_t;

/** Store the daemon binary path used for /restart (typically argv[0]). */
void mailboxd_service_set_launch_binary(mailboxd_service_t *service, const char *argv0);

/** Sysop /shutdown or /restart — stops the main loop; restart re-execs after exit. */
void mailboxd_service_request_shutdown(mailboxd_service_t *service, int restart);

/** Shutdown mode requested while the service is still running. */
mailboxd_shutdown_mode_t mailboxd_service_shutdown_mode(const mailboxd_service_t *service);

/**
 * Re-exec the daemon when @ref mailboxd_service_shutdown_mode is
 * @ref MAILBOXD_SHUTDOWN_RESTART. Call before @ref mailboxd_service_destroy.
 * Does not return on success.
 */
void mailboxd_service_restart_exec(const mailboxd_service_t *service);

const char *mailboxd_service_config_path(const mailboxd_service_t *service);

mailboxd_storage_t *mailboxd_service_get_storage(mailboxd_service_t *service);
const struct mailboxd_config *mailboxd_service_get_config(mailboxd_service_t *service);
const mailboxd_auth_config_t *mailboxd_service_get_auth(mailboxd_service_t *service);
const mailboxd_texts_config_t *mailboxd_service_get_texts(mailboxd_service_t *service);

/**
 * Global input prompt shown to every connected user.
 * Empty string when unset (default): no visible prompt before input.
 */
const char *mailboxd_service_get_prompt(const mailboxd_service_t *service);

/** Global service name from configuration (default @ref MAILBOXD_DEFAULT_SERVICE_NAME). */
const char *mailboxd_service_get_name(const mailboxd_service_t *service);

/** Configured maximum simultaneous online sessions (default @ref MAILBOXD_DEFAULT_MAX_ONLINE). */
unsigned mailboxd_service_max_online(const mailboxd_service_t *service);

/**
 * Non-zero when `[service] login_announce=yes`:
 * broadcast "*** User login: user@plugin" and list /who as user@plugin.
 */
int mailboxd_service_login_announce(const mailboxd_service_t *service);

/** Currently connected sessions (all transports). */
unsigned mailboxd_service_active_nodes(const mailboxd_service_t *service);

/** Guest auto-disconnect timeout in seconds (from INI minutes setting). */
unsigned mailboxd_service_guest_timeout_seconds(const mailboxd_service_t *service);

struct mailboxd_user_record;

/**
 * Assign the lowest free ephemeral guest slot (Guest1 … Guest25).
 * Guests are not written to user files.
 */
mailboxd_result_t mailboxd_service_guest_assign(mailboxd_service_t *service,
                                          const char *guest_prefix,
                                          struct mailboxd_user_record *out,
                                          unsigned *slot_out);

/** Release a guest slot when the session ends or leaves guest mode. */
void mailboxd_service_guest_release(mailboxd_service_t *service, unsigned slot);

/**
 * Reserve one node slot for a new connection.
 * @return MAILBOXD_ERR_BUSY when @ref mailboxd_service_max_online is reached.
 */
mailboxd_result_t mailboxd_service_acquire_node(mailboxd_service_t *service);

/** Release a node slot (pair with @ref mailboxd_service_acquire_node). */
void mailboxd_service_release_node(mailboxd_service_t *service);

struct mailboxd_session;

/** Track an active connection for chat broadcast and similar features. */
mailboxd_result_t mailboxd_service_attach_session(mailboxd_service_t *service,
                                            struct mailboxd_session *session);

/** Remove a session from the active connection list. */
void mailboxd_service_detach_session(mailboxd_service_t *service,
                                  struct mailboxd_session *session);

typedef void (*mailboxd_service_session_visit_fn)(struct mailboxd_session *session,
                                               void *userdata);

/** Invoke @p fn for each attached session (snapshot; lock not held in @p fn). */
void mailboxd_service_visit_sessions(mailboxd_service_t *service,
                                  mailboxd_service_session_visit_fn fn,
                                  void *userdata);

/**
 * Return a logged-in registered session for @p user_id on another connection,
 * or NULL when the account is not online elsewhere.
 */
struct mailboxd_session *mailboxd_service_find_registered_session(
    mailboxd_service_t *service,
    uint64_t user_id,
    struct mailboxd_session *exclude);

/**
 * Try plugin on_command interceptors.
 * Returns MAILBOXD_OK if a plugin handled the command, MAILBOXD_ERR_NOT_FOUND otherwise.
 */
mailboxd_result_t mailboxd_service_try_plugin_command(mailboxd_service_t *service,
                                                struct mailboxd_session *session,
                                                const struct mailboxd_parsed_command *cmd);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_SERVICE_H */
