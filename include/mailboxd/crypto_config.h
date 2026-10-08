/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_CRYPTO_CONFIG_H
#define MAILBOXD_CRYPTO_CONFIG_H

#include "mailboxd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mailboxd_config;

/** Password hashing backend for new hashes and verification. */
typedef enum mailboxd_password_hash_backend {
    MAILBOXD_PASSWORD_HASH_TINYSHA256 = 1,
    MAILBOXD_PASSWORD_HASH_OPENSSL = 2
} mailboxd_password_hash_backend_t;

/** AES-256-GCM implementation backend. */
typedef enum mailboxd_aes_gcm_backend {
    MAILBOXD_AES_GCM_TINYAES = 1,
    MAILBOXD_AES_GCM_OPENSSL = 2
} mailboxd_aes_gcm_backend_t;

/** XChaCha20-Poly1305 implementation backend. */
typedef enum mailboxd_chacha_backend {
    MAILBOXD_CHACHA_MONOCYPHER = 1,
    MAILBOXD_CHACHA_LIBSODIUM = 2
} mailboxd_chacha_backend_t;

/** X25519 key agreement backend (sealed-box mode). */
typedef enum mailboxd_x25519_backend {
    MAILBOXD_X25519_MONOCYPHER = 1,
    MAILBOXD_X25519_LIBSODIUM = 2
} mailboxd_x25519_backend_t;

/** OS random bytes source. */
typedef enum mailboxd_random_backend {
    MAILBOXD_RANDOM_SYSTEM = 1,
    MAILBOXD_RANDOM_OPENSSL = 2
} mailboxd_random_backend_t;

typedef struct mailboxd_crypto_config {
    mailboxd_password_hash_backend_t password_hash;
    mailboxd_aes_gcm_backend_t aes_gcm;
    mailboxd_chacha_backend_t chacha;
    mailboxd_x25519_backend_t x25519;
    mailboxd_random_backend_t random;
} mailboxd_crypto_config_t;

/** Bundled defaults: tinysha256, tinyaes, monocypher, system random. */
void mailboxd_crypto_config_defaults(mailboxd_crypto_config_t *cfg);

/** Load `[crypto]` from @p config (missing keys keep defaults). */
void mailboxd_crypto_config_apply(const struct mailboxd_config *config);

/** Active system-wide crypto settings (defaults until apply is called). */
const mailboxd_crypto_config_t *mailboxd_crypto_config_get(void);

const char *mailboxd_password_hash_backend_name(mailboxd_password_hash_backend_t b);
const char *mailboxd_aes_gcm_backend_name(mailboxd_aes_gcm_backend_t b);
const char *mailboxd_chacha_backend_name(mailboxd_chacha_backend_t b);
const char *mailboxd_x25519_backend_name(mailboxd_x25519_backend_t b);
const char *mailboxd_random_backend_name(mailboxd_random_backend_t b);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_CRYPTO_CONFIG_H */
