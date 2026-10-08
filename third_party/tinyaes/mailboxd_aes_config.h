/* MailboxD tiny-AES-c configuration: AES-256, ECB + CTR for GCM.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MAILBOXD_AES_CONFIG_H
#define MAILBOXD_AES_CONFIG_H

#define CBC 0
#define ECB 1
#define CTR 1
#undef AES128
#define AES128 0
#undef AES256
#define AES256 1

#endif
