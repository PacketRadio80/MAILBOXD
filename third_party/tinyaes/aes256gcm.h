/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MAILBOXD_AES256GCM_H
#define MAILBOXD_AES256GCM_H

#include <stddef.h>
#include <stdint.h>

#define MAILBOXD_AES256GCM_KEY_SIZE   32
#define MAILBOXD_AES256GCM_NONCE_SIZE 12
#define MAILBOXD_AES256GCM_TAG_SIZE   16

/**
 * AES-256-GCM encrypt. @p ciphertext length equals @p plaintext_len.
 * @p tag receives the 16-byte authentication tag.
 */
int mailboxd_aes256gcm_encrypt(const uint8_t key[MAILBOXD_AES256GCM_KEY_SIZE],
                            const uint8_t nonce[MAILBOXD_AES256GCM_NONCE_SIZE],
                            const uint8_t *aad, size_t aad_len,
                            const uint8_t *plaintext, size_t plaintext_len,
                            uint8_t *ciphertext,
                            uint8_t tag[MAILBOXD_AES256GCM_TAG_SIZE]);

/**
 * AES-256-GCM decrypt. Returns 0 on success (tag valid), -1 on failure.
 */
int mailboxd_aes256gcm_decrypt(const uint8_t key[MAILBOXD_AES256GCM_KEY_SIZE],
                            const uint8_t nonce[MAILBOXD_AES256GCM_NONCE_SIZE],
                            const uint8_t *aad, size_t aad_len,
                            const uint8_t *ciphertext, size_t ciphertext_len,
                            uint8_t *plaintext,
                            const uint8_t tag[MAILBOXD_AES256GCM_TAG_SIZE]);

#endif
