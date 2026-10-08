/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "telnet_proto.h"

#include "mailboxd/socket.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#define TELNET_IAC   255
#define TELNET_DONT  254
#define TELNET_DO    253
#define TELNET_WONT  252
#define TELNET_WILL  251
#define TELNET_SB    250
#define TELNET_GA    249
#define TELNET_EL    248
#define TELNET_EC    247
#define TELNET_AYT   246
#define TELNET_AO    245
#define TELNET_IP    244
#define TELNET_BRK   243
#define TELNET_DM    242
#define TELNET_NOP   241
#define TELNET_SE    240

#define TELNET_OPT_ECHO     1
#define TELNET_OPT_SGA      3
#define TELNET_OPT_TTYPE    24
#define TELNET_OPT_NAWS     31
#define TELNET_OPT_LINEMODE 34
#define TELNET_OPT_CHARSET  42

enum telnet_parse_state {
    TS_DATA = 0,
    TS_IAC,
    TS_CMD,
    TS_SUBNEG,
    TS_SUBNEG_IAC
};

static mailboxd_result_t telnet_send_raw(int fd, const uint8_t *data, size_t len)
{
    size_t sent_total = 0;

    while (sent_total < len) {
        ssize_t sent;

        sent = send(fd, data + sent_total, len - sent_total, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return MAILBOXD_ERR_IO;
        }
        if (sent == 0) {
            return MAILBOXD_ERR_IO;
        }

        sent_total += (size_t)sent;
    }

    return MAILBOXD_OK;
}

static mailboxd_result_t telnet_send_option(int fd, uint8_t cmd, uint8_t option)
{
    uint8_t buf[3];

    buf[0] = TELNET_IAC;
    buf[1] = cmd;
    buf[2] = option;
    return telnet_send_raw(fd, buf, sizeof(buf));
}

static mailboxd_result_t telnet_reply_do(int fd, uint8_t option)
{
    switch (option) {
    case TELNET_OPT_ECHO:
        return telnet_send_option(fd, TELNET_WONT, option);
    case TELNET_OPT_SGA:
        return telnet_send_option(fd, TELNET_WILL, option);
    case TELNET_OPT_TTYPE:
    case TELNET_OPT_NAWS:
    case TELNET_OPT_LINEMODE:
    case TELNET_OPT_CHARSET:
        return telnet_send_option(fd, TELNET_WONT, option);
    default:
        return telnet_send_option(fd, TELNET_WONT, option);
    }
}

static mailboxd_result_t telnet_reply_will(int fd, uint8_t option)
{
    switch (option) {
    case TELNET_OPT_SGA:
        return telnet_send_option(fd, TELNET_DO, option);
    case TELNET_OPT_ECHO:
        return telnet_send_option(fd, TELNET_DONT, option);
    default:
        return telnet_send_option(fd, TELNET_DONT, option);
    }
}

static void emit_byte(mailboxd_telnet_data_cb on_data, void *ctx, uint8_t byte)
{
    if (on_data != NULL) {
        on_data(ctx, &byte, 1);
    }
}

void mailboxd_telnet_parser_init(mailboxd_telnet_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }

    parser->state = TS_DATA;
    parser->command = 0;
}

mailboxd_result_t mailboxd_telnet_send_greeting(int fd)
{
    static const uint8_t greeting[] = {
        TELNET_IAC, TELNET_WONT, TELNET_OPT_ECHO,
        TELNET_IAC, TELNET_WILL, TELNET_OPT_SGA,
        TELNET_IAC, TELNET_DONT, TELNET_OPT_LINEMODE,
    };

    if (fd < 0) {
        return MAILBOXD_ERR_INVALID;
    }

    return telnet_send_raw(fd, greeting, sizeof(greeting));
}

mailboxd_result_t mailboxd_telnet_parser_feed(mailboxd_telnet_parser_t *parser, int fd,
                                        const uint8_t *data, size_t len,
                                        mailboxd_telnet_data_cb on_data,
                                        void *ctx)
{
    size_t i;
    mailboxd_result_t rc;

    if (parser == NULL || data == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    for (i = 0; i < len; i++) {
        uint8_t byte = data[i];

        switch (parser->state) {
        case TS_DATA:
            if (byte == TELNET_IAC) {
                parser->state = TS_IAC;
            } else {
                emit_byte(on_data, ctx, byte);
            }
            break;

        case TS_IAC:
            if (byte == TELNET_IAC) {
                emit_byte(on_data, ctx, TELNET_IAC);
                parser->state = TS_DATA;
            } else if (byte == TELNET_WILL || byte == TELNET_WONT ||
                       byte == TELNET_DO || byte == TELNET_DONT) {
                parser->command = (int)byte;
                parser->state = TS_CMD;
            } else if (byte == TELNET_SB) {
                parser->state = TS_SUBNEG;
            } else {
                parser->state = TS_DATA;
            }
            break;

        case TS_CMD:
            if (fd >= 0) {
                if (parser->command == TELNET_DO) {
                    rc = telnet_reply_do(fd, byte);
                    if (rc != MAILBOXD_OK) {
                        return rc;
                    }
                } else if (parser->command == TELNET_WILL) {
                    rc = telnet_reply_will(fd, byte);
                    if (rc != MAILBOXD_OK) {
                        return rc;
                    }
                }
            }
            parser->state = TS_DATA;
            break;

        case TS_SUBNEG:
            if (byte == TELNET_IAC) {
                parser->state = TS_SUBNEG_IAC;
            }
            break;

        case TS_SUBNEG_IAC:
            if (byte == TELNET_SE) {
                parser->state = TS_DATA;
            } else {
                parser->state = TS_SUBNEG;
            }
            break;

        default:
            parser->state = TS_DATA;
            break;
        }
    }

    return MAILBOXD_OK;
}
