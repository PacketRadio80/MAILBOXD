/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_TRAFFIC_H
#define MAILBOXD_TRAFFIC_H

#include "mailboxd/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_config;
struct mailboxd_session;

/** Host link speed MailboxD is optimized for (8N1, ~1 byte per 10 bit times). */
#define MAILBOXD_BAUD2400 2400u

/** Default terminal width (columns). */
#define MAILBOXD_LINE_WIDTH 80u

/** Maximum configurable `line_width` in INI. */
#define MAILBOXD_LINE_WIDTH_MAX 132u

/** Maximum user input line (commands / chat). */
#define MAILBOXD_LINE_MAX 80u

typedef struct mailboxd_traffic_config {
    unsigned baud;
    unsigned line_width;
    int pace_output;
    int ansi;
    int input_echo;
} mailboxd_traffic_config_t;

void mailboxd_traffic_config_defaults(mailboxd_traffic_config_t *cfg);
void mailboxd_traffic_config_apply(const struct mailboxd_config *config);
const mailboxd_traffic_config_t *mailboxd_traffic_config_get(void);

/**
 * Microseconds to wait after one 8N1 byte at @p baud (0 when pacing is off).
 */
unsigned mailboxd_traffic_byte_delay_us(unsigned baud);

/**
 * Emit @p data to @p session with plain-ASCII policy, column wrap, and pacing.
 * @p out_col is updated (wrap position on the current output line).
 */
mailboxd_result_t mailboxd_traffic_emit(struct mailboxd_session *session,
                                  unsigned *out_col,
                                  const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_TRAFFIC_H */
