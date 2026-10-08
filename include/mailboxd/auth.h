/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_AUTH_H
#define MAILBOXD_AUTH_H

#include "mailboxd/password.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAILBOXD_AUTH_GUEST_PREFIX_MAX 32

/** Default guest name prefix: Guest1, Guest2, … */
#define MAILBOXD_AUTH_DEFAULT_GUEST_PREFIX "Guest"

/** Highest guest number issued (Guest1 … Guest25); max simultaneous guests. */
#define MAILBOXD_GUEST_NUMBER_MAX 25u

/**
 * Synthetic user IDs for ephemeral guest sessions (not stored in user files).
 * Slot @p n is 1 … @ref MAILBOXD_GUEST_NUMBER_MAX.
 */
#define MAILBOXD_GUEST_USER_ID(n) (0xffffffffffffff00ULL + (uint64_t)(n))

#define MAILBOXD_USERNAME_MIN_LEN 4u
#define MAILBOXD_USERNAME_MAX_LEN 12u
#define MAILBOXD_USERNAME_MAX_DIGITS 4u

/**
 * Default Sysop account (auto-created when no Sysop exists in the user
 * database). Fixed initial password, matching PRTERM's built-in admin
 * default (`PR_DEFAULT_ADMIN_PASS` in PRTERM's src/session.c) — change
 * it on first login via /passwd.
 */
#define MAILBOXD_DEFAULT_SYSOP_USERNAME "Sysop"
#define MAILBOXD_SYSOP_INIT_PASSWORD "PRTerm"

/** Plain-text password length policy for new passwords (/changeme, future set-password). */
#define MAILBOXD_PASSWORD_MIN_LEN 8u
#define MAILBOXD_PASSWORD_MAX_LEN 24u

/**
 * User privilege levels (highest to lowest).
 * Sysop: exactly one account on the system.
 */
typedef enum mailboxd_user_level {
    MAILBOXD_LEVEL_SYSOP = 1,
    MAILBOXD_LEVEL_ADMIN = 2,
    MAILBOXD_LEVEL_MOD = 3,
    MAILBOXD_LEVEL_USER = 4,
    MAILBOXD_LEVEL_GUEST = 5
} mailboxd_user_level_t;

typedef struct mailboxd_auth_config {
    int auto_login;
    char guest_prefix[MAILBOXD_AUTH_GUEST_PREFIX_MAX];
} mailboxd_auth_config_t;

void mailboxd_auth_config_defaults(mailboxd_auth_config_t *auth);

const char *mailboxd_user_level_name(mailboxd_user_level_t level);
mailboxd_user_level_t mailboxd_user_level_parse(const char *name);
int mailboxd_user_level_is_guest(mailboxd_user_level_t level);

/** Non-zero when @p level is Sysop or Admin. */
int mailboxd_user_level_is_sysop_or_admin(mailboxd_user_level_t level);

/** Non-zero when @p level is the protected Sysop account tier. */
int mailboxd_user_level_is_sysop(mailboxd_user_level_t level);

/** Non-zero when @p password meets the plain-text password policy (8–24 characters). */
int mailboxd_password_plain_valid(const char *password);

/**
 * Return non-zero when @p username is valid for registration:
 * 4–12 characters, letters (a–z), digits (max 4), and at most one `_` or one `-`
 * (not both); case-insensitive.
 */
int mailboxd_username_valid(const char *username, const char *guest_prefix);

/** Fold @p username to lowercase in place (for case-insensitive lookup). */
void mailboxd_username_normalize(char *username);

/** Presentation form for output (e.g. Sysop instead of stored sysop). */
const char *mailboxd_username_display(const char *username,
                                   mailboxd_user_level_t level);

struct mailboxd_user_record;

/** User-facing display name (nickname when set, else username). */
const char *mailboxd_user_display_name(const struct mailboxd_user_record *user);

/**
 * Derive a default nickname from a legacy stored username when none is set.
 */
void mailboxd_nickname_infer(const char *stored_username,
                          char *nickname,
                          size_t nickname_len);

/**
 * Parse @p username as @c <guest_prefix><1-25> (case-insensitive prefix).
 * Writes slot to @p slot_out. Returns 1 on match, else 0.
 */
int mailboxd_guest_slot_from_username(const char *guest_prefix,
                                   const char *username,
                                   unsigned *slot_out);

struct mailboxd_user_record;

/** Fill @p out with an ephemeral guest record for @p slot (1 … 25). */
void mailboxd_guest_fill_record(const char *guest_prefix,
                             unsigned slot,
                             struct mailboxd_user_record *out);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_AUTH_H */
