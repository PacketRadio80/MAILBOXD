/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/types.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <stdio.h>
#include <string.h>

void mailboxd_openssl_sha256_hex(const char *data, size_t len, char hex[65])
{
    unsigned char digest[32];
    unsigned int digest_len = sizeof(digest);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();

    if (ctx == NULL) {
        hex[0] = '\0';
        return;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        hex[0] = '\0';
        return;
    }

    EVP_MD_CTX_free(ctx);

    for (size_t i = 0; i < digest_len; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
}

static int openssl_gcm_crypt(int encrypt, const uint8_t key[32],
                             const uint8_t nonce[12], const uint8_t *aad,
                             size_t aad_len, const uint8_t *in, size_t in_len,
                             uint8_t *out, const uint8_t tag_in[16],
                             uint8_t tag_out[16])
{
    EVP_CIPHER_CTX *ctx = NULL;
    int out_len = 0;
    int total = 0;
    int rc = -1;

    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        return -1;
    }

    if (EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL, encrypt) !=
            1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) != 1 ||
        EVP_CipherInit_ex(ctx, NULL, NULL, key, nonce, encrypt) != 1) {
        goto done;
    }

    if (aad_len > 0 && aad != NULL) {
        if (EVP_CipherUpdate(ctx, NULL, &out_len, aad, (int)aad_len) != 1) {
            goto done;
        }
    }

    if (in_len > 0) {
        if (EVP_CipherUpdate(ctx, out, &out_len, in, (int)in_len) != 1) {
            goto done;
        }
        total = out_len;
    }

    if (encrypt) {
        if (EVP_EncryptFinal_ex(ctx, out + total, &out_len) != 1) {
            goto done;
        }
        if (tag_out == NULL ||
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag_out) != 1) {
            goto done;
        }
        rc = 0;
        goto done;
    }

    if (tag_in == NULL ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                            (void *)(uintptr_t)tag_in) != 1) {
        goto done;
    }

    if (EVP_DecryptFinal_ex(ctx, out + total, &out_len) != 1) {
        goto done;
    }

    rc = 0;
done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

int mailboxd_openssl_aes256gcm_encrypt(
    const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
    size_t aad_len, const uint8_t *plaintext, size_t plaintext_len,
    uint8_t *ciphertext, uint8_t tag[16])
{
    if (key == NULL || nonce == NULL || tag == NULL) {
        return -1;
    }
    if (plaintext_len > 0 && (plaintext == NULL || ciphertext == NULL)) {
        return -1;
    }

    return openssl_gcm_crypt(1, key, nonce, aad, aad_len, plaintext,
                             plaintext_len, ciphertext, NULL, tag);
}

int mailboxd_openssl_aes256gcm_decrypt(
    const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
    size_t aad_len, const uint8_t *ciphertext, size_t ciphertext_len,
    uint8_t *plaintext, const uint8_t tag[16])
{
    if (key == NULL || nonce == NULL || tag == NULL) {
        return -1;
    }
    if (ciphertext_len > 0 && (ciphertext == NULL || plaintext == NULL)) {
        return -1;
    }

    return openssl_gcm_crypt(0, key, nonce, aad, aad_len, ciphertext,
                             ciphertext_len, plaintext, tag, NULL);
}

mailboxd_result_t mailboxd_openssl_random(uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return MAILBOXD_ERR_INVALID;
    }

    if (RAND_bytes(buf, (int)len) == 1) {
        return MAILBOXD_OK;
    }

    return MAILBOXD_ERR_IO;
}
