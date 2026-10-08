/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_TELNET_PROTO_H
#define MAILBOXD_TELNET_PROTO_H

#include "mailboxd/types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mailboxd_telnet_parser {
    int state;
    int command;
} mailboxd_telnet_parser_t;

typedef void (*mailboxd_telnet_data_cb)(void *ctx, const uint8_t *data, size_t len);

void mailboxd_telnet_parser_init(mailboxd_telnet_parser_t *parser);

/** Send initial option negotiation (echo, suppress go ahead). */
mailboxd_result_t mailboxd_telnet_send_greeting(int fd);

/**
 * Strip telnet protocol bytes and invoke @p on_data for user payload.
 */
mailboxd_result_t mailboxd_telnet_parser_feed(mailboxd_telnet_parser_t *parser, int fd,
                                        const uint8_t *data, size_t len,
                                        mailboxd_telnet_data_cb on_data,
                                        void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_TELNET_PROTO_H */
