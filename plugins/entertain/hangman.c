/*
 * Entertain plugin — hangman.c
 * Hangman game logic, word list, gallows rendering.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "entertain_hangman.h"
#include "mailboxd/session.h"
#include "mailboxd/log.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

/* ── Word list ─────────────────────────────────────────────── */

static const char *const g_words[] = {
    /* Easy */
    "RADIO", "WATER", "HOUSE", "MUSIC", "LIGHT", "SMILE",
    "STONE", "TRAIN", "WORLD", "CLOUD", "PAPER", "OCEAN",
    /* Medium */
    "SIGNAL", "PACKET", "BEACON", "CASTLE", "FOREST", "GUITAR",
    "ISLAND", "ROCKET", "SILVER", "WINTER", "CAMERA", "BRIDGE",
    /* Hard (fair) */
    "GALAXY", "OXYGEN", "JACKET", "JUNGLE", "KAYAK", "VORTEX",
    "BOXING", "PUZZLE", "FROZEN", "WHISKY", "EXPERT", "QUARTZ"
};

#define WORD_COUNT (sizeof(g_words) / sizeof(g_words[0]))

/* ── Gallows art ───────────────────────────────────────────── */

static const char *const g_gallows[HANGMAN_MAX_WRONG + 1][HANGMAN_GALLOWS_ROWS] = {
    /* 0 wrong — Boden + Galgenstummel */
    {
        "    |",
        "    |",
        "",
        "",
        "",
        "____|______________"
    },
    /* 1 — Pfahl komplett */
    {
        "    |",
        "    |",
        "    |",
        "    |",
        "",
        "____|______________"
    },
    /* 2 — Querbalken */
    {
        "    +---------+",
        "    |",
        "    |",
        "    |",
        "",
        "____|______________"
    },
    /* 3 — Seil */
    {
        "    +---------+",
        "    |         |",
        "    |",
        "    |",
        "",
        "____|______________"
    },
    /* 4 — Kopf */
    {
        "    +---------+",
        "    |         |",
        "    |         O",
        "    |",
        "",
        "____|______________"
    },
    /* 5 — Rumpf */
    {
        "    +---------+",
        "    |         |",
        "    |         O",
        "    |         |",
        "",
        "____|______________"
    },
    /* 6 — Arme */
    {
        "    +---------+",
        "    |         |",
        "    |         O",
        "    |        /|\\",
        "",
        "____|______________"
    },
    /* 7 — Beine (komplett = tot) */
    {
        "    +---------+",
        "    |         |",
        "    |         O",
        "    |        /|\\",
        "    |        / \\",
        "____|______________"
    }
};

/* ── State ─────────────────────────────────────────────────── */

static hangman_game_t g_games[HANGMAN_SESSION_MAX];
static unsigned g_max_games = HANGMAN_DEFAULT_MAX;
static unsigned g_word_seed = 0;

/* ── Lifecycle ─────────────────────────────────────────────── */

void hangman_init(unsigned max_games)
{
    memset(g_games, 0, sizeof(g_games));
    if (max_games == 0 || max_games > HANGMAN_SESSION_MAX) {
        max_games = HANGMAN_DEFAULT_MAX;
    }
    g_max_games = max_games;
    g_word_seed = (unsigned)time(NULL);
}

void hangman_shutdown(void)
{
    unsigned i;

    for (i = 0; i < g_max_games; i++) {
        g_games[i].active = 0;
        g_games[i].session = NULL;
    }
}

unsigned hangman_max_games(void)
{
    return g_max_games;
}

/* ── Game management ───────────────────────────────────────── */

hangman_game_t *hangman_get_game(unsigned index)
{
    if (index >= g_max_games) {
        return NULL;
    }
    return &g_games[index];
}

hangman_game_t *hangman_find_player_game(const struct mailboxd_session *s)
{
    unsigned i;

    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active && g_games[i].session == s) {
            return &g_games[i];
        }
    }
    return NULL;
}

void hangman_release_game(hangman_game_t *g)
{
    if (g == NULL) {
        return;
    }
    g->active = 0;
    g->session = NULL;
}

unsigned hangman_clear_finished(void)
{
    unsigned i;
    unsigned n = 0;

    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active && g_games[i].result != HANGMAN_PLAYING) {
            hangman_release_game(&g_games[i]);
            n++;
        }
    }
    return n;
}

hangman_game_t *hangman_create_game(struct mailboxd_session *s)
{
    unsigned i;
    hangman_game_t *slot = NULL;
    const char *word;
    size_t len;

    for (i = 0; i < g_max_games; i++) {
        if (!g_games[i].active) {
            slot = &g_games[i];
            break;
        }
        if (g_games[i].result != HANGMAN_PLAYING) {
            slot = &g_games[i];
            break;
        }
    }
    if (slot == NULL) {
        return NULL;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    slot->session = s;
    snprintf(slot->game_id, sizeof(slot->game_id), "H%u",
             (unsigned)(slot - g_games) + 1);

    word = g_words[g_word_seed % WORD_COUNT];
    g_word_seed++;
    mailboxd_strlcpy(slot->word, word, sizeof(slot->word));
    len = strlen(word);

    /* Initialize display: all underscores */
    for (i = 0; i < (unsigned)len; i++) {
        slot->display[i] = '_';
    }
    slot->display[len] = '\0';

    mailboxd_log_info("[hangman] game %s created by %s (%u letters)",
                      slot->game_id,
                      mailboxd_session_username(s)
                          ? mailboxd_session_username(s) : "?",
                      (unsigned)len);
    return slot;
}

/* ── Gameplay ──────────────────────────────────────────────── */

int hangman_guess_letter(hangman_game_t *g, char letter)
{
    size_t len;
    size_t i;
    int found = 0;

    if (g == NULL || g->result != HANGMAN_PLAYING) {
        return 0;
    }

    letter = (char)toupper((unsigned char)letter);
    if (letter < 'A' || letter > 'Z') {
        return -1;
    }

    /* Already guessed? */
    {
        const char *p;

        for (p = g->display; *p; p++) {
            if (*p == letter) {
                return -2;
            }
        }
        for (p = g->wrong; *p; p++) {
            if (*p == letter) {
                return -2;
            }
        }
    }

    len = strlen(g->word);
    for (i = 0; i < len; i++) {
        if (g->word[i] == letter) {
            g->display[i] = letter;
            found = 1;
        }
    }

    if (!found) {
        if (g->wrong_count < HANGMAN_WRONG_MAX) {
            g->wrong[g->wrong_count] = letter;
            g->wrong_count++;
            g->wrong[g->wrong_count] = '\0';
        }
    }

    /* Check win */
    if (strcmp(g->display, g->word) == 0) {
        g->result = HANGMAN_WON;
    } else if (g->wrong_count >= HANGMAN_MAX_WRONG) {
        g->result = HANGMAN_LOST;
        /* Reveal the word */
        mailboxd_strlcpy(g->display, g->word, sizeof(g->display));
    }

    return found ? 1 : 0;
}

int hangman_guess_word(hangman_game_t *g, const char *word)
{
    char upper[HANGMAN_WORD_MAX];
    size_t i;
    size_t len;

    if (g == NULL || g->result != HANGMAN_PLAYING || word == NULL) {
        return 0;
    }

    len = strlen(word);
    if (len != strlen(g->word)) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        upper[i] = (char)toupper((unsigned char)word[i]);
    }
    upper[len] = '\0';

    if (strcmp(upper, g->word) == 0) {
        mailboxd_strlcpy(g->display, g->word, sizeof(g->display));
        g->result = HANGMAN_WON;
        return 1;
    }

    /* Wrong word guess = one more wrong letter */
    if (g->wrong_count < HANGMAN_WRONG_MAX) {
        g->wrong[g->wrong_count] = '?';
        g->wrong_count++;
        g->wrong[g->wrong_count] = '\0';
    }
    if (g->wrong_count >= HANGMAN_MAX_WRONG) {
        g->result = HANGMAN_LOST;
        mailboxd_strlcpy(g->display, g->word, sizeof(g->display));
    }
    return 0;
}

/* ── Board rendering ───────────────────────────────────────── */

static void format_word(const char *display, char *out, size_t outlen)
{
    size_t i;
    size_t len = strlen(display);
    size_t n = 0;

    for (i = 0; i < len && n + 3 < outlen; i++) {
        if (i > 0) {
            out[n++] = ' ';
        }
        out[n++] = display[i];
    }
    out[n] = '\0';
}

static void format_wrong(const char *wrong, char *out, size_t outlen)
{
    size_t i;
    size_t len = strlen(wrong);
    size_t n = 0;

    for (i = 0; i < len && n + 3 < outlen; i++) {
        if (i > 0) {
            out[n++] = ' ';
        }
        out[n++] = wrong[i];
    }
    out[n] = '\0';
}

void hangman_send_board(hangman_game_t *g, struct mailboxd_session *s)
{
    char line[128];
    char word_str[96];
    char wrong_str[64];
    unsigned stage;
    unsigned i;

    if (g == NULL || s == NULL) {
        return;
    }

    /* Label */
    if (g->result == HANGMAN_WON) {
        snprintf(line, sizeof(line), "[%s] Word: %s  — You win! (%u wrong)",
                 g->game_id, g->word, g->wrong_count);
    } else if (g->result == HANGMAN_LOST) {
        snprintf(line, sizeof(line), "[%s] Word: %s  — HANGED! (%u wrong)",
                 g->game_id, g->word, g->wrong_count);
    } else {
        snprintf(line, sizeof(line), "[%s]  %u / %u wrong",
                 g->game_id, g->wrong_count, HANGMAN_MAX_WRONG);
    }
    mailboxd_session_write_line(s, line);

    /* Gallows */
    stage = g->wrong_count;
    if (stage > HANGMAN_MAX_WRONG) {
        stage = HANGMAN_MAX_WRONG;
    }
    for (i = 0; i < HANGMAN_GALLOWS_ROWS; i++) {
        mailboxd_session_write_line(s, g_gallows[stage][i]);
    }

    /* Word */
    format_word(g->display, word_str, sizeof(word_str));
    snprintf(line, sizeof(line), "  Word:  %s", word_str);
    mailboxd_session_write_line(s, line);

    /* Wrong letters */
    if (g->wrong_count > 0) {
        format_wrong(g->wrong, wrong_str, sizeof(wrong_str));
        snprintf(line, sizeof(line), "  Wrong: %s", wrong_str);
        mailboxd_session_write_line(s, line);
    } else {
        mailboxd_session_write_line(s, "  Wrong: -");
    }

    if (g->result == HANGMAN_PLAYING) {
        snprintf(line, sizeof(line), "  (%u wrong = hanged)",
                 HANGMAN_MAX_WRONG);
        mailboxd_session_write_line(s, line);
    }
}

/* ── Tick ──────────────────────────────────────────────────── */

void hangman_tick(struct mailboxd_service *service)
{
    (void)service;
    /* Single-player — no spectator refresh needed. */
}