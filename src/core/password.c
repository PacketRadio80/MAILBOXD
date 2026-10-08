/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/password.h"
#include "mailboxd/crypto.h"
#include "mailboxd/util.h"

#include "crypto_backends.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define MD5_HEX_SIZE (32 + 1)

static int str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }

    while (*a != '\0' && *b != '\0') {
        char ca = (char)(*a >= 'A' && *a <= 'Z' ? *a + 32 : *a);
        char cb = (char)(*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);

        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static int hex_string_eq(const char *a, const char *b, size_t hex_len)
{
    size_t i;

    for (i = 0; i < hex_len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }

    return 1;
}

static int hex_string_valid(const char *s, size_t hex_len)
{
    size_t i;

    if (s == NULL) {
        return 0;
    }

    for (i = 0; i < hex_len; i++) {
        if (hex_digit(s[i]) < 0) {
            return 0;
        }
    }

    return s[hex_len] == '\0';
}

int mailboxd_password_is_hashed(const char *stored)
{
    if (stored == NULL || stored[0] == '\0') {
        return 0;
    }

    if (strncmp(stored, MAILBOXD_PASSWORD_SHA256_PREFIX, 8) == 0) {
        return hex_string_valid(stored + 8, 64);
    }

    if (strncmp(stored, MAILBOXD_PASSWORD_MD5_PREFIX, 5) == 0) {
        return hex_string_valid(stored + 5, 32);
    }

    return 0;
}

int mailboxd_password_is_plain(const char *stored)
{
    if (stored == NULL || stored[0] == '\0') {
        return 0;
    }

    return !mailboxd_password_is_hashed(stored);
}

mailboxd_result_t mailboxd_password_hash_sha256(const char *plain,
                                          char *out,
                                          size_t out_size)
{
    char hex[MAILBOXD_BACKEND_SHA256_HEX_SIZE];
    size_t need;

    if (plain == NULL || out == NULL) {
        return MAILBOXD_ERR_INVALID;
    }

    need = strlen(MAILBOXD_PASSWORD_SHA256_PREFIX) + 64 + 1;
    if (out_size < need) {
        return MAILBOXD_ERR_INVALID;
    }

    mailboxd_backend_sha256_hex(plain, strlen(plain), hex);
    snprintf(out, out_size, "%s%s", MAILBOXD_PASSWORD_SHA256_PREFIX, hex);
    return MAILBOXD_OK;
}

mailboxd_result_t mailboxd_password_hash(const char *plain, char *out, size_t out_size)
{
    return mailboxd_password_hash_sha256(plain, out, out_size);
}

/* Minimal MD5 (RFC 1321) for legacy {md5} verification only. */
typedef struct md5_ctx {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t buffer[64];
} md5_ctx_t;

#define MD5_F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | (~z)))
#define MD5_ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define MD5_FF(a, b, c, d, x, s, ac) \
    do { \
        (a) += MD5_F((b), (c), (d)) + (x) + (uint32_t)(ac); \
        (a) = MD5_ROTATE_LEFT((a), (s)); \
        (a) += (b); \
    } while (0)

#define MD5_GG(a, b, c, d, x, s, ac) \
    do { \
        (a) += MD5_G((b), (c), (d)) + (x) + (uint32_t)(ac); \
        (a) = MD5_ROTATE_LEFT((a), (s)); \
        (a) += (b); \
    } while (0)

#define MD5_HH(a, b, c, d, x, s, ac) \
    do { \
        (a) += MD5_H((b), (c), (d)) + (x) + (uint32_t)(ac); \
        (a) = MD5_ROTATE_LEFT((a), (s)); \
        (a) += (b); \
    } while (0)

#define MD5_II(a, b, c, d, x, s, ac) \
    do { \
        (a) += MD5_I((b), (c), (d)) + (x) + (uint32_t)(ac); \
        (a) = MD5_ROTATE_LEFT((a), (s)); \
        (a) += (b); \
    } while (0)

static void md5_encode(uint8_t *out, const uint32_t *in, size_t len)
{
    size_t i;
    size_t j;

    for (i = 0, j = 0; j < len; i++, j += 4) {
        out[j] = (uint8_t)(in[i] & 0xff);
        out[j + 1] = (uint8_t)((in[i] >> 8) & 0xff);
        out[j + 2] = (uint8_t)((in[i] >> 16) & 0xff);
        out[j + 3] = (uint8_t)((in[i] >> 24) & 0xff);
    }
}

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t x[16];
    size_t i;

    for (i = 0; i < 16; i++) {
        x[i] = ((uint32_t)block[i * 4]) |
               ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    }

    MD5_FF(a, b, c, d, x[0], 7, 0xd76aa478);
    MD5_FF(d, a, b, c, x[1], 12, 0xe8c7b756);
    MD5_FF(c, d, a, b, x[2], 17, 0x242070db);
    MD5_FF(b, c, d, a, x[3], 22, 0xc1bdceee);
    MD5_FF(a, b, c, d, x[4], 7, 0xf57c0faf);
    MD5_FF(d, a, b, c, x[5], 12, 0x4787c62a);
    MD5_FF(c, d, a, b, x[6], 17, 0xa8304613);
    MD5_FF(b, c, d, a, x[7], 22, 0xfd469501);
    MD5_FF(a, b, c, d, x[8], 7, 0x698098d8);
    MD5_FF(d, a, b, c, x[9], 12, 0x8b44f7af);
    MD5_FF(c, d, a, b, x[10], 17, 0xffff5bb1);
    MD5_FF(b, c, d, a, x[11], 22, 0x895cd7be);
    MD5_FF(a, b, c, d, x[12], 7, 0x6b901122);
    MD5_FF(d, a, b, c, x[13], 12, 0xfd987193);
    MD5_FF(c, d, a, b, x[14], 17, 0xa679438e);
    MD5_FF(b, c, d, a, x[15], 22, 0x49b40821);
    MD5_GG(a, b, c, d, x[1], 5, 0xf61e2562);
    MD5_GG(d, a, b, c, x[6], 9, 0xc040b340);
    MD5_GG(c, d, a, b, x[11], 14, 0x265e5a51);
    MD5_GG(b, c, d, a, x[0], 20, 0xe9b6c7aa);
    MD5_GG(a, b, c, d, x[5], 5, 0xd62f105d);
    MD5_GG(d, a, b, c, x[10], 9, 0x02441453);
    MD5_GG(c, d, a, b, x[15], 14, 0xd8a1e681);
    MD5_GG(b, c, d, a, x[4], 20, 0xe7d3fbc8);
    MD5_GG(a, b, c, d, x[9], 5, 0x21e1cde6);
    MD5_GG(d, a, b, c, x[14], 9, 0xc33707d6);
    MD5_GG(c, d, a, b, x[3], 14, 0xf4d50d87);
    MD5_GG(b, c, d, a, x[8], 20, 0x455a14ed);
    MD5_GG(a, b, c, d, x[13], 5, 0xa9e3e905);
    MD5_GG(d, a, b, c, x[2], 9, 0xfcefa3f8);
    MD5_GG(c, d, a, b, x[7], 14, 0x676f02d9);
    MD5_GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);
    MD5_HH(a, b, c, d, x[5], 4, 0xfffa3942);
    MD5_HH(d, a, b, c, x[8], 11, 0x8771f681);
    MD5_HH(c, d, a, b, x[11], 16, 0x6d9d6122);
    MD5_HH(b, c, d, a, x[14], 23, 0xfde5380c);
    MD5_HH(a, b, c, d, x[1], 4, 0xa4beea44);
    MD5_HH(d, a, b, c, x[4], 11, 0x4bdecfa9);
    MD5_HH(c, d, a, b, x[7], 16, 0xf6bb4b60);
    MD5_HH(b, c, d, a, x[10], 23, 0xbebfbc70);
    MD5_HH(a, b, c, d, x[13], 4, 0x289b7ec6);
    MD5_HH(d, a, b, c, x[0], 11, 0xeaa127fa);
    MD5_HH(c, d, a, b, x[3], 16, 0xd4ef3085);
    MD5_HH(b, c, d, a, x[6], 23, 0x04881d05);
    MD5_HH(a, b, c, d, x[9], 4, 0xd9d4d039);
    MD5_HH(d, a, b, c, x[12], 11, 0xe6db99e5);
    MD5_HH(c, d, a, b, x[15], 16, 0x1fa27cf8);
    MD5_HH(b, c, d, a, x[2], 23, 0xc4ac5665);
    MD5_II(a, b, c, d, x[0], 6, 0xf4292244);
    MD5_II(d, a, b, c, x[7], 10, 0x432aff97);
    MD5_II(c, d, a, b, x[14], 15, 0xab9423a7);
    MD5_II(b, c, d, a, x[5], 21, 0xfc93a039);
    MD5_II(a, b, c, d, x[12], 6, 0x655b59c3);
    MD5_II(d, a, b, c, x[3], 10, 0x8f0ccc92);
    MD5_II(c, d, a, b, x[10], 15, 0xffeff47d);
    MD5_II(b, c, d, a, x[1], 21, 0x85845dd1);
    MD5_II(a, b, c, d, x[8], 6, 0x6fa87e4f);
    MD5_II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
    MD5_II(c, d, a, b, x[6], 15, 0xa3014314);
    MD5_II(b, c, d, a, x[13], 21, 0x4e0811a1);
    MD5_II(a, b, c, d, x[4], 6, 0xf7537e82);
    MD5_II(d, a, b, c, x[11], 10, 0xbd3af235);
    MD5_II(c, d, a, b, x[2], 15, 0x2ad7d2bb);
    MD5_II(b, c, d, a, x[9], 21, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_init(md5_ctx_t *ctx)
{
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md5_update(md5_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i;
    size_t index = (size_t)((ctx->count[0] >> 3) & 0x3f);

    if ((ctx->count[0] += (uint32_t)(len << 3)) < (uint32_t)(len << 3)) {
        ctx->count[1]++;
    }
    ctx->count[1] += (uint32_t)(len >> 29);

    size_t part_len = 64 - index;

    if (len >= part_len) {
        memcpy(&ctx->buffer[index], data, part_len);
        md5_transform(ctx->state, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64) {
            memcpy(ctx->buffer, &data[i], 64);
            md5_transform(ctx->state, ctx->buffer);
        }
        index = 0;
    } else {
        i = 0;
    }

    memcpy(&ctx->buffer[index], &data[i], len - i);
}

static void md5_finalize_hex(md5_ctx_t *ctx, char *hex)
{
    uint8_t bits[8];
    uint8_t digest[16];
    size_t index = (size_t)((ctx->count[0] >> 3) & 0x3f);
    size_t pad_len = (index < 56) ? (56 - index) : (120 - index);
    static const uint8_t padding[64] = { 0x80 };
    size_t i;

    md5_encode(bits, ctx->count, 8);
    md5_update(ctx, padding, pad_len);
    md5_update(ctx, bits, 8);
    md5_encode(digest, ctx->state, 16);

    for (i = 0; i < 16; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
}

static void md5_hex(const char *plain, char *hex)
{
    md5_ctx_t ctx;

    md5_init(&ctx);
    md5_update(&ctx, (const uint8_t *)plain, strlen(plain));
    md5_finalize_hex(&ctx, hex);
}

int mailboxd_password_match(const char *stored, const char *provided)
{
    char hex[MAILBOXD_BACKEND_SHA256_HEX_SIZE];
    char md5[MD5_HEX_SIZE];

    if (provided == NULL || provided[0] == '\0') {
        return 0;
    }

    if (stored == NULL || stored[0] == '\0') {
        return 0;
    }

    if (strncmp(stored, MAILBOXD_PASSWORD_SHA256_PREFIX, 8) == 0) {
        mailboxd_backend_sha256_hex(provided, strlen(provided), hex);
        return hex_string_eq(stored + 8, hex, 64);
    }

    if (strncmp(stored, MAILBOXD_PASSWORD_MD5_PREFIX, 5) == 0) {
        md5_hex(provided, md5);
        return hex_string_eq(stored + 5, md5, 32);
    }

    return str_ieq(stored, provided);
}

mailboxd_result_t mailboxd_password_generate_alnum(char *out,
                                            size_t out_size,
                                            size_t min_len,
                                            size_t max_len)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    uint8_t rnd[32];
    size_t len;
    size_t span;
    size_t i;
    size_t rnd_off = sizeof(rnd);

    if (out == NULL || out_size == 0 || min_len == 0 || min_len > max_len ||
        max_len >= out_size) {
        return MAILBOXD_ERR_INVALID;
    }

    span = max_len - min_len + 1u;
    if (mailboxd_crypto_random(rnd, sizeof(rnd)) != MAILBOXD_OK) {
        return MAILBOXD_ERR_IO;
    }

    len = min_len + (size_t)(rnd[0] % (unsigned)span);
    for (i = 0; i < len; i++) {
        if (rnd_off >= sizeof(rnd)) {
            if (mailboxd_crypto_random(rnd, sizeof(rnd)) != MAILBOXD_OK) {
                return MAILBOXD_ERR_IO;
            }
            rnd_off = 0;
        }
        out[i] = alphabet[rnd[rnd_off++] % 36u];
    }
    out[len] = '\0';
    return MAILBOXD_OK;
}
