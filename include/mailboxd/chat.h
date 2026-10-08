/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_CHAT_H
#define MAILBOXD_CHAT_H

#include "mailboxd/config.h"
#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_service;
struct mailboxd_session;

/** Hard limit on chat channels (also the default configured count). */
#define MAILBOXD_CHAT_CHANNEL_MAX 10u

#define MAILBOXD_CHAT_CHANNEL_NAME_MAX 32

#define MAILBOXD_DEFAULT_CHAT_CHANNELS MAILBOXD_CHAT_CHANNEL_MAX

/** Maximum characters per chat message line (fits 80-column wrap). */
#define MAILBOXD_CHAT_MESSAGE_MAX 72u

typedef struct mailboxd_chat_config {
    /** Active channels (1 … @ref MAILBOXD_CHAT_CHANNEL_MAX). */
    unsigned channel_count;
    /** Maximum characters per chat message line. */
    unsigned message_max;
    /**
     * Channel names (index 0 = channel 1). Default: Channel1 … Channel10.
     * Override with [chat] channel1 … channel10 in INI.
     */
    char names[MAILBOXD_CHAT_CHANNEL_MAX][MAILBOXD_CHAT_CHANNEL_NAME_MAX];
} mailboxd_chat_config_t;

void mailboxd_chat_config_defaults(mailboxd_chat_config_t *chat);

/** Load `[chat]` settings from an INI file. */
void mailboxd_chat_config_apply(mailboxd_chat_config_t *chat,
                             const mailboxd_config_t *config);

const mailboxd_chat_config_t *mailboxd_service_get_chat(const struct mailboxd_service *service);

/** Return channel name for @p channel_index (1-based), or NULL when invalid. */
const char *mailboxd_chat_channel_name(const mailboxd_chat_config_t *chat,
                                    unsigned channel_index);

/**
 * Resolve @p spec as channel number (1-based) or name (case-insensitive).
 * @p out_index receives 1-based channel index on success.
 */
mailboxd_result_t mailboxd_chat_resolve_channel(const mailboxd_chat_config_t *chat,
                                          const char *spec,
                                          unsigned *out_index);

/** List configured channels on @p session. */
void mailboxd_chat_list_channels(struct mailboxd_session *session,
                              const mailboxd_chat_config_t *chat);

/** Broadcast a chat line within the sender's channel.
 * Sender sees "ME: …"; others see "<username>: …".
 */
mailboxd_result_t mailboxd_chat_post(struct mailboxd_service *service,
                               struct mailboxd_session *from,
                               const char *message);

/** List display names in the requester's current chat channel only. */
void mailboxd_chat_show_channel(struct mailboxd_service *service,
                             struct mailboxd_session *session);

/**
 * List all users in public chat channels as nick@ChannelN, sorted by channel,
 * wrapped at 80 columns (conference sessions excluded).
 */
void mailboxd_chat_show_all(struct mailboxd_service *service,
                         struct mailboxd_session *session);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_CHAT_H */
