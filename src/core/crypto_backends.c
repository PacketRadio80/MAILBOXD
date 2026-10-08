/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "crypto_backends.h"
#include "mailboxd/crypto_config.h"

#include "aes256gcm.h"
#include "monocypher.h"
#include "tinysha256.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>
#endif

#if defined(MAILBOXD_HAVE_OPENSSL)
void mailboxd_openssl_sha256_hex(const char *data, size_t len, char hex[65]);
int mailboxd_openssl_aes256gcm_encrypt(
    const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
    size_t aad_len, const uint8_t *plaintext, size_t plaintext_len,
    uint8_t *ciphertext, uint8_t tag[16]);
int mailboxd_openssl_aes256gcm_decrypt(
    const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
    size_t aad_len, const uint8_t *ciphertext, size_t ciphertext_len,
    uint8_t *plaintext, const uint8_t tag[16]);
mailboxd_result_t mailboxd_openssl_random(uint8_t *buf, size_t len);
#endif

#if defined(MAILBOXD_HAVE_LIBSODIUM)
void mailboxd_libsodium_chacha_lock(uint8_t *ciphertext, uint8_t tag[16],
                                 const uint8_t key[32], const uint8_t nonce[24],
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *plaintext, size_t plaintext_len);
int mailboxd_libsodium_chacha_unlock(uint8_t *plaintext, const uint8_t tag[16],
                                  const uint8_t key[32], const uint8_t nonce[24],
                                  const uint8_t *aad, size_t aad_len,
                                  const uint8_t *ciphertext, size_t ciphertext_len);
void mailboxd_libsodium_x25519_public_key(uint8_t public_key[32],
                                       const uint8_t secret_key[32]);
void mailboxd_libsodium_x25519_shared(uint8_t shared[32],
                                   const uint8_t secret_key[32],
                                   const uint8_t peer_public_key[32]);
#endif

void mailboxd_backend_sha256_hex(const char *data, size_t len, char hex[65])
{
#if defined(MAILBOXD_HAVE_OPENSSL)
    const mailboxd_crypto_config_t *cfg = mailboxd_crypto_config_get();

    if (cfg->password_hash == MAILBOXD_PASSWORD_HASH_OPENSSL) {
        mailboxd_openssl_sha256_hex(data, len, hex);
        return;
    }
#endif

    tinysha256_hex(data, len, hex);
}

int mailboxd_backend_aes256gcm_encrypt(
    const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
    size_t aad_len, const uint8_t *plaintext, size_t plaintext_len,
    uint8_t *ciphertext, uint8_t tag[16])
{
#if defined(MAILBOXD_HAVE_OPENSSL)
    const mailboxd_crypto_config_t *cfg = mailboxd_crypto_config_get();

    if (cfg->aes_gcm == MAILBOXD_AES_GCM_OPENSSL) {
        return mailboxd_openssl_aes256gcm_encrypt(key, nonce, aad, aad_len,
                                                 plaintext, plaintext_len,
                                                 ciphertext, tag);
    }
#endif

    return mailboxd_aes256gcm_encrypt(key, nonce, aad, aad_len, plaintext,
                                   plaintext_len, ciphertext, tag);
}

int mailboxd_backend_aes256gcm_decrypt(
    const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
    size_t aad_len, const uint8_t *ciphertext, size_t ciphertext_len,
    uint8_t *plaintext, const uint8_t tag[16])
{
#if defined(MAILBOXD_HAVE_OPENSSL)
    const mailboxd_crypto_config_t *cfg = mailboxd_crypto_config_get();

    if (cfg->aes_gcm == MAILBOXD_AES_GCM_OPENSSL) {
        return mailboxd_openssl_aes256gcm_decrypt(key, nonce, aad, aad_len,
                                               ciphertext, ciphertext_len,
                                               plaintext, tag);
    }
#endif

    return mailboxd_aes256gcm_decrypt(key, nonce, aad, aad_len, ciphertext,
                                   ciphertext_len, plaintext, tag);
}

void mailboxd_backend_chacha_lock(uint8_t *ciphertext, uint8_t tag[16],
                               const uint8_t key[32], const uint8_t nonce[24],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t *plaintext, size_t plaintext_len)
{
#if defined(MAILBOXD_HAVE_LIBSODIUM)
    const mailboxd_crypto_config_t *cfg = mailboxd_crypto_config_get();

    if (cfg->chacha == MAILBOXD_CHACHA_LIBSODIUM) {
        mailboxd_libsodium_chacha_lock(ciphertext, tag, key, nonce, aad, aad_len,
                                    plaintext, plaintext_len);
        return;
    }
#endif

    crypto_aead_lock(ciphertext, tag, key, nonce, aad, aad_len, plaintext,
                     plaintext_len);
}

int mailboxd_backend_chacha_unlock(uint8_t *plaintext, const uint8_t tag[16],
                                const uint8_t key[32], const uint8_t nonce[24],
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t *ciphertext, size_t ciphertext_len)
{
#if defined(MAILBOXD_HAVE_LIBSODIUM)
    const mailboxd_crypto_config_t *cfg = mailboxd_crypto_config_get();

    if (cfg->chacha == MAILBOXD_CHACHA_LIBSODIUM) {
        return mailboxd_libsodium_chacha_unlock(plaintext, tag, key, nonce, aad,
                                             aad_len, ciphertext,
                                             ciphertext_len);
    }
#endif

    return crypto_aead_unlock(plaintext, tag, key, nonce, aad, aad_len,
                              ciphertext, ciphertext_len);
}

void mailboxd_backend_x25519_public_key(uint8_t public_key[32],
                                     const uint8_t secret_key[32])
{
#if defined(MAILBOXD_HAVE_LIBSODIUM)
    const mailboxd_crypto_config_t *cfg = mailboxd_crypto_config_get();

    if (cfg->x25519 == MAILBOXD_X25519_LIBSODIUM) {
        mailboxd_libsodium_x25519_public_key(public_key, secret_key);
        return;
    }
#endif

    crypto_x25519_public_key(public_key, secret_key);
}

void mailboxd_backend_x25519_shared(uint8_t shared[32],
                                 const uint8_t secret_key[32],
                                 const uint8_t peer_public_key[32])
{
#if defined(MAILBOXD_HAVE_LIBSODIUM)
    const mailboxd_crypto_config_t *cfg = mailboxd_crypto_config_get();

    if (cfg->x25519 == MAILBOXD_X25519_LIBSODIUM) {
        mailboxd_libsodium_x25519_shared(shared, secret_key, peer_public_key);
        return;
    }
#endif

    crypto_x25519(shared, secret_key, peer_public_key);
}

void mailboxd_backend_blake2b_keyed(uint8_t *hash, size_t hash_size,
                                 const uint8_t *message, size_t message_size,
                                 const uint8_t *key, size_t key_size)
{
    crypto_blake2b_keyed(hash, hash_size, message, message_size, key, key_size);
}

static mailboxd_result_t system_random(uint8_t *buf, size_t len)
{
    size_t done = 0;

#if defined(__linux__)
    while (done < len) {
        ssize_t n = getrandom(buf + done, len - done, 0);

        if (n < 0) {
            break;
        }
        done += (size_t)n;
    }
#endif

    if (done < len) {
        int fd = open("/dev/urandom", O_RDONLY);

        if (fd < 0) {
            return MAILBOXD_ERR_IO;
        }

        while (done < len) {
            ssize_t n = read(fd, buf + done, len - done);

            if (n <= 0) {
                close(fd);
                return MAILBOXD_ERR_IO;
            }
            done += (size_t)n;
        }

        close(fd);
    }

    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_backend_random(uint8_t *buf, size_t len)
{
#if defined(MAILBOXD_HAVE_OPENSSL)
    const mailboxd_crypto_config_t *cfg = mailboxd_crypto_config_get();

    if (cfg->random == MAILBOXD_RANDOM_OPENSSL) {
        return mailboxd_openssl_random(buf, len);
    }
#endif

    return system_random(buf, len);
}
