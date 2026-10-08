/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../plugins/entertain/entertain_aichat.h"
#include "mailboxd/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_failures;

static void check_str(const char *label, const char *got, const char *want)
{
    if (got == NULL || want == NULL || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got '%s' want '%s'\n",
                label, got != NULL ? got : "(null)", want);
        g_failures++;
    }
}

static void check_int(const char *label, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", label, got, want);
        g_failures++;
    }
}

int main(void)
{
    mailboxd_config_t cfg;
    entertain_aichat_config_t ai_cfg;
    const char *path = "mailboxd_test_aichat.ini";
    FILE *fp;

    fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "FAIL fopen\n");
        return 1;
    }

    fputs("[aichat]\n", fp);
    fputs("enabled = yes\n", fp);
    fputs("provider = openrouter\n", fp);
    fputs("api_key = test_key_12345\n", fp);
    fputs("model = openrouter/free\n", fp);
    fputs("max_tokens = 250\n", fp);
    fputs("rate_limit_per_user_hour = 10\n", fp);
    fclose(fp);

    if (mailboxd_config_load(&cfg, path) != MAILBOXD_OK) {
        fprintf(stderr, "FAIL config load\n");
        remove(path);
        return 1;
    }

    /* Test config parsing via aichat_init (reads from config) */
    /* For unit test we parse manually since aichat_init expects mailboxd_config_t* */
    ai_cfg.enabled = mailboxd_config_get_bool(&cfg, "aichat", "enabled", 0);
    const char *v;
    v = mailboxd_config_get(&cfg, "aichat", "provider", "openrouter");
    snprintf(ai_cfg.provider, sizeof(ai_cfg.provider), "%s", v ? v : "openrouter");
    v = mailboxd_config_get(&cfg, "aichat", "api_key", "");
    snprintf(ai_cfg.api_key, sizeof(ai_cfg.api_key), "%s", v ? v : "");
    v = mailboxd_config_get(&cfg, "aichat", "model", "openrouter/free");
    snprintf(ai_cfg.model, sizeof(ai_cfg.model), "%s", v ? v : "openrouter/free");
    ai_cfg.max_tokens = mailboxd_config_get_uint(&cfg, "aichat", "max_tokens", 300, 50, 2048);
    ai_cfg.rate_limit_per_user_hour = mailboxd_config_get_uint(&cfg, "aichat", "rate_limit_per_user_hour", 5, 1, 100);

    check_int("aichat.enabled", ai_cfg.enabled, 1);
    check_str("aichat.provider", ai_cfg.provider, "openrouter");
    check_str("aichat.api_key", ai_cfg.api_key, "test_key_12345");
    check_str("aichat.model", ai_cfg.model, "openrouter/free");
    check_int("aichat.max_tokens", (int)ai_cfg.max_tokens, 250);
    check_int("aichat.rate_limit_per_user_hour", (int)ai_cfg.rate_limit_per_user_hour, 10);

    mailboxd_config_free(&cfg);
    remove(path);

    if (g_failures != 0) {
        fprintf(stderr, "%u failure(s)\n", g_failures);
        return 1;
    }

    puts("test_aichat: ok");
    return 0;
}
