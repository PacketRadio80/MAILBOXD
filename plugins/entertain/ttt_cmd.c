/*
 * Entertain plugin — ttt_cmd.c
 * /ttt · /tictactoe command handler.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "entertain_ttt.h"
#include "mailboxd/session.h"
#include "mailboxd/service.h"
#include "mailboxd/command.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

mailboxd_result_t entertain_cmd_ttt(struct mailboxd_service *service,
                                    struct mailboxd_session *session,
                                    const struct mailboxd_parsed_command *cmd);

static int ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a != '\0' && *b != '\0') {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;

        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static void ttt_help(mailboxd_session_t *session)
{
    mailboxd_session_write_line(session, "Tic-Tac-Toe (Entertain area):");
    mailboxd_session_write_line(session, "  /ttt new              Create game (you = X)");
    mailboxd_session_write_line(session, "  /ttt join [id]        Join as o");
    mailboxd_session_write_line(session, "  /ttt watch [id]       Spectate");
    mailboxd_session_write_line(session, "  /ttt board            Show your board");
    mailboxd_session_write_line(session, "  /ttt move <row> <col> Place mark (row/col 1-3)");
    mailboxd_session_write_line(session, "  /ttt resign           Resign");
    mailboxd_session_write_line(session, "  /ttt list             List games");
    mailboxd_session_write_line(session, "  /ttt leave            Stop spectating");
    mailboxd_session_write_line(session, "  /ttt clear            Free finished game slots");
    mailboxd_session_write_line(session, "  /tictactoe …          Alias for /ttt");
}

static void announce_result(ttt_game_t *g)
{
    char line[128];
    const char *res;
    unsigned j;

    if (g == NULL || g->result == TTT_RESULT_NONE) {
        return;
    }
    switch (g->result) {
    case TTT_X_WINS:
        res = "X wins";
        break;
    case TTT_O_WINS:
        res = "o wins";
        break;
    case TTT_DRAW:
        res = "Draw";
        break;
    default:
        res = "Game over";
        break;
    }
    snprintf(line, sizeof(line), "[%s] %s", g->game_id, res);
    if (g->x_session != NULL) {
        mailboxd_session_write_line(g->x_session, line);
    }
    if (g->o_session != NULL) {
        mailboxd_session_write_line(g->o_session, line);
    }
    for (j = 0; j < g->spectator_count; j++) {
        mailboxd_session_write_line(g->spectators[j], line);
    }
}

mailboxd_result_t entertain_cmd_ttt(struct mailboxd_service *service,
                                    struct mailboxd_session *session,
                                    const struct mailboxd_parsed_command *cmd)
{
    const char *sub;
    char line[160];

    (void)service;

    if (!mailboxd_session_logged_in(session)) {
        mailboxd_session_write_line(session, "Log in to play Tic-Tac-Toe.");
        return MAILBOXD_ERR_DENIED;
    }

    sub = (cmd->argc >= 1) ? cmd->argv[0] : "help";

    if (ieq(sub, "help") || ieq(sub, "?")) {
        ttt_help(session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "list") || ieq(sub, "ls")) {
        unsigned found = 0;
        unsigned i;

        mailboxd_session_write_line(session, "Tic-Tac-Toe games:");
        for (i = 0; i < ttt_max_games(); i++) {
            ttt_game_t *g = ttt_get_game(i);
            const char *st;

            if (g == NULL || !g->active) {
                continue;
            }
            if (g->result != TTT_RESULT_NONE) {
                st = "finished";
            } else if (g->o_session == NULL) {
                st = "waiting";
            } else {
                st = "playing";
            }
            snprintf(line, sizeof(line), "  [%s] %s(X) vs %s(o)  %s  move %u",
                     g->game_id, g->x_name,
                     g->o_session ? g->o_name : "...",
                     st, g->move_count);
            mailboxd_session_write_line(session, line);
            found++;
        }
        if (!found) {
            mailboxd_session_write_line(session, "  No active games.");
        }
        return MAILBOXD_OK;
    }

    if (ieq(sub, "new") || ieq(sub, "create")) {
        ttt_game_t *g;

        if (ttt_find_player_game(session) != NULL) {
            mailboxd_session_write_line(session,
                "Already in a game. /ttt resign or wait for finish + /ttt clear");
            return MAILBOXD_ERR_DENIED;
        }
        g = ttt_create_game(session, mailboxd_session_username(session));
        if (g == NULL) {
            mailboxd_session_write_line(session,
                "No free game slots. /ttt clear to free finished games.");
            return MAILBOXD_ERR_DENIED;
        }
        snprintf(line, sizeof(line), "Game %s created. You are X.",
                 g->game_id);
        mailboxd_session_write_line(session, line);
        mailboxd_session_write_line(session, "Waiting for o: /ttt join");
        ttt_send_board(g, session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "join")) {
        const char *id = (cmd->argc >= 2) ? cmd->argv[1] : NULL;
        ttt_game_t *g;

        if (ttt_find_player_game(session) != NULL) {
            mailboxd_session_write_line(session, "Already in a game.");
            return MAILBOXD_ERR_DENIED;
        }
        if (id != NULL) {
            g = ttt_find_game_by_id(id);
            if (g == NULL || !g->active) {
                mailboxd_session_write_line(session, "Game not found.");
                return MAILBOXD_ERR_NOT_FOUND;
            }
            if (g->o_session != NULL) {
                mailboxd_session_write_line(session, "Game is full.");
                return MAILBOXD_ERR_DENIED;
            }
            if (g->result != TTT_RESULT_NONE) {
                mailboxd_session_write_line(session, "Game already finished.");
                return MAILBOXD_ERR_DENIED;
            }
        } else {
            g = ttt_find_open_game();
            if (g == NULL) {
                mailboxd_session_write_line(session, "No open games. /ttt new");
                return MAILBOXD_ERR_NOT_FOUND;
            }
        }
        if (!ttt_join_game(g, session, mailboxd_session_username(session))) {
            mailboxd_session_write_line(session, "Cannot join that game.");
            return MAILBOXD_ERR_DENIED;
        }
        snprintf(line, sizeof(line), "Joined %s as o. X to move.",
                 g->game_id);
        mailboxd_session_write_line(session, line);
        if (g->x_session != NULL) {
            snprintf(line, sizeof(line), "[%s] %s joined as o.",
                     g->game_id, g->o_name);
            mailboxd_session_write_line(g->x_session, line);
        }
        ttt_broadcast_board(g);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "watch") || ieq(sub, "spectate")) {
        const char *id = (cmd->argc >= 2) ? cmd->argv[1] : NULL;
        ttt_game_t *g = NULL;

        if (id != NULL) {
            g = ttt_find_game_by_id(id);
        } else {
            unsigned i;

            for (i = 0; i < ttt_max_games(); i++) {
                ttt_game_t *t = ttt_get_game(i);

                if (t != NULL && t->active && t->o_session != NULL &&
                    t->result == TTT_RESULT_NONE) {
                    g = t;
                    break;
                }
            }
        }
        if (g == NULL || !g->active) {
            mailboxd_session_write_line(session, "No game to watch.");
            return MAILBOXD_ERR_NOT_FOUND;
        }
        if (!ttt_add_spectator(g, session)) {
            mailboxd_session_write_line(session,
                "Cannot watch (player or spectator list full).");
            return MAILBOXD_ERR_DENIED;
        }
        snprintf(line, sizeof(line),
                 "Watching %s. Board refreshes every %us for spectators.",
                 g->game_id, TTT_REFRESH_SEC);
        mailboxd_session_write_line(session, line);
        ttt_send_board(g, session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "leave") || ieq(sub, "unwatch")) {
        unsigned i;

        for (i = 0; i < ttt_max_games(); i++) {
            ttt_game_t *g = ttt_get_game(i);

            if (g != NULL && ttt_is_spectator(g, session)) {
                ttt_remove_spectator(g, session);
                mailboxd_session_write_line(session, "Stopped watching.");
                return MAILBOXD_OK;
            }
        }
        mailboxd_session_write_line(session, "Not watching any game.");
        return MAILBOXD_ERR_NOT_FOUND;
    }

    if (ieq(sub, "board") || ieq(sub, "show")) {
        unsigned i;

        for (i = 0; i < ttt_max_games(); i++) {
            ttt_game_t *g = ttt_get_game(i);

            if (g != NULL && g->active &&
                (ttt_is_player(g, session) || ttt_is_spectator(g, session))) {
                ttt_send_board(g, session);
                return MAILBOXD_OK;
            }
        }
        mailboxd_session_write_line(session,
            "Not in a game. /ttt new, join, or watch.");
        return MAILBOXD_ERR_NOT_FOUND;
    }

    if (ieq(sub, "move") || ieq(sub, "m")) {
        ttt_game_t *game;
        unsigned row, col;
        char err[64];

        if (cmd->argc < 3) {
            mailboxd_session_write_line(session,
                "Usage: /ttt move <row> <col>  (row/col 1-3)");
            return MAILBOXD_ERR_INVALID;
        }

        game = ttt_find_player_game(session);
        if (game == NULL || game->result != TTT_RESULT_NONE) {
            mailboxd_session_write_line(session, "No active game. /ttt new or /ttt join");
            return MAILBOXD_ERR_DENIED;
        }
        if ((game->turn == TTT_TURN_X && game->x_session != session) ||
            (game->turn == TTT_TURN_O && game->o_session != session)) {
            mailboxd_session_write_line(session, "Not your turn.");
            return MAILBOXD_ERR_DENIED;
        }

        row = (unsigned)atoi(cmd->argv[1]);
        col = (unsigned)atoi(cmd->argv[2]);
        if (row < 1 || row > 3 || col < 1 || col > 3) {
            mailboxd_session_write_line(session, "Row and column must be 1-3.");
            return MAILBOXD_ERR_INVALID;
        }

        if (!ttt_make_move(game, row - 1, col - 1, err, sizeof(err))) {
            snprintf(line, sizeof(line), "Illegal: %s", err);
            mailboxd_session_write_line(session, line);
            return MAILBOXD_ERR_INVALID;
        }
        ttt_broadcast_board(game);
        if (game->result != TTT_RESULT_NONE) {
            announce_result(game);
        }
        return MAILBOXD_OK;
    }

    if (ieq(sub, "resign")) {
        ttt_game_t *game = ttt_find_player_game(session);
        const char *who;

        if (game == NULL) {
            mailboxd_session_write_line(session, "Not in a game.");
            return MAILBOXD_ERR_NOT_FOUND;
        }
        if (game->result != TTT_RESULT_NONE) {
            mailboxd_session_write_line(session, "Game already finished.");
            return MAILBOXD_ERR_DENIED;
        }
        who = mailboxd_session_username(session);
        ttt_resign(game, session);
        snprintf(line, sizeof(line), "[%s] %s resigned.", game->game_id,
                 who != NULL ? who : "?");
        if (game->x_session != NULL) {
            mailboxd_session_write_line(game->x_session, line);
        }
        if (game->o_session != NULL) {
            mailboxd_session_write_line(game->o_session, line);
        }
        {
            unsigned j;

            for (j = 0; j < game->spectator_count; j++) {
                mailboxd_session_write_line(game->spectators[j], line);
            }
        }
        announce_result(game);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "clear") || ieq(sub, "reset")) {
        unsigned n = ttt_clear_finished();

        snprintf(line, sizeof(line), "Cleared %u finished game(s).", n);
        mailboxd_session_write_line(session, line);
        return MAILBOXD_OK;
    }

    mailboxd_session_write_line(session, "Unknown ttt command. /ttt help");
    return MAILBOXD_ERR_NOT_FOUND;
}