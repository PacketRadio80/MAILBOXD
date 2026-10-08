/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#if defined(__linux__) || defined(__GLIBC__)
#define _DEFAULT_SOURCE 1
#endif

#include "client_display.h"

#include "mailboxd/terminal.h"
#include "mailboxd/traffic.h"

#include <stdio.h>
#include <unistd.h>

void mailboxd_client_display_init(mailboxd_client_display_t *disp,
                               const mailboxd_traffic_config_t *traffic)
{
    if (disp == NULL) {
        return;
    }

    disp->traffic = traffic != NULL ? traffic : mailboxd_traffic_config_get();
    disp->col = 0;
}

static void pace_byte(unsigned baud)
{
    unsigned delay_us;

    if (baud == 0) {
        return;
    }

    delay_us = mailboxd_traffic_byte_delay_us(baud);
    if (delay_us > 0) {
        usleep(delay_us);
    }
}

void mailboxd_client_display_byte(mailboxd_client_display_t *disp, uint8_t byte)
{
    const mailboxd_traffic_config_t *cfg;

    if (disp == NULL) {
        return;
    }

    cfg = disp->traffic;
    if (cfg == NULL) {
        fputc((int)byte, stdout);
        fflush(stdout);
        return;
    }

    if (!cfg->ansi && byte == 0x1b) {
        return;
    }

    if (cfg->line_width > 0 && (byte == '\n' || byte == '\r')) {
        disp->col = 0;
    } else if (cfg->line_width > 0 && byte >= 0x20 && byte != 0x7f) {
        disp->col++;
        if (disp->col > cfg->line_width) {
            fputc('\n', stdout);
            disp->col = 1;
        }
    }

    fputc((int)byte, stdout);
    fflush(stdout);

    if (cfg->pace_output) {
        pace_byte(cfg->baud);
    }
}

void mailboxd_client_display_write(mailboxd_client_display_t *disp,
                                const uint8_t *data, size_t len)
{
    size_t i;

    if (disp == NULL || data == NULL) {
        return;
    }

    for (i = 0; i < len; i++) {
        mailboxd_client_display_byte(disp, data[i]);
    }
}
