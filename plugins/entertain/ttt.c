/*
 * Entertain plugin — ttt.c
 * Tic-Tac-Toe board, move logic, win/draw detection, tick.
 * V3 convention: X = uppercase (Rot), o = lowercase (Gelb).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "entertain_ttt.h"
#include "mailboxd/session.h"
#include "mailboxd/log.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ttt_game_t g_games[TTT_GAME_MAX];
static unsigned g_max_games = TTT_DEFAULT_MAX;

/* ── Board init ────────────────────────────────────────────── */

static void ttt_board_init(char board[TTT_BOARD_SIZE][TTT_BOARD_SIZE])
{
    unsigned r, c;

    for (r = 0; r < TTT_BOARD_SIZE; r++) {
        for (c = 0; c < TTT_BOARD_SIZE; c++) {
            board[r][c] = '.';
        }
    }
}

/* ── Lifecycle ─────────────────────────────────────────────── */

void ttt_init(unsigned max_games)
{
    unsigned i;

    memset(g_games, 0, sizeof(g_games));
    if (max_games == 0 || max_games > TTT_GAME_MAX) {
        max_games = TTT_DEFAULT_MAX;
    }
    g_max_games = max_games;
    for (i = 0; i < TTT_GAME_MAX; i++) {
        snprintf(g_games[i].game_id, sizeof(g_games[i].game_id), "T%u", i + 1);
        g_games[i].active = 0;
    }
}

void ttt_shutdown(void)
{
    unsigned i;

    for (i = 0; i < TTT_GAME_MAX; i++) {
        g_games[i].active = 0;
        g_games[i].x_session = NULL;
        g_games[i].o_session = NULL;
        g_games[i].spectator_count = 0;
    }
}

unsigned ttt_max_games(void)
{
    return g_max_games;
}

/* ── Game management ───────────────────────────────────────── */

ttt_game_t *ttt_get_game(unsigned index)
{
    if (index >= g_max_games) {
        return NULL;
    }
    return &g_games[index];
}

ttt_game_t *ttt_find_game_by_id(const char *id)
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

ttt_game_t *ttt_find_open_game(void)
{
    unsigned i;

    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active && g_games[i].o_session == NULL &&
            g_games[i].result == TTT_RESULT_NONE) {
            return &g_games[i];
        }
    }
    return NULL;
}

ttt_game_t *ttt_find_player_game(const struct mailboxd_session *s)
{
    unsigned i;

    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active &&
            (g_games[i].x_session == s || g_games[i].o_session == s)) {
            return &g_games[i];
        }
    }
    return NULL;
}

int ttt_is_player(const ttt_game_t *g, const struct mailboxd_session *s)
{
    return g != NULL && (g->x_session == s || g->o_session == s);
}

int ttt_is_spectator(const ttt_game_t *g, const struct mailboxd_session *s)
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

int ttt_add_spectator(ttt_game_t *g, struct mailboxd_session *spec)
{
    if (g == NULL || spec == NULL) {
        return 0;
    }
    if (ttt_is_player(g, spec) || ttt_is_spectator(g, spec)) {
        return 0;
    }
    if (g->spectator_count >= TTT_SPECTATOR_MAX) {
        return 0;
    }
    g->spectators[g->spectator_count++] = spec;
    return 1;
}

void ttt_remove_spectator(ttt_game_t *g, struct mailboxd_session *spec)
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

void ttt_release_game(ttt_game_t *g)
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

unsigned ttt_clear_finished(void)
{
    unsigned i;
    unsigned n = 0;

    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active && g_games[i].result != TTT_RESULT_NONE) {
            ttt_release_game(&g_games[i]);
            n++;
        }
    }
    return n;
}

ttt_game_t *ttt_create_game(struct mailboxd_session *x, const char *name)
{
    unsigned i;
    ttt_game_t *slot = NULL;

    for (i = 0; i < g_max_games; i++) {
        if (!g_games[i].active) {
            slot = &g_games[i];
            break;
        }
        if (g_games[i].result != TTT_RESULT_NONE) {
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
    mailboxd_strlcpy(slot->x_name, name != NULL ? name : "?", TTT_NAME_MAX);
    slot->x_session = x;
    ttt_board_init(slot->board);
    slot->turn = TTT_TURN_X;
    mailboxd_log_info("[ttt] game %s created by %s", slot->game_id,
                      name != NULL ? name : "?");
    return slot;
}

int ttt_join_game(ttt_game_t *g, struct mailboxd_session *o, const char *name)
{
    if (g == NULL || !g->active || g->o_session != NULL ||
        g->result != TTT_RESULT_NONE || o == NULL) {
        return 0;
    }
    mailboxd_strlcpy(g->o_name, name != NULL ? name : "?", TTT_NAME_MAX);
    g->o_session = o;
    mailboxd_log_info("[ttt] %s joined %s as O", g->o_name, g->game_id);
    return 1;
}

/* ── Win/draw detection ────────────────────────────────────── */

static int check_line(const char board[TTT_BOARD_SIZE][TTT_BOARD_SIZE],
                      unsigned r1, unsigned c1, unsigned r2, unsigned c2,
                      unsigned r3, unsigned c3)
{
    char a = board[r1][c1];

    if (a == '.') {
        return 0;
    }
    return a == board[r2][c2] && a == board[r3][c3];
}

static ttt_result_t check_winner(const char board[TTT_BOARD_SIZE][TTT_BOARD_SIZE])
{
    /* Rows */
    if (check_line(board, 0, 0, 0, 1, 0, 2)) {
        return board[0][0] == 'X' ? TTT_X_WINS : TTT_O_WINS;
    }
    if (check_line(board, 1, 0, 1, 1, 1, 2)) {
        return board[1][0] == 'X' ? TTT_X_WINS : TTT_O_WINS;
    }
    if (check_line(board, 2, 0, 2, 1, 2, 2)) {
        return board[2][0] == 'X' ? TTT_X_WINS : TTT_O_WINS;
    }
    /* Columns */
    if (check_line(board, 0, 0, 1, 0, 2, 0)) {
        return board[0][0] == 'X' ? TTT_X_WINS : TTT_O_WINS;
    }
    if (check_line(board, 0, 1, 1, 1, 2, 1)) {
        return board[0][1] == 'X' ? TTT_X_WINS : TTT_O_WINS;
    }
    if (check_line(board, 0, 2, 1, 2, 2, 2)) {
        return board[0][2] == 'X' ? TTT_X_WINS : TTT_O_WINS;
    }
    /* Diagonals */
    if (check_line(board, 0, 0, 1, 1, 2, 2)) {
        return board[0][0] == 'X' ? TTT_X_WINS : TTT_O_WINS;
    }
    if (check_line(board, 0, 2, 1, 1, 2, 0)) {
        return board[0][2] == 'X' ? TTT_X_WINS : TTT_O_WINS;
    }
    return TTT_RESULT_NONE;
}

/* ── Moves ─────────────────────────────────────────────────── */

int ttt_make_move(ttt_game_t *g, unsigned row, unsigned col,
                  char *err, size_t errlen)
{
    char ch;

    if (g == NULL || g->result != TTT_RESULT_NONE) {
        if (err != NULL && errlen > 0) {
            mailboxd_strlcpy(err, "Game already finished.", errlen);
        }
        return 0;
    }
    if (row >= TTT_BOARD_SIZE || col >= TTT_BOARD_SIZE) {
        if (err != NULL && errlen > 0) {
            mailboxd_strlcpy(err, "Row and column must be 1-3.", errlen);
        }
        return 0;
    }
    if (g->board[row][col] != '.') {
        if (err != NULL && errlen > 0) {
            mailboxd_strlcpy(err, "Cell already occupied.", errlen);
        }
        return 0;
    }

    ch = (g->turn == TTT_TURN_X) ? 'X' : 'o';
    g->board[row][col] = ch;
    g->move_count++;

    g->result = check_winner(g->board);
    if (g->result == TTT_RESULT_NONE && g->move_count >= TTT_BOARD_SIZE * TTT_BOARD_SIZE) {
        g->result = TTT_DRAW;
    }

    g->turn = (g->turn == TTT_TURN_X) ? TTT_TURN_O : TTT_TURN_X;
    g->tick_counter = 0;
    return 1;
}

void ttt_resign(ttt_game_t *g, struct mailboxd_session *who)
{
    if (g == NULL || g->result != TTT_RESULT_NONE) {
        return;
    }
    if (g->x_session == who) {
        g->result = TTT_O_WINS;
    } else if (g->o_session == who) {
        g->result = TTT_X_WINS;
    }
}

/* ── Board rendering ───────────────────────────────────────── */

static void render_board(const char board[TTT_BOARD_SIZE][TTT_BOARD_SIZE],
                         struct mailboxd_session *s, const char *label)
{
    unsigned r;

    if (label != NULL && label[0] != '\0') {
        mailboxd_session_write_line(s, label);
    }
    mailboxd_session_write_line(s, "    1   2   3");
    for (r = 0; r < TTT_BOARD_SIZE; r++) {
        unsigned rk = TTT_BOARD_SIZE - 1 - r;
        char line[64];

        snprintf(line, sizeof(line), "  +---+---+---+");
        mailboxd_session_write_line(s, line);
        snprintf(line, sizeof(line), "%u | %c | %c | %c | %u",
                 rk + 1, board[rk][0], board[rk][1], board[rk][2], rk + 1);
        mailboxd_session_write_line(s, line);
    }
    mailboxd_session_write_line(s, "  +---+---+---+");
    mailboxd_session_write_line(s, "    1   2   3");
    mailboxd_session_write_line(s, "  (X=GROSS  o=klein)");
}

void ttt_send_board(ttt_game_t *g, struct mailboxd_session *s)
{
    char label[192];
    const char *turn_name;

    if (g == NULL || s == NULL) {
        return;
    }

    if (g->result != TTT_RESULT_NONE) {
        const char *rs;

        switch (g->result) {
        case TTT_X_WINS:
            rs = "1-0 X wins";
            break;
        case TTT_O_WINS:
            rs = "0-1 o wins";
            break;
        case TTT_DRAW:
            rs = "1/2-1/2 Draw";
            break;
        default:
            rs = "Over";
            break;
        }
        snprintf(label, sizeof(label), "[%s] %s(X) vs %s(o)  %s  Move %u",
                 g->game_id, g->x_name,
                 g->o_session ? g->o_name : "...", rs, g->move_count);
    } else {
        turn_name = (g->turn == TTT_TURN_X) ? g->x_name : g->o_name;
        snprintf(label, sizeof(label),
                 "[%s] %s(X) vs %s(o)  %s to move  Move %u",
                 g->game_id, g->x_name,
                 g->o_session ? g->o_name : "...", turn_name,
                 g->move_count);
    }
    render_board(g->board, s, label);
}

void ttt_broadcast_board(ttt_game_t *g)
{
    unsigned j;

    if (g == NULL) {
        return;
    }
    if (g->x_session != NULL) {
        ttt_send_board(g, g->x_session);
    }
    if (g->o_session != NULL) {
        ttt_send_board(g, g->o_session);
    }
    for (j = 0; j < g->spectator_count; j++) {
        ttt_send_board(g, g->spectators[j]);
    }
}

/* ── Tick — board refresh for spectators ───────────────────── */

void ttt_tick(struct mailboxd_service *service)
{
    unsigned i;

    (void)service;
    for (i = 0; i < g_max_games; i++) {
        ttt_game_t *g = &g_games[i];
        unsigned j;

        if (!g->active || g->spectator_count == 0) {
            continue;
        }
        if (g->o_session == NULL || g->result != TTT_RESULT_NONE) {
            continue;
        }
        g->tick_counter++;
        if (g->tick_counter < TTT_REFRESH_SEC) {
            continue;
        }
        g->tick_counter = 0;
        for (j = 0; j < g->spectator_count; j++) {
            ttt_send_board(g, g->spectators[j]);
        }
    }
}