/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mailboxd/crypto_config.h"
#include "mailboxd/config.h"
#include "mailboxd/log.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static mailboxd_crypto_config_t g_crypto_config;
static int g_crypto_config_ready;

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

static mailboxd_password_hash_backend_t parse_password_hash(const char *value)
{
    if (value == NULL || value[0] == '\0' ||
        str_ieq(value, "tinysha256") || str_ieq(value, "bundled") ||
        str_ieq(value, "default")) {
        return MAILBOXD_PASSWORD_HASH_TINYSHA256;
    }

    if (str_ieq(value, "openssl") || str_ieq(value, "libcrypto")) {
        return MAILBOXD_PASSWORD_HASH_OPENSSL;
    }

    return MAILBOXD_PASSWORD_HASH_TINYSHA256;
}

static mailboxd_aes_gcm_backend_t parse_aes_gcm(const char *value)
{
    if (value == NULL || value[0] == '\0' ||
        str_ieq(value, "tinyaes") || str_ieq(value, "bundled") ||
        str_ieq(value, "default")) {
        return MAILBOXD_AES_GCM_TINYAES;
    }

    if (str_ieq(value, "openssl") || str_ieq(value, "libcrypto")) {
        return MAILBOXD_AES_GCM_OPENSSL;
    }

    return MAILBOXD_AES_GCM_TINYAES;
}

static mailboxd_chacha_backend_t parse_chacha(const char *value)
{
    if (value == NULL || value[0] == '\0' ||
        str_ieq(value, "monocypher") || str_ieq(value, "bundled") ||
        str_ieq(value, "default")) {
        return MAILBOXD_CHACHA_MONOCYPHER;
    }

    if (str_ieq(value, "libsodium") || str_ieq(value, "sodium")) {
        return MAILBOXD_CHACHA_LIBSODIUM;
    }

    return MAILBOXD_CHACHA_MONOCYPHER;
}

static mailboxd_x25519_backend_t parse_x25519(const char *value)
{
    if (value == NULL || value[0] == '\0' ||
        str_ieq(value, "monocypher") || str_ieq(value, "bundled") ||
        str_ieq(value, "default")) {
        return MAILBOXD_X25519_MONOCYPHER;
    }

    if (str_ieq(value, "libsodium") || str_ieq(value, "sodium")) {
        return MAILBOXD_X25519_LIBSODIUM;
    }

    return MAILBOXD_X25519_MONOCYPHER;
}

static mailboxd_random_backend_t parse_random(const char *value)
{
    if (value == NULL || value[0] == '\0' ||
        str_ieq(value, "system") || str_ieq(value, "bundled") ||
        str_ieq(value, "default") || str_ieq(value, "getrandom") ||
        str_ieq(value, "urandom")) {
        return MAILBOXD_RANDOM_SYSTEM;
    }

    if (str_ieq(value, "openssl") || str_ieq(value, "libcrypto")) {
        return MAILBOXD_RANDOM_OPENSSL;
    }

    return MAILBOXD_RANDOM_SYSTEM;
}

static void resolve_backends(mailboxd_crypto_config_t *cfg)
{
#if !defined(MAILBOXD_HAVE_OPENSSL)
    if (cfg->password_hash == MAILBOXD_PASSWORD_HASH_OPENSSL) {
        mailboxd_log_warn("[crypto] password_hash=openssl requested but MailboxD was not "
                       "built with OpenSSL; using tinysha256");
        cfg->password_hash = MAILBOXD_PASSWORD_HASH_TINYSHA256;
    }
    if (cfg->aes_gcm == MAILBOXD_AES_GCM_OPENSSL) {
        mailboxd_log_warn("[crypto] aes_gcm=openssl requested but MailboxD was not built "
                       "with OpenSSL; using tinyaes");
        cfg->aes_gcm = MAILBOXD_AES_GCM_TINYAES;
    }
    if (cfg->random == MAILBOXD_RANDOM_OPENSSL) {
        mailboxd_log_warn("[crypto] random=openssl requested but MailboxD was not built "
                       "with OpenSSL; using system");
        cfg->random = MAILBOXD_RANDOM_SYSTEM;
    }
#endif

#if !defined(MAILBOXD_HAVE_LIBSODIUM)
    if (cfg->chacha == MAILBOXD_CHACHA_LIBSODIUM) {
        mailboxd_log_warn("[crypto] chacha=libsodium requested but MailboxD was not built "
                       "with libsodium; using monocypher");
        cfg->chacha = MAILBOXD_CHACHA_MONOCYPHER;
    }
    if (cfg->x25519 == MAILBOXD_X25519_LIBSODIUM) {
        mailboxd_log_warn("[crypto] x25519=libsodium requested but MailboxD was not built "
                       "with libsodium; using monocypher");
        cfg->x25519 = MAILBOXD_X25519_MONOCYPHER;
    }
#endif
}

void mailboxd_crypto_config_defaults(mailboxd_crypto_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    cfg->password_hash = MAILBOXD_PASSWORD_HASH_TINYSHA256;
    cfg->aes_gcm = MAILBOXD_AES_GCM_TINYAES;
    cfg->chacha = MAILBOXD_CHACHA_MONOCYPHER;
    cfg->x25519 = MAILBOXD_X25519_MONOCYPHER;
    cfg->random = MAILBOXD_RANDOM_SYSTEM;
}

void mailboxd_crypto_config_apply(const mailboxd_config_t *config)
{
    const char *value;

    mailboxd_crypto_config_defaults(&g_crypto_config);

    if (config != NULL) {
        value = mailboxd_config_get(config, "crypto", "password_hash", NULL);
        g_crypto_config.password_hash = parse_password_hash(value);

        value = mailboxd_config_get(config, "crypto", "aes_gcm", NULL);
        g_crypto_config.aes_gcm = parse_aes_gcm(value);

        value = mailboxd_config_get(config, "crypto", "chacha", NULL);
        g_crypto_config.chacha = parse_chacha(value);

        value = mailboxd_config_get(config, "crypto", "x25519", NULL);
        g_crypto_config.x25519 = parse_x25519(value);

        value = mailboxd_config_get(config, "crypto", "random", NULL);
        g_crypto_config.random = parse_random(value);
    }

    resolve_backends(&g_crypto_config);
    g_crypto_config_ready = 1;

    mailboxd_log_info("[crypto] password_hash=%s aes_gcm=%s chacha=%s x25519=%s random=%s",
           mailboxd_password_hash_backend_name(g_crypto_config.password_hash),
           mailboxd_aes_gcm_backend_name(g_crypto_config.aes_gcm),
           mailboxd_chacha_backend_name(g_crypto_config.chacha),
           mailboxd_x25519_backend_name(g_crypto_config.x25519),
           mailboxd_random_backend_name(g_crypto_config.random));
}

const mailboxd_crypto_config_t *mailboxd_crypto_config_get(void)
{
    if (!g_crypto_config_ready) {
        mailboxd_crypto_config_defaults(&g_crypto_config);
        g_crypto_config_ready = 1;
    }

    return &g_crypto_config;
}

const char *mailboxd_password_hash_backend_name(mailboxd_password_hash_backend_t b)
{
    switch (b) {
    case MAILBOXD_PASSWORD_HASH_OPENSSL:
        return "openssl";
    default:
        return "tinysha256";
    }
}

const char *mailboxd_aes_gcm_backend_name(mailboxd_aes_gcm_backend_t b)
{
    switch (b) {
    case MAILBOXD_AES_GCM_OPENSSL:
        return "openssl";
    default:
        return "tinyaes";
    }
}

const char *mailboxd_chacha_backend_name(mailboxd_chacha_backend_t b)
{
    switch (b) {
    case MAILBOXD_CHACHA_LIBSODIUM:
        return "libsodium";
    default:
        return "monocypher";
    }
}

const char *mailboxd_x25519_backend_name(mailboxd_x25519_backend_t b)
{
    switch (b) {
    case MAILBOXD_X25519_LIBSODIUM:
        return "libsodium";
    default:
        return "monocypher";
    }
}

const char *mailboxd_random_backend_name(mailboxd_random_backend_t b)
{
    switch (b) {
    case MAILBOXD_RANDOM_OPENSSL:
        return "openssl";
    default:
        return "system";
    }
}
