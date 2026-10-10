/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_BEACON_H
#define MAILBOXD_BEACON_H

/**
 * Autonomous CQ beacon daemon for MailboxD.
 *
 * When configured with a callsign and PRTERM URL, this daemon periodically
 * sends CQ AX.25 broadcasts via PRTERM's mbox_cqbeacon action, cycling
 * through all configured devices.  The 150-second band-free gate is
 * enforced by PRTERM's TNC state.
 *
 * INI: [beacon]
 *   enabled    = yes
 *   callerid   = MGHBX1          ; AX.25 callsign for MailboxD
 *   interval   = 150             ; seconds between beacon cycles
 *   prterm_url = http://127.0.0.1/prterm.cgi
 */

#include "mailboxd/types.h"

struct mailboxd_config;

/** Start the beacon daemon thread (if [beacon] enabled=yes). */
mailboxd_result_t mailboxd_beacon_start(const struct mailboxd_config *config);

/** Stop the beacon daemon thread. */
void mailboxd_beacon_stop(void);

#endif /* MAILBOXD_BEACON_H */