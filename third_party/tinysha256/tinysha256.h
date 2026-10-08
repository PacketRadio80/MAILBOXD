#ifndef TINYSHA256_H
#define TINYSHA256_H

/*
 * tinysha256 — public-domain SHA-256 for MailboxD (bundled, no external deps).
 * Adapted from the Unlicense implementation by 983 (https://github.com/983/SHA-256).
 */

#include <stddef.h>
#include <stdint.h>

#define TINYSHA256_HEX_SIZE (64 + 1)
#define TINYSHA256_BYTES_SIZE 32

void tinysha256_hex(const void *src, size_t n_bytes, char *dst_hex65);
void tinysha256_bytes(const void *src, size_t n_bytes, void *dst_bytes32);

typedef struct tinysha256_ctx {
    uint32_t state[8];
    uint8_t buffer[64];
    uint64_t n_bits;
    uint8_t buffer_counter;
} tinysha256_ctx_t;

void tinysha256_init(tinysha256_ctx_t *ctx);
void tinysha256_append(tinysha256_ctx_t *ctx, const void *data, size_t n_bytes);
void tinysha256_finalize_hex(tinysha256_ctx_t *ctx, char *dst_hex65);
void tinysha256_finalize_bytes(tinysha256_ctx_t *ctx, void *dst_bytes32);

#endif /* TINYSHA256_H */
