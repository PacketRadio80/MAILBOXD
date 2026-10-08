/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_CRYPTO_H
#define MAILBOXD_CRYPTO_H

#include "mailboxd/types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MailboxD reversible cryptography.
 *
 * Defaults use bundled in-tree sources (tinyaes, monocypher). Backends are
 * selectable system-wide via `[crypto]` in mailboxd.ini when optional OpenSSL or
 * libsodium support is enabled at build time (see mailboxd/crypto_config.h).
 *
 * Three modern AEAD schemes:
 *   1. AES-256-GCM        (NIST, TLS, IPsec)
 *   2. XChaCha20-Poly1305 (extended-nonce ChaCha, Monocypher)
 *   3. X25519 + XChaCha20-Poly1305 sealed messages (ECIES-style, Monocypher)
 *
 * Password hashing (one-way) remains in mailboxd/password.h (SHA-256 / MD5).
 */

typedef enum mailboxd_crypto_alg {
    MAILBOXD_CRYPTO_AES_256_GCM = 1,
    MAILBOXD_CRYPTO_XCHACHA20_POLY1305 = 2,
    MAILBOXD_CRYPTO_X25519_AEAD = 3
} mailboxd_crypto_alg_t;

#define MAILBOXD_CRYPTO_KEY_SIZE          32
#define MAILBOXD_CRYPTO_TAG_SIZE          16
#define MAILBOXD_CRYPTO_AES_GCM_NONCE     12
#define MAILBOXD_CRYPTO_XCHACHA_NONCE     24
#define MAILBOXD_CRYPTO_X25519_PUBLIC_KEY 32
#define MAILBOXD_CRYPTO_X25519_SECRET_KEY 32
/** Sealed blob overhead: ephemeral_pk(32) + nonce(24) + tag(16). */
#define MAILBOXD_CRYPTO_X25519_SEAL_OVERHEAD 72

/** Fill @p buf with system random bytes. */
mailboxd_result_t mailboxd_crypto_random(uint8_t *buf, size_t len);

/** Human-readable cipher name. */
const char *mailboxd_crypto_alg_name(mailboxd_crypto_alg_t alg);

/** Required nonce length for @p alg (0 for X25519 sealed mode). */
size_t mailboxd_crypto_nonce_size(mailboxd_crypto_alg_t alg);

/**
 * Symmetric AEAD encrypt (algorithms 1 and 2).
 * @p ciphertext length equals @p plaintext_len; authentication tag is separate.
 */
mailboxd_result_t mailboxd_crypto_encrypt(mailboxd_crypto_alg_t alg,
                                    const uint8_t key[MAILBOXD_CRYPTO_KEY_SIZE],
                                    const uint8_t *nonce, size_t nonce_len,
                                    const uint8_t *aad, size_t aad_len,
                                    const uint8_t *plaintext,
                                    size_t plaintext_len,
                                    uint8_t *ciphertext,
                                    uint8_t tag[MAILBOXD_CRYPTO_TAG_SIZE]);

/** Symmetric AEAD decrypt; returns MAILBOXD_ERR_DENIED when the tag is invalid. */
mailboxd_result_t mailboxd_crypto_decrypt(mailboxd_crypto_alg_t alg,
                                    const uint8_t key[MAILBOXD_CRYPTO_KEY_SIZE],
                                    const uint8_t *nonce, size_t nonce_len,
                                    const uint8_t *aad, size_t aad_len,
                                    const uint8_t *ciphertext,
                                    size_t ciphertext_len,
                                    uint8_t *plaintext,
                                    const uint8_t tag[MAILBOXD_CRYPTO_TAG_SIZE]);

/** Generate an X25519 key pair (Montgomery curve, RFC 7748). */
mailboxd_result_t mailboxd_crypto_x25519_keypair(
    uint8_t public_key[MAILBOXD_CRYPTO_X25519_PUBLIC_KEY],
    uint8_t secret_key[MAILBOXD_CRYPTO_X25519_SECRET_KEY]);

/**
 * Asymmetric seal for @p recipient_pk.
 * Output: ephemeral_pk || nonce || ciphertext || tag.
 */
mailboxd_result_t mailboxd_crypto_x25519_seal(
    const uint8_t recipient_pk[MAILBOXD_CRYPTO_X25519_PUBLIC_KEY],
    const uint8_t *aad, size_t aad_len,
    const uint8_t *plaintext, size_t plaintext_len,
    uint8_t *sealed, size_t *sealed_len, size_t sealed_cap);

/** Open a sealed message with @p recipient_sk. */
mailboxd_result_t mailboxd_crypto_x25519_open(
    const uint8_t recipient_sk[MAILBOXD_CRYPTO_X25519_SECRET_KEY],
    const uint8_t *aad, size_t aad_len,
    const uint8_t *sealed, size_t sealed_len,
    uint8_t *plaintext, size_t *plaintext_len, size_t plaintext_cap);

#ifdef __cplusplus
}
#endif

#endif /* MAILBOXD_CRYPTO_H */
