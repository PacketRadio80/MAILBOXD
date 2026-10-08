/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_CLIENT_DISPLAY_H
#define MAILBOXD_CLIENT_DISPLAY_H

#include "mailboxd/traffic.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mailboxd_client_display {
    const mailboxd_traffic_config_t *traffic;
    unsigned col;
} mailboxd_client_display_t;

void mailboxd_client_display_init(mailboxd_client_display_t *disp,
                               const mailboxd_traffic_config_t *traffic);

void mailboxd_client_display_byte(mailboxd_client_display_t *disp, uint8_t byte);

void mailboxd_client_display_write(mailboxd_client_display_t *disp,
                                const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_CLIENT_DISPLAY_H */
