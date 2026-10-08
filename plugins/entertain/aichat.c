/*
 * Entertain plugin — aichat.c
 * /aichat · /ai · /ask — OpenRouter AI chat for MailboxD Entertain area.
 *
 * Spec: 0-RESEARCHES/projects/mailboxd/2026-08-09-mailboxd-aichat-openrouter-specification.md
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L

#include "entertain_aichat.h"
#include "mailboxd/config.h"
#include "mailboxd/session.h"
#include "mailboxd/service.h"
#include "mailboxd/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>

static entertain_aichat_config_t         g_cfg;
static entertain_aichat_user_rate_t      g_rates[AICHAT_MAX_USERS];
static size_t                            g_rate_count = 0;

/* ── helpers ────────────────────────────────────────────── */

static int ieq(const char *a, const char *b)
{
    while (a && b && *a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return a && b && *a == *b;
}

static void json_escape(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    if (out_sz == 0) return;
    for (; in && *in && j + 2 < out_sz; in++) {
        if (*in == '"' || *in == '\\') { out[j++] = '\\'; }
        if (*in == '\n' || *in == '\r') { out[j++] = ' '; continue; }
        out[j++] = *in;
    }
    out[j] = '\0';
}

/* Strip non-ASCII bytes (Umlaute etc.) to '?' — prevents parse/display issues */
static void ascii_sanitize(char *s)
{
    for (; s && *s; s++) {
        if ((unsigned char)*s > 127) {
            *s = '?';
        }
    }
}

static int parse_content(const char *json, char *out, size_t out_sz)
{
    const char *p;
    size_t j = 0;
    int esc = 0;

    if (!json || !out || out_sz == 0) return -1;
    out[0] = '\0';

    /* Try error object first */
    p = strstr(json, "\"error\"");
    if (p != NULL) {
        const char *msg = strstr(p, "\"message\"");
        if (msg) {
            msg = strchr(msg, ':');
            if (msg) {
                while (*msg == ':' || *msg == ' ' || *msg == '"') msg++;
                snprintf(out, out_sz, "[AI Error] %.*s", (int)(out_sz - 16), msg);
                size_t len = strlen(out);
                while (len > 0 && (out[len-1] == '"' || out[len-1] == '}')) out[--len] = '\0';
                return 0;
            }
        }
        snprintf(out, out_sz, "[AI Error] OpenRouter returned an error.");
        return 0;
    }

    /* Find "content":"..." */
    p = strstr(json, "\"content\"");
    if (!p) {
        snprintf(out, out_sz, "[AI Error] No content in AI response.");
        return -1;
    }
    p = strchr(p, ':');
    if (!p) return -1;
    while (*p && *p != '"') p++;
    if (*p == '"') p++;

    while (*p && j + 1 < out_sz) {
        if (esc) {
            switch (*p) {
                case 'n':  out[j++] = '\n'; break;
                case 't':  out[j++] = ' ';  break;
                case 'r':  /* skip */        break;
                case '\\': out[j++] = '\\'; break;
                case '"':  out[j++] = '"';  break;
                default:   out[j++] = *p;   break;
            }
            esc = 0;
        } else if (*p == '\\') {
            esc = 1;
        } else if (*p == '"') {
            break;
        } else {
            out[j++] = *p;
        }
        p++;
    }
    out[j] = '\0';
    ascii_sanitize(out);
    return 0;
}

/* ── rate limiter ────────────────────────────────────────── */

static int rate_limit_ok(const char *user, unsigned max_per_hour)
{
    time_t now = time(NULL);
    int idx = -1;
    size_t i;

    for (i = 0; i < g_rate_count; i++) {
        if (strcmp(g_rates[i].username, user) == 0) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        if (g_rate_count < AICHAT_MAX_USERS) {
            idx = (int)g_rate_count++;
        } else {
            idx = 0;
        }
        snprintf(g_rates[idx].username, sizeof(g_rates[idx].username), "%s", user);
        g_rates[idx].window_start = (long)now;
        g_rates[idx].request_count = 0;
    }

    if (now - (time_t)g_rates[idx].window_start >= 3600) {
        g_rates[idx].window_start = (long)now;
        g_rates[idx].request_count = 0;
    }

    if (g_rates[idx].request_count >= max_per_hour) {
        return 0;
    }
    g_rates[idx].request_count++;
    return 1;
}

/* ── 78-col ASCII wrap ───────────────────────────────────── */

static void send_wrapped(mailboxd_session_t *session, const char *text)
{
    char line[84];
    size_t col = 0;
    const char *p = text;

    mailboxd_session_write_line(session, "--- AI Assistant (OpenRouter) ---");

    while (*p) {
        if (*p == '\n') {
            line[col] = '\0';
            mailboxd_session_write_line(session, line);
            col = 0;
            p++;
            continue;
        }
        line[col++] = *p++;
        if (col >= AICHAT_WRAP_COL) {
            size_t bp = col;
            while (bp > 40 && line[bp - 1] != ' ') bp--;
            if (bp <= 40) bp = col;
            char saved = line[bp];
            line[bp] = '\0';
            mailboxd_session_write_line(session, line);
            size_t rem = col - bp;
            if (saved == ' ') {
                if (rem > 0) rem--;
                memmove(line, line + bp + 1, rem);
            } else {
                memmove(line, line + bp, rem);
            }
            col = rem;
        }
    }
    if (col > 0) {
        line[col] = '\0';
        mailboxd_session_write_line(session, line);
    }
    mailboxd_session_write_line(session, "---------------------------------");
}

/* ── lifecycle ────────────────────────────────────────────── */

void aichat_init(const mailboxd_config_t *config)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    memset(&g_rates, 0, sizeof(g_rates));
    g_rate_count = 0;

    if (!config) return;

    g_cfg.enabled = mailboxd_config_get_bool(config, "aichat", "enabled", 0);

    const char *v;
    v = mailboxd_config_get(config, "aichat", "provider", "openrouter");
    snprintf(g_cfg.provider, sizeof(g_cfg.provider), "%s", v ? v : "openrouter");

    v = mailboxd_config_get(config, "aichat", "api_key", "");
    snprintf(g_cfg.api_key, sizeof(g_cfg.api_key), "%s", v ? v : "");

    v = mailboxd_config_get(config, "aichat", "api_url",
                         "https://openrouter.ai/api/v1/chat/completions");
    snprintf(g_cfg.api_url, sizeof(g_cfg.api_url), "%s",
             v ? v : "https://openrouter.ai/api/v1/chat/completions");

    v = mailboxd_config_get(config, "aichat", "model", "openrouter/free");
    snprintf(g_cfg.model, sizeof(g_cfg.model), "%s", v ? v : "openrouter/free");

    g_cfg.max_tokens            = mailboxd_config_get_uint(config, "aichat", "max_tokens", 300, 50, 2048);
    g_cfg.temperature           = 0.7; /* default; could add config getter later */
    g_cfg.rate_limit_per_user_hour = mailboxd_config_get_uint(config, "aichat", "rate_limit_per_user_hour", 5, 1, 100);
    g_cfg.timeout_sec           = mailboxd_config_get_uint(config, "aichat", "timeout_sec", 10, 2, 60);

    v = mailboxd_config_get(config, "aichat", "site_url", "https://mailboxd.un1t.me");
    snprintf(g_cfg.site_url, sizeof(g_cfg.site_url), "%s", v ? v : "https://mailboxd.un1t.me");

    v = mailboxd_config_get(config, "aichat", "site_name", "MailboxD Network");
    snprintf(g_cfg.site_name, sizeof(g_cfg.site_name), "%s", v ? v : "MailboxD Network");

    if (g_cfg.enabled) {
        mailboxd_log_info("[aichat] enabled — provider=%s model=%s max_tokens=%u rate=%u/h",
                       g_cfg.provider, g_cfg.model, g_cfg.max_tokens,
                       g_cfg.rate_limit_per_user_hour);
    }
}

void aichat_shutdown(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    memset(&g_rates, 0, sizeof(g_rates));
    g_rate_count = 0;
}

/* ── /aichat command ──────────────────────────────────────── */

mailboxd_result_t aichat_cmd(mailboxd_service_t *service,
                           mailboxd_session_t *session,
                           const mailboxd_parsed_command_t *cmd)
{
    char prompt[1024] = {0};
    char escaped[2048] = {0};
    char json_req[4096] = {0};
    char json_resp[8192] = {0};
    char ai_text[4096] = {0};
    char cmd_buf[8192] = {0};
    char tmp_path[256];
    size_t i;

    (void)service;

    if (!g_cfg.enabled) {
        mailboxd_session_write_line(session,
            "[AI] /aichat is disabled. (Enable with [aichat] enabled=yes)");
        return MAILBOXD_OK;
    }

    /* /aichat help */
    if (cmd->argc == 0 || (cmd->argc == 1 && ieq(cmd->argv[0], "help"))) {
        mailboxd_session_write_line(session, "/aichat — AI assistant (OpenRouter)");
        mailboxd_session_write_line(session, "  /aichat <prompt>     Ask the AI");
        mailboxd_session_write_line(session, "  /ai <prompt>         Alias");
        mailboxd_session_write_line(session, "  /ask <prompt>        Alias");
        mailboxd_session_write_line(session, "");
        mailboxd_session_write_line(session,
            "Notice: Prompts are sent to an external AI service (OpenRouter).");
        mailboxd_session_write_line(session,
            "Do not send private passwords or confidential data.");
        return MAILBOXD_OK;
    }

    /* Reconstruct prompt */
    for (i = 0; i < cmd->argc; i++) {
        if (i > 0) strncat(prompt, " ", sizeof(prompt) - strlen(prompt) - 1);
        strncat(prompt, cmd->argv[i], sizeof(prompt) - strlen(prompt) - 1);
    }

    const char *user = mailboxd_session_username(session);
    if (!user || !*user) user = "Guest";

    if (!rate_limit_ok(user, g_cfg.rate_limit_per_user_hour)) {
        snprintf(ai_text, sizeof(ai_text),
            "[AI] Hourly quota reached (%u/%u). Please try again later.",
            g_cfg.rate_limit_per_user_hour, g_cfg.rate_limit_per_user_hour);
        mailboxd_session_write_line(session, ai_text);
        return MAILBOXD_OK;
    }

    if (g_cfg.api_key[0] == '\0') {
        mailboxd_session_write_line(session,
            "[AI Error] API key missing in [aichat] section of ./local/mailboxd.ini");
        return MAILBOXD_OK;
    }

    mailboxd_session_write_line(session, "[AI] Querying OpenRouter assistant...");

    json_escape(prompt, escaped, sizeof(escaped));

    snprintf(json_req, sizeof(json_req),
        "{\"model\":\"%s\",\"max_tokens\":%u,\"temperature\":%.1f,"
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"You are an AI assistant on a text-based Mailbox (BBX) system. "
        "Reply strictly in plain ASCII text. Keep responses concise, under 78 columns wide, "
        "and without markdown headers, bold, or complex symbols.\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}"
        "]}",
        g_cfg.model, g_cfg.max_tokens, g_cfg.temperature, escaped);

    /* Write request to temp file */
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/mailboxd_aichat_%d_%ld.json",
             (int)getpid(), (long)time(NULL));

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        mailboxd_session_write_line(session, "[AI Error] Cannot write temp request file.");
        return MAILBOXD_OK;
    }
    fputs(json_req, fp);
    fclose(fp);

    snprintf(cmd_buf, sizeof(cmd_buf),
        "curl -s -m %u -X POST \"%s\" "
        "-H \"Authorization: Bearer %s\" "
        "-H \"HTTP-Referer: %s\" "
        "-H \"X-Title: %s\" "
        "-H \"Content-Type: application/json\" "
        "-d @\"%s\"",
        g_cfg.timeout_sec, g_cfg.api_url, g_cfg.api_key,
        g_cfg.site_url, g_cfg.site_name, tmp_path);

    FILE *pipe = popen(cmd_buf, "r");
    if (!pipe) {
        unlink(tmp_path);
        mailboxd_session_write_line(session, "[AI Error] Cannot launch HTTP request.");
        return MAILBOXD_OK;
    }

    size_t n = fread(json_resp, 1, sizeof(json_resp) - 1, pipe);
    json_resp[n] = '\0';
    int curl_rc = pclose(pipe);
    unlink(tmp_path);

    if (n == 0) {
        mailboxd_session_write_line(session, "[AI Error] AI backend timed out.");
        return MAILBOXD_OK;
    }

    /* HTTP 401 check (curl returns exit code != 0 for auth failures) */
    if (strstr(json_resp, "\"code\":401") != NULL ||
        strstr(json_resp, "\"status\":401") != NULL) {
        mailboxd_session_write_line(session,
            "[AI Error] API key invalid. Check [aichat] api_key in ./local/mailboxd.ini");
        mailboxd_log_warn("[aichat] Invalid API key for user %s", user);
        return MAILBOXD_OK;
    }

    /* HTTP 429 check */
    if (strstr(json_resp, "\"code\":429") != NULL ||
        strstr(json_resp, "\"status\":429") != NULL) {
        mailboxd_session_write_line(session,
            "[AI] Cloud AI quota temporarily busy. Try again in a few minutes.");
        return MAILBOXD_OK;
    }

    /* HTTP 404 check (model not found) */
    if (strstr(json_resp, "\"code\":404") != NULL ||
        strstr(json_resp, "\"status\":404") != NULL) {
        mailboxd_session_write_line(session,
            "[AI Error] Model not available. Check [aichat] model in ./local/mailboxd.ini");
        mailboxd_log_warn("[aichat] Model %s returned 404", g_cfg.model);
        return MAILBOXD_OK;
    }

    if (parse_content(json_resp, ai_text, sizeof(ai_text)) != 0) {
        mailboxd_session_write_line(session, "[AI Error] Failed to parse AI response content.");
        return MAILBOXD_OK;
    }

    send_wrapped(session, ai_text);
    return MAILBOXD_OK;
}
