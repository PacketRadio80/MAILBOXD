/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/types.h"

#include <sodium.h>

#include <stdlib.h>
#include <string.h>

static int sodium_ready;

static int ensure_sodium(void)
{
    if (!sodium_ready) {
        if (sodium_init() < 0) {
            return -1;
        }
        sodium_ready = 1;
    }

    return 0;
}

void mailboxd_libsodium_chacha_lock(uint8_t *ciphertext, uint8_t tag[16],
                                 const uint8_t key[32], const uint8_t nonce[24],
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *plaintext, size_t plaintext_len)
{
    unsigned long long out_len = 0;
    uint8_t *packed;
    size_t packed_cap;

    if (ensure_sodium() != 0) {
        return;
    }

    packed_cap = plaintext_len +
                 crypto_aead_xchacha20poly1305_ietf_ABYTES;
    packed = malloc(packed_cap);
    if (packed == NULL) {
        return;
    }

    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            packed, &out_len, plaintext, plaintext_len, aad, aad_len, NULL,
            nonce, key) != 0) {
        free(packed);
        return;
    }

    if (plaintext_len > 0 && ciphertext != NULL) {
        memcpy(ciphertext, packed, plaintext_len);
    }
    memcpy(tag, packed + plaintext_len,
           crypto_aead_xchacha20poly1305_ietf_ABYTES);
    free(packed);
}

int mailboxd_libsodium_chacha_unlock(uint8_t *plaintext, const uint8_t tag[16],
                                  const uint8_t key[32], const uint8_t nonce[24],
                                  const uint8_t *aad, size_t aad_len,
                                  const uint8_t *ciphertext, size_t ciphertext_len)
{
    unsigned long long pt_len = 0;
    uint8_t *packed;
    size_t packed_len;
    int rc;

    if (ensure_sodium() != 0) {
        return -1;
    }

    packed_len = ciphertext_len +
                 crypto_aead_xchacha20poly1305_ietf_ABYTES;
    packed = malloc(packed_len);
    if (packed == NULL) {
        return -1;
    }

    if (ciphertext_len > 0 && ciphertext != NULL) {
        memcpy(packed, ciphertext, ciphertext_len);
    }
    memcpy(packed + ciphertext_len, tag,
           crypto_aead_xchacha20poly1305_ietf_ABYTES);

    rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        plaintext, &pt_len, NULL, packed, packed_len, aad, aad_len, nonce,
        key);

    free(packed);
    return rc;
}

void mailboxd_libsodium_x25519_public_key(uint8_t public_key[32],
                                         const uint8_t secret_key[32])
{
    if (ensure_sodium() != 0) {
        return;
    }

    crypto_scalarmult_base(public_key, secret_key);
}

void mailboxd_libsodium_x25519_shared(uint8_t shared[32],
                                   const uint8_t secret_key[32],
                                   const uint8_t peer_public_key[32])
{
    if (ensure_sodium() != 0) {
        return;
    }

    crypto_scalarmult(shared, secret_key, peer_public_key);
}
