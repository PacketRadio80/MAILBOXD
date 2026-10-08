/*
 * AES-256-GCM for MailboxD (NIST SP 800-38D) using bundled tiny-AES-c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "aes256gcm.h"
#include "aes.h"

#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t hi;
    uint64_t lo;
} u128;

static u128 u128_from_be(const uint8_t in[16])
{
    u128 v;
    int i;

    v.hi = 0;
    v.lo = 0;
    for (i = 0; i < 8; i++) {
        v.hi = (v.hi << 8) | in[i];
        v.lo = (v.lo << 8) | in[8 + i];
    }
    return v;
}

static void u128_to_be(u128 v, uint8_t out[16])
{
    int i;

    for (i = 7; i >= 0; i--) {
        out[i] = (uint8_t)(v.hi & 0xff);
        v.hi >>= 8;
    }
    for (i = 15; i >= 8; i--) {
        out[i] = (uint8_t)(v.lo & 0xff);
        v.lo >>= 8;
    }
}

static void u128_xor(u128 *a, const u128 *b)
{
    a->hi ^= b->hi;
    a->lo ^= b->lo;
}

static void u128_shr1(u128 *v)
{
    int lsb = (int)(v->lo & 1);

    v->lo = (v->hi << 63) | (v->lo >> 1);
    v->hi >>= 1;
    if (lsb) {
        v->hi ^= 0xe100000000000000ULL;
    }
}

static void gcm_gmult(uint8_t x[16], const uint8_t y[16])
{
    u128 V = u128_from_be(y);
    u128 Z = { 0, 0 };
    int i;

    for (i = 0; i < 128; i++) {
        if (x[i / 8] & (0x80u >> (i % 8))) {
            u128_xor(&Z, &V);
        }
        if (i == 127) {
            break;
        }
        u128_shr1(&V);
    }

    u128_to_be(Z, x);
}

static void xor_block(uint8_t dst[16], const uint8_t a[16], const uint8_t b[16])
{
    int i;

    for (i = 0; i < 16; i++) {
        dst[i] = (uint8_t)(a[i] ^ b[i]);
    }
}

static void aes_ecb_encrypt_block(const struct AES_ctx *ctx,
                                  const uint8_t in[16], uint8_t out[16])
{
    uint8_t buf[16];

    memcpy(buf, in, 16);
    AES_ECB_encrypt((struct AES_ctx *)ctx, buf);
    memcpy(out, buf, 16);
}

static void ghash(const uint8_t h[16], const uint8_t *aad, size_t aad_len,
                  const uint8_t *cipher, size_t cipher_len, uint8_t out[16])
{
    uint8_t y[16] = { 0 };
    uint8_t block[16];
    size_t i;

    for (i = 0; i + 16 <= aad_len; i += 16) {
        xor_block(y, y, aad + i);
        gcm_gmult(y, h);
    }
    if (i < aad_len) {
        memset(block, 0, sizeof(block));
        memcpy(block, aad + i, aad_len - i);
        xor_block(y, y, block);
        gcm_gmult(y, h);
    }

    for (i = 0; i + 16 <= cipher_len; i += 16) {
        xor_block(y, y, cipher + i);
        gcm_gmult(y, h);
    }
    if (i < cipher_len) {
        memset(block, 0, sizeof(block));
        memcpy(block, cipher + i, cipher_len - i);
        xor_block(y, y, block);
        gcm_gmult(y, h);
    }

    memset(block, 0, sizeof(block));
    for (i = 0; i < 8; i++) {
        block[7 - i] = (uint8_t)((aad_len * 8) >> (i * 8));
        block[15 - i] = (uint8_t)((cipher_len * 8) >> (i * 8));
    }
    xor_block(y, y, block);
    gcm_gmult(y, h);
    memcpy(out, y, 16);
}

static void gctr(struct AES_ctx *ctx, const uint8_t icb[16], uint8_t *data,
                 size_t len)
{
    uint8_t counter[16];
    uint8_t stream[16];
    size_t i;

    memcpy(counter, icb, 16);
    for (i = 0; i < len; i += 16) {
        size_t j;

        aes_ecb_encrypt_block(ctx, counter, stream);
        for (j = 0; j < 16 && i + j < len; j++) {
            data[i + j] ^= stream[j];
        }
        {
            int k;

            for (k = 15; k >= 12; k--) {
                counter[k] = (uint8_t)(counter[k] + 1);
                if (counter[k] != 0) {
                    break;
                }
            }
        }
    }
}

static void inc32(uint8_t block[16])
{
    int i;

    for (i = 15; i >= 12; i--) {
        block[i] = (uint8_t)(block[i] + 1);
        if (block[i] != 0) {
            break;
        }
    }
}

static int aes256gcm_crypt(int encrypt, const uint8_t key[32],
                           const uint8_t nonce[12], const uint8_t *aad,
                           size_t aad_len, const uint8_t *in, size_t in_len,
                           uint8_t *out, const uint8_t tag_in[16],
                           uint8_t tag_out[16])
{
    struct AES_ctx ctx;
    uint8_t h[16] = { 0 };
    uint8_t j0[16];
    uint8_t s[16];
    uint8_t expected[16];
    int diff = 0;
    size_t i;

    if (key == NULL || nonce == NULL) {
        return -1;
    }
    if (in_len > 0 && (in == NULL || out == NULL)) {
        return -1;
    }

    AES_init_ctx(&ctx, key);
    aes_ecb_encrypt_block(&ctx, h, h);

    memset(j0, 0, sizeof(j0));
    memcpy(j0, nonce, 12);
    j0[15] = 1;

    if (in_len > 0) {
        memcpy(out, in, in_len);
        if (encrypt) {
            memcpy(expected, j0, 16);
            inc32(expected);
            gctr(&ctx, expected, out, in_len);
        }
    }

    ghash(h, aad, aad_len, out, in_len, s);
    memcpy(expected, j0, 16);
    gctr(&ctx, expected, s, 16);

    if (encrypt) {
        if (tag_out != NULL) {
            memcpy(tag_out, s, 16);
        }
        return 0;
    }

    if (tag_in == NULL) {
        return -1;
    }

    for (i = 0; i < 16; i++) {
        diff |= (int)(s[i] ^ tag_in[i]);
    }
    if (diff != 0) {
        return -1;
    }

    if (in_len > 0) {
        memcpy(expected, j0, 16);
        inc32(expected);
        memcpy(out, in, in_len);
        gctr(&ctx, expected, out, in_len);
    }

    return 0;
}

int mailboxd_aes256gcm_encrypt(const uint8_t key[MAILBOXD_AES256GCM_KEY_SIZE],
                            const uint8_t nonce[MAILBOXD_AES256GCM_NONCE_SIZE],
                            const uint8_t *aad, size_t aad_len,
                            const uint8_t *plaintext, size_t plaintext_len,
                            uint8_t *ciphertext,
                            uint8_t tag[MAILBOXD_AES256GCM_TAG_SIZE])
{
    return aes256gcm_crypt(1, key, nonce, aad, aad_len, plaintext,
                           plaintext_len, ciphertext, NULL, tag);
}

int mailboxd_aes256gcm_decrypt(const uint8_t key[MAILBOXD_AES256GCM_KEY_SIZE],
                            const uint8_t nonce[MAILBOXD_AES256GCM_NONCE_SIZE],
                            const uint8_t *aad, size_t aad_len,
                            const uint8_t *ciphertext, size_t ciphertext_len,
                            uint8_t *plaintext,
                            const uint8_t tag[MAILBOXD_AES256GCM_TAG_SIZE])
{
    return aes256gcm_crypt(0, key, nonce, aad, aad_len, ciphertext,
                           ciphertext_len, plaintext, tag, NULL);
}
