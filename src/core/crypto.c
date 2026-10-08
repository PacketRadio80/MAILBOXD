/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/crypto.h"

#include "crypto_backends.h"
#include "monocypher.h"

#include <string.h>

#define MAILBOXD_CRYPTO_MAX_BYTES (256u * 1024u)

static mailboxd_result_t crypto_len_ok(size_t len)
{
    if (len > MAILBOXD_CRYPTO_MAX_BYTES) {
        return MAILBOXD_ERR_INVALID;
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_crypto_random(uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    return mailboxd_backend_random(buf, len);
}

const char *mailboxd_crypto_alg_name(mailboxd_crypto_alg_t alg)
{
    switch (alg) {
    case MAILBOXD_CRYPTO_AES_256_GCM:
        return "aes-256-gcm";
    case MAILBOXD_CRYPTO_XCHACHA20_POLY1305:
        return "xchacha20-poly1305";
    case MAILBOXD_CRYPTO_X25519_AEAD:
        return "x25519-xchacha20-poly1305";
    default:
        return "unknown";
    }
}

size_t mailboxd_crypto_nonce_size(mailboxd_crypto_alg_t alg)
{
    switch (alg) {
    case MAILBOXD_CRYPTO_AES_256_GCM:
        return MAILBOXD_CRYPTO_AES_GCM_NONCE;
    case MAILBOXD_CRYPTO_XCHACHA20_POLY1305:
        return MAILBOXD_CRYPTO_XCHACHA_NONCE;
    default:
        return 0;
    }
}

static mailboxd_result_t derive_x25519_key(const uint8_t shared[32],
                                        uint8_t key[32])
{
    static const uint8_t label[] = "mailboxd-x25519-aead-key-v1";

    mailboxd_backend_blake2b_keyed(key, 32, shared, 32, label, sizeof(label) - 1);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_crypto_encrypt(mailboxd_crypto_alg_t alg,
                                    const uint8_t key[MAILBOXD_CRYPTO_KEY_SIZE],
                                    const uint8_t *nonce, size_t nonce_len,
                                    const uint8_t *aad, size_t aad_len,
                                    const uint8_t *plaintext,
                                    size_t plaintext_len,
                                    uint8_t *ciphertext,
                                    uint8_t tag[MAILBOXD_CRYPTO_TAG_SIZE])
{
    mailboxd_result_t rc;

    if (key == NULL || nonce == NULL || tag == NULL ||
        (plaintext_len > 0 && (plaintext == NULL || ciphertext == NULL))) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = crypto_len_ok(plaintext_len);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (alg == MAILBOXD_CRYPTO_AES_256_GCM) {
        if (nonce_len != MAILBOXD_CRYPTO_AES_GCM_NONCE) {
            return MAILBOXD_ERR_INVALID;
        }
        if (mailboxd_backend_aes256gcm_encrypt(key, nonce, aad, aad_len, plaintext,
                                            plaintext_len, ciphertext, tag) !=
            0) {
            return MAILBOXD_ERR_IO;
        }
        return MAILBOXD_OK;
    }

    if (alg == MAILBOXD_CRYPTO_XCHACHA20_POLY1305) {
        uint8_t nonce24[MAILBOXD_CRYPTO_XCHACHA_NONCE];

        if (nonce_len != MAILBOXD_CRYPTO_XCHACHA_NONCE) {
            return MAILBOXD_ERR_INVALID;
        }
        memcpy(nonce24, nonce, sizeof(nonce24));
        mailboxd_backend_chacha_lock(ciphertext, tag, key, nonce24, aad, aad_len,
                                  plaintext, plaintext_len);
        crypto_wipe(nonce24, sizeof(nonce24));
        return MAILBOXD_OK;
    }

    return MAILBOXD_ERR_INVALID;
}

mailboxd_result_t mailboxd_crypto_decrypt(mailboxd_crypto_alg_t alg,
                                    const uint8_t key[MAILBOXD_CRYPTO_KEY_SIZE],
                                    const uint8_t *nonce, size_t nonce_len,
                                    const uint8_t *aad, size_t aad_len,
                                    const uint8_t *ciphertext,
                                    size_t ciphertext_len,
                                    uint8_t *plaintext,
                                    const uint8_t tag[MAILBOXD_CRYPTO_TAG_SIZE])
{
    mailboxd_result_t rc;
    int ok;

    if (key == NULL || nonce == NULL || tag == NULL ||
        (ciphertext_len > 0 && (ciphertext == NULL || plaintext == NULL))) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = crypto_len_ok(ciphertext_len);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    if (alg == MAILBOXD_CRYPTO_AES_256_GCM) {
        if (nonce_len != MAILBOXD_CRYPTO_AES_GCM_NONCE) {
            return MAILBOXD_ERR_INVALID;
        }
        ok = mailboxd_backend_aes256gcm_decrypt(key, nonce, aad, aad_len,
                                           ciphertext, ciphertext_len,
                                           plaintext, tag);
        return ok == 0 ? MAILBOXD_OK : MAILBOXD_ERR_DENIED;
    }

    if (alg == MAILBOXD_CRYPTO_XCHACHA20_POLY1305) {
        uint8_t nonce24[MAILBOXD_CRYPTO_XCHACHA_NONCE];

        if (nonce_len != MAILBOXD_CRYPTO_XCHACHA_NONCE) {
            return MAILBOXD_ERR_INVALID;
        }
        memcpy(nonce24, nonce, sizeof(nonce24));
        ok = mailboxd_backend_chacha_unlock(plaintext, tag, key, nonce24, aad,
                                       aad_len, ciphertext, ciphertext_len);
        crypto_wipe(nonce24, sizeof(nonce24));
        return ok == 0 ? MAILBOXD_OK : MAILBOXD_ERR_DENIED;
    }

    return MAILBOXD_ERR_INVALID;
}

mailboxd_result_t mailboxd_crypto_x25519_keypair(
    uint8_t public_key[MAILBOXD_CRYPTO_X25519_PUBLIC_KEY],
    uint8_t secret_key[MAILBOXD_CRYPTO_X25519_SECRET_KEY])
{
    if (public_key == NULL || secret_key == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_crypto_random(secret_key, MAILBOXD_CRYPTO_X25519_SECRET_KEY) !=
        MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    secret_key[0] &= 248;
    secret_key[31] &= 127;
    secret_key[31] |= 64;

    mailboxd_backend_x25519_public_key(public_key, secret_key);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_crypto_x25519_seal(
    const uint8_t recipient_pk[MAILBOXD_CRYPTO_X25519_PUBLIC_KEY],
    const uint8_t *aad, size_t aad_len,
    const uint8_t *plaintext, size_t plaintext_len,
    uint8_t *sealed, size_t *sealed_len, size_t sealed_cap)
{
    uint8_t ep_sk[32];
    uint8_t ep_pk[32];
    uint8_t shared[32];
    uint8_t key[32];
    uint8_t nonce[MAILBOXD_CRYPTO_XCHACHA_NONCE];
    uint8_t *cipher;
    uint8_t *tag;
    size_t need;
    mailboxd_result_t rc;

    if (recipient_pk == NULL || sealed == NULL || sealed_len == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    rc = crypto_len_ok(plaintext_len);
    if (rc != MAILBOXD_OK) {
        return rc;
    }

    need = MAILBOXD_CRYPTO_X25519_SEAL_OVERHEAD + plaintext_len;
    if (sealed_cap < need) {
        return MAILBOXD_ERR_INVALID;
    }

    if (mailboxd_crypto_random(ep_sk, sizeof(ep_sk)) != MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }
    ep_sk[0] &= 248;
    ep_sk[31] &= 127;
    ep_sk[31] |= 64;

    mailboxd_backend_x25519_public_key(ep_pk, ep_sk);
    mailboxd_backend_x25519_shared(shared, ep_sk, recipient_pk);
    derive_x25519_key(shared, key);

    if (mailboxd_crypto_random(nonce, sizeof(nonce)) != MAILBOXD_OK) {
        crypto_wipe(ep_sk, sizeof(ep_sk));
        crypto_wipe(shared, sizeof(shared));
        crypto_wipe(key, sizeof(key));
        return MAILBOXD_ERR_IO;
    }

    memcpy(sealed, ep_pk, 32);
    memcpy(sealed + 32, nonce, sizeof(nonce));
    cipher = sealed + 32 + sizeof(nonce);
    tag = sealed + 32 + sizeof(nonce) + plaintext_len;

    rc = mailboxd_crypto_encrypt(MAILBOXD_CRYPTO_XCHACHA20_POLY1305, key, nonce,
                              sizeof(nonce), aad, aad_len, plaintext,
                              plaintext_len, cipher, tag);

    crypto_wipe(ep_sk, sizeof(ep_sk));
    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(key, sizeof(key));

    if (rc != MAILBOXD_OK) {
        return rc;
    }

    *sealed_len = need;
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_crypto_x25519_open(
    const uint8_t recipient_sk[MAILBOXD_CRYPTO_X25519_SECRET_KEY],
    const uint8_t *aad, size_t aad_len,
    const uint8_t *sealed, size_t sealed_len,
    uint8_t *plaintext, size_t *plaintext_len, size_t plaintext_cap)
{
    const uint8_t *ep_pk;
    const uint8_t *nonce;
    const uint8_t *cipher;
    const uint8_t *tag;
    size_t cipher_len;
    uint8_t shared[32];
    uint8_t key[32];
    uint8_t tag_buf[MAILBOXD_CRYPTO_TAG_SIZE];
    mailboxd_result_t rc;

    if (recipient_sk == NULL || sealed == NULL || plaintext == NULL ||
        plaintext_len == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    if (sealed_len < MAILBOXD_CRYPTO_X25519_SEAL_OVERHEAD) {
        return MAILBOXD_ERR_INVALID;
    }

    cipher_len = sealed_len - MAILBOXD_CRYPTO_X25519_SEAL_OVERHEAD;
    if (cipher_len > plaintext_cap) {
        return MAILBOXD_ERR_INVALID;
    }

    ep_pk = sealed;
    nonce = sealed + 32;
    cipher = sealed + 32 + MAILBOXD_CRYPTO_XCHACHA_NONCE;
    tag = cipher + cipher_len;
    memcpy(tag_buf, tag, sizeof(tag_buf));

    mailboxd_backend_x25519_shared(shared, recipient_sk, ep_pk);
    derive_x25519_key(shared, key);

    rc = mailboxd_crypto_decrypt(MAILBOXD_CRYPTO_XCHACHA20_POLY1305, key, nonce,
                              MAILBOXD_CRYPTO_XCHACHA_NONCE, aad, aad_len,
                              cipher, cipher_len, plaintext, tag_buf);

    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(key, sizeof(key));

    if (rc != MAILBOXD_OK) {
        return rc;
    }

    *plaintext_len = cipher_len;
    return MAILBOXD_OK;
}
