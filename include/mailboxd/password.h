/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_PASSWORD_H
#define MAILBOXD_PASSWORD_H

#include "mailboxd/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Stored password prefix for SHA-256 (default, tinysha256). */
#define MAILBOXD_PASSWORD_SHA256_PREFIX "{sha256}"
/** Stored password prefix for legacy MD5 (verify-only fallback). */
#define MAILBOXD_PASSWORD_MD5_PREFIX "{md5}"

/** Max stored password field: prefix + 64 hex digits + NUL. */
#define MAILBOXD_PASSWORD_STORED_MAX 96

/** Non-zero when @p stored is a hashed password (sha256 or md5 prefix). */
int mailboxd_password_is_hashed(const char *stored);

/** Non-zero when @p stored is plain text and should be upgraded on load. */
int mailboxd_password_is_plain(const char *stored);

/**
 * Hash @p plain into @p out as {sha256}<hex> using the configured backend
 * (default: bundled tinysha256; see [crypto] password_hash in mailboxd.ini).
 */
mailboxd_result_t mailboxd_password_hash_sha256(const char *plain,
                                          char *out,
                                          size_t out_size);

/** Hash @p plain with the configured default algorithm (SHA-256). */
mailboxd_result_t mailboxd_password_hash(const char *plain, char *out, size_t out_size);

/** Return non-zero when @p provided matches @p stored (plain, sha256, or md5). */
int mailboxd_password_match(const char *stored, const char *provided);

/**
 * Fill @p out with a random password of length @p min_len … @p max_len using
 * characters a-z and 0-9 only.
 */
mailboxd_result_t mailboxd_password_generate_alnum(char *out,
                                             size_t out_size,
                                             size_t min_len,
                                             size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_PASSWORD_H */
