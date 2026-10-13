/*
 * Entertain plugin — c4.c
 * Connect Four board, drop logic, win/draw detection, tick.
 * V3 convention: X = uppercase (Rot), o = lowercase (Gelb).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "entertain_c4.h"
#include "mailboxd/session.h"
#include "mailboxd/log.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static c4_game_t g_games[C4_GAME_MAX];
static unsigned g_max_games = C4_DEFAULT_MAX;

/* ── Board init ────────────────────────────────────────────── */

static void c4_board_init(char board[C4_ROWS][C4_COLS])
{
    unsigned r, c;

    for (r = 0; r < C4_ROWS; r++) {
        for (c = 0; c < C4_COLS; c++) {
            board[r][c] = '.';
        }
    }
}

/* ── Lifecycle ─────────────────────────────────────────────── */

void c4_init(unsigned max_games)
{
    unsigned i;

    memset(g_games, 0, sizeof(g_games));
    if (max_games == 0 || max_games > C4_GAME_MAX) {
        max_games = C4_DEFAULT_MAX;
    }
    g_max_games = max_games;
    for (i = 0; i < C4_GAME_MAX; i++) {
        snprintf(g_games[i].game_id, sizeof(g_games[i].game_id), "C%u", i + 1);
        g_games[i].active = 0;
    }
}

void c4_shutdown(void)
{
    unsigned i;

    for (i = 0; i < C4_GAME_MAX; i++) {
        g_games[i].active = 0;
        g_games[i].red_session = NULL;
        g_games[i].yellow_session = NULL;
        g_games[i].spectator_count = 0;
    }
}

unsigned c4_max_games(void)
{
    return g_max_games;
}

/* ── Game management ───────────────────────────────────────── */

c4_game_t *c4_get_game(unsigned index)
{
    if (index >= g_max_games) {
        return NULL;
    }
    return &g_games[index];
}

c4_game_t *c4_find_game_by_id(const char *id)
{
    unsigned i;

    if (id == NULL) {
        return NULL;
    }
    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active &&
            strcasecmp(g_games[i].game_id, id) == 0) {
            return &g_games[i];
        }
    }
    return NULL;
}

c4_game_t *c4_find_open_game(void)
{
    unsigned i;

    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active && g_games[i].yellow_session == NULL &&
            g_games[i].result == C4_RESULT_NONE) {
            return &g_games[i];
        }
    }
    return NULL;
}

c4_game_t *c4_find_player_game(const struct mailboxd_session *s)
{
    unsigned i;

    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active &&
            (g_games[i].red_session == s || g_games[i].yellow_session == s)) {
            return &g_games[i];
        }
    }
    return NULL;
}

int c4_is_player(const c4_game_t *g, const struct mailboxd_session *s)
{
    return g != NULL && (g->red_session == s || g->yellow_session == s);
}

int c4_is_spectator(const c4_game_t *g, const struct mailboxd_session *s)
{
    unsigned j;

    if (g == NULL) {
        return 0;
    }
    for (j = 0; j < g->spectator_count; j++) {
        if (g->spectators[j] == s) {
            return 1;
        }
    }
    return 0;
}

int c4_add_spectator(c4_game_t *g, struct mailboxd_session *spec)
{
    if (g == NULL || spec == NULL) {
        return 0;
    }
    if (c4_is_player(g, spec) || c4_is_spectator(g, spec)) {
        return 0;
    }
    if (g->spectator_count >= C4_SPECTATOR_MAX) {
        return 0;
    }
    g->spectators[g->spectator_count++] = spec;
    return 1;
}

void c4_remove_spectator(c4_game_t *g, struct mailboxd_session *spec)
{
    unsigned j;

    if (g == NULL || spec == NULL) {
        return;
    }
    for (j = 0; j < g->spectator_count; j++) {
        if (g->spectators[j] == spec) {
            g->spectators[j] = g->spectators[g->spectator_count - 1];
            g->spectator_count--;
            return;
        }
    }
}

void c4_release_game(c4_game_t *g)
{
    char id[8];

    if (g == NULL) {
        return;
    }
    mailboxd_strlcpy(id, g->game_id, sizeof(id));
    memset(g, 0, sizeof(*g));
    mailboxd_strlcpy(g->game_id, id, sizeof(g->game_id));
    g->active = 0;
}

unsigned c4_clear_finished(void)
{
    unsigned i;
    unsigned n = 0;

    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active && g_games[i].result != C4_RESULT_NONE) {
            c4_release_game(&g_games[i]);
            n++;
        }
    }
    return n;
}

c4_game_t *c4_create_game(struct mailboxd_session *red, const char *name)
{
    unsigned i;
    c4_game_t *slot = NULL;

    for (i = 0; i < g_max_games; i++) {
        if (!g_games[i].active) {
            slot = &g_games[i];
            break;
        }
        if (g_games[i].result != C4_RESULT_NONE) {
            slot = &g_games[i];
            break;
        }
    }
    if (slot == NULL) {
        return NULL;
    }

    {
        char id[8];

        mailboxd_strlcpy(id, slot->game_id, sizeof(id));
        memset(slot, 0, sizeof(*slot));
        mailboxd_strlcpy(slot->game_id, id, sizeof(slot->game_id));
    }
    slot->active = 1;
    mailboxd_strlcpy(slot->red_name, name != NULL ? name : "?", C4_NAME_MAX);
    slot->red_session = red;
    c4_board_init(slot->board);
    slot->turn = C4_TURN_RED;
    mailboxd_log_info("[c4] game %s created by %s", slot->game_id,
                      name != NULL ? name : "?");
    return slot;
}

int c4_join_game(c4_game_t *g, struct mailboxd_session *yellow, const char *name)
{
    if (g == NULL || !g->active || g->yellow_session != NULL ||
        g->result != C4_RESULT_NONE || yellow == NULL) {
        return 0;
    }
    mailboxd_strlcpy(g->yellow_name, name != NULL ? name : "?", C4_NAME_MAX);
    g->yellow_session = yellow;
    mailboxd_log_info("[c4] %s joined %s as Yellow", g->yellow_name, g->game_id);
    return 1;
}

/* ── Win detection (4 in a row) ────────────────────────────── */

static int check_four(char a, char b, char c, char d)
{
    return a != '.' && a == b && a == c && a == d;
}

static c4_result_t check_winner(const char board[C4_ROWS][C4_COLS])
{
    unsigned r, c;

    /* Horizontal */
    for (r = 0; r < C4_ROWS; r++) {
        for (c = 0; c <= C4_COLS - 4; c++) {
            if (check_four(board[r][c], board[r][c + 1],
                           board[r][c + 2], board[r][c + 3])) {
                return board[r][c] == 'X' ? C4_RED_WINS : C4_YELLOW_WINS;
            }
        }
    }
    /* Vertical */
    for (c = 0; c < C4_COLS; c++) {
        for (r = 0; r <= C4_ROWS - 4; r++) {
            if (check_four(board[r][c], board[r + 1][c],
                           board[r + 2][c], board[r + 3][c])) {
                return board[r][c] == 'X' ? C4_RED_WINS : C4_YELLOW_WINS;
            }
        }
    }
    /* Diagonal down-right */
    for (r = 0; r <= C4_ROWS - 4; r++) {
        for (c = 0; c <= C4_COLS - 4; c++) {
            if (check_four(board[r][c], board[r + 1][c + 1],
                           board[r + 2][c + 2], board[r + 3][c + 3])) {
                return board[r][c] == 'X' ? C4_RED_WINS : C4_YELLOW_WINS;
            }
        }
    }
    /* Diagonal down-left */
    for (r = 0; r <= C4_ROWS - 4; r++) {
        for (c = 3; c < C4_COLS; c++) {
            if (check_four(board[r][c], board[r + 1][c - 1],
                           board[r + 2][c - 2], board[r + 3][c - 3])) {
                return board[r][c] == 'X' ? C4_RED_WINS : C4_YELLOW_WINS;
            }
        }
    }
    return C4_RESULT_NONE;
}

/* ── Moves ─────────────────────────────────────────────────── */

int c4_make_move(c4_game_t *g, unsigned col,
                 char *err, size_t errlen)
{
    unsigned row;
    char ch;

    if (g == NULL || g->result != C4_RESULT_NONE) {
        if (err != NULL && errlen > 0) {
            mailboxd_strlcpy(err, "Game already finished.", errlen);
        }
        return 0;
    }
    if (col >= C4_COLS) {
        if (err != NULL && errlen > 0) {
            mailboxd_strlcpy(err, "Column must be 1-7.", errlen);
        }
        return 0;
    }

    /* Find lowest empty row in this column */
    row = C4_ROWS;
    {
        unsigned r;

        for (r = 0; r < C4_ROWS; r++) {
            if (g->board[r][col] == '.') {
                row = r;
            }
        }
    }
    if (row >= C4_ROWS) {
        if (err != NULL && errlen > 0) {
            mailboxd_strlcpy(err, "Column is full.", errlen);
        }
        return 0;
    }

    ch = (g->turn == C4_TURN_RED) ? 'X' : 'o';
    g->board[row][col] = ch;
    g->move_count++;

    g->result = check_winner(g->board);
    if (g->result == C4_RESULT_NONE && g->move_count >= C4_ROWS * C4_COLS) {
        g->result = C4_DRAW;
    }

    g->turn = (g->turn == C4_TURN_RED) ? C4_TURN_YELLOW : C4_TURN_RED;
    g->tick_counter = 0;
    return 1;
}

void c4_resign(c4_game_t *g, struct mailboxd_session *who)
{
    if (g == NULL || g->result != C4_RESULT_NONE) {
        return;
    }
    if (g->red_session == who) {
        g->result = C4_YELLOW_WINS;
    } else if (g->yellow_session == who) {
        g->result = C4_RED_WINS;
    }
}

/* ── Board rendering ───────────────────────────────────────── */

static void render_board(const char board[C4_ROWS][C4_COLS],
                         struct mailboxd_session *s, const char *label)
{
    char line[96];
    unsigned r;

    if (label != NULL && label[0] != '\0') {
        mailboxd_session_write_line(s, label);
    }
    mailboxd_session_write_line(s, "  1 2 3 4 5 6 7");
    for (r = 0; r < C4_ROWS; r++) {
        unsigned rk = C4_ROWS - 1 - r;
        int n = snprintf(line, sizeof(line), "%u ", rk + 1);
        unsigned c;

        for (c = 0; c < C4_COLS; c++) {
            n += snprintf(line + (size_t)n, sizeof(line) - (size_t)n,
                          "%c ", board[rk][c]);
        }
        n += snprintf(line + (size_t)n, sizeof(line) - (size_t)n,
                      "%u", rk + 1);
        mailboxd_session_write_line(s, line);
    }
    mailboxd_session_write_line(s, "  1 2 3 4 5 6 7");
    mailboxd_session_write_line(s, "  (X=GROSS  o=klein)");
}

void c4_send_board(c4_game_t *g, struct mailboxd_session *s)
{
    char label[192];
    const char *turn_name;

    if (g == NULL || s == NULL) {
        return;
    }

    if (g->result != C4_RESULT_NONE) {
        const char *rs;

        switch (g->result) {
        case C4_RED_WINS:
            rs = "1-0 Red (X) wins";
            break;
        case C4_YELLOW_WINS:
            rs = "0-1 Yellow (o) wins";
            break;
        case C4_DRAW:
            rs = "1/2-1/2 Draw";
            break;
        default:
            rs = "Over";
            break;
        }
        snprintf(label, sizeof(label), "[%s] %s(X) vs %s(o)  %s  Move %u",
                 g->game_id, g->red_name,
                 g->yellow_session ? g->yellow_name : "...", rs, g->move_count);
    } else {
        turn_name = (g->turn == C4_TURN_RED) ? g->red_name : g->yellow_name;
        snprintf(label, sizeof(label),
                 "[%s] %s(X) vs %s(o)  %s to drop  Move %u",
                 g->game_id, g->red_name,
                 g->yellow_session ? g->yellow_name : "...", turn_name,
                 g->move_count);
    }
    render_board(g->board, s, label);
}

void c4_broadcast_board(c4_game_t *g)
{
    unsigned j;

    if (g == NULL) {
        return;
    }
    if (g->red_session != NULL) {
        c4_send_board(g, g->red_session);
    }
    if (g->yellow_session != NULL) {
        c4_send_board(g, g->yellow_session);
    }
    for (j = 0; j < g->spectator_count; j++) {
        c4_send_board(g, g->spectators[j]);
    }
}

/* ── Tick ──────────────────────────────────────────────────── */

void c4_tick(struct mailboxd_service *service)
{
    unsigned i;

    (void)service;
    for (i = 0; i < g_max_games; i++) {
        c4_game_t *g = &g_games[i];
        unsigned j;

        if (!g->active || g->spectator_count == 0) {
            continue;
        }
        if (g->yellow_session == NULL || g->result != C4_RESULT_NONE) {
            continue;
        }
        g->tick_counter++;
        if (g->tick_counter < C4_REFRESH_SEC) {
            continue;
        }
        g->tick_counter = 0;
        for (j = 0; j < g->spectator_count; j++) {
            c4_send_board(g, g->spectators[j]);
        }
    }
}