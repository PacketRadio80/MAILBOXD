/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_PLUGIN_H
#define MAILBOXD_PLUGIN_H

#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_service;
struct mailboxd_session;
struct mailboxd_parsed_command;

/**
 * Transport plugin interface (link adapters / host-client bridges).
 *
 * MailboxD is plugin-only: session core + plugins. MailboxD has no radio
 * side at all — no TNC, modem or serial code; RF is PRTERM's business.
 *
 * Core uses TCP/IPv4+IPv6 and HBX internally. Plugins terminate the wire
 * to external clients (telnet TCP, WebSocket, …) and expose a byte stream
 * to mailboxd_session via the write callback.
 *
 * Optional extensions (NULL = not used):
 *   tick        — called every ~1s from service loop (game clocks, AI, …)
 *   on_command  — intercept "/verb" before core dispatch (entertainment, …)
 */
typedef struct mailboxd_transport_plugin {
    const char *name;
    mailboxd_transport_kind_t kind;
    unsigned int version;

    /** Called once when the plugin is loaded. */
    mailboxd_result_t (*init)(struct mailboxd_service *service);

    /** Called once when the plugin is unloaded. */
    void (*shutdown)(void);

    /**
     * Start listening / accepting connections on the given bind address.
     * @p config is a semicolon-separated key=value string built from the
     * transport's INI section (e.g. "bind=0.0.0.0;port=2323").
     */
    mailboxd_result_t (*start)(const char *config);

    /** Stop accepting new connections; existing sessions may continue. */
    mailboxd_result_t (*stop)(void);

    /**
     * Send raw bytes to the connected client for this session.
     * NULL → core writes to stdout (development fallback).
     */
    mailboxd_result_t (*write)(struct mailboxd_session *session,
                            const char *data, size_t len);

    /**
     * Periodic tick — called every ~1 second from the service loop.
     * NULL → no periodic work. Used by game clocks, AI timers, etc.
     */
    void (*tick)(struct mailboxd_service *service);

    /**
     * Command intercept — called before core command dispatch.
     * Return MAILBOXD_OK if handled, MAILBOXD_ERR_NOT_FOUND to fall through.
     * NULL → no command handling.
     */
    mailboxd_result_t (*on_command)(struct mailboxd_service *service,
                                 struct mailboxd_session *session,
                                 const struct mailboxd_parsed_command *cmd);

} mailboxd_transport_plugin_t;

/**
 * Session callbacks invoked by a transport plugin when a client connects.
 */
typedef struct mailboxd_session_ops {
    mailboxd_result_t (*on_connect)(struct mailboxd_session *session, void *userdata);
    mailboxd_result_t (*on_data)(struct mailboxd_session *session,
                              const uint8_t *data, size_t len,
                              void *userdata);
    void (*on_disconnect)(struct mailboxd_session *session, void *userdata);
} mailboxd_session_ops_t;

/** Opaque session handle passed between core and transport plugins. */
typedef struct mailboxd_session {
    const mailboxd_transport_plugin_t *transport;
    void *transport_data;
    void *core_data;
} mailboxd_session_t;

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_PLUGIN_H */
