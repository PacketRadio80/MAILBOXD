/*
 * Entertain plugin — c4_cmd.c
 * /c4 · /connect · /connect4 command handler.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "entertain_c4.h"
#include "mailboxd/session.h"
#include "mailboxd/service.h"
#include "mailboxd/command.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

mailboxd_result_t entertain_cmd_c4(struct mailboxd_service *service,
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

static void c4_help(mailboxd_session_t *session)
{
    mailboxd_session_write_line(session, "Connect Four (Entertain area):");
    mailboxd_session_write_line(session, "  /c4 new              Create game (you = Red/X)");
    mailboxd_session_write_line(session, "  /c4 join [id]        Join as Yellow/o");
    mailboxd_session_write_line(session, "  /c4 watch [id]       Spectate");
    mailboxd_session_write_line(session, "  /c4 board            Show your board");
    mailboxd_session_write_line(session, "  /c4 drop <col>       Drop piece (col 1-7)");
    mailboxd_session_write_line(session, "  /c4 resign           Resign");
    mailboxd_session_write_line(session, "  /c4 list             List games");
    mailboxd_session_write_line(session, "  /c4 leave            Stop spectating");
    mailboxd_session_write_line(session, "  /c4 clear            Free finished game slots");
    mailboxd_session_write_line(session, "  /connect …           Alias for /c4");
    mailboxd_session_write_line(session, "  /connect4 …          Alias for /c4");
}

static void announce_result(c4_game_t *g)
{
    char line[128];
    const char *res;
    unsigned j;

    if (g == NULL || g->result == C4_RESULT_NONE) {
        return;
    }
    switch (g->result) {
    case C4_RED_WINS:
        res = "Red (X) wins";
        break;
    case C4_YELLOW_WINS:
        res = "Yellow (o) wins";
        break;
    case C4_DRAW:
        res = "Draw";
        break;
    default:
        res = "Game over";
        break;
    }
    snprintf(line, sizeof(line), "[%s] %s", g->game_id, res);
    if (g->red_session != NULL) {
        mailboxd_session_write_line(g->red_session, line);
    }
    if (g->yellow_session != NULL) {
        mailboxd_session_write_line(g->yellow_session, line);
    }
    for (j = 0; j < g->spectator_count; j++) {
        mailboxd_session_write_line(g->spectators[j], line);
    }
}

mailboxd_result_t entertain_cmd_c4(struct mailboxd_service *service,
                                   struct mailboxd_session *session,
                                   const struct mailboxd_parsed_command *cmd)
{
    const char *sub;
    char line[160];

    (void)service;

    if (!mailboxd_session_logged_in(session)) {
        mailboxd_session_write_line(session, "Log in to play Connect Four.");
        return MAILBOXD_ERR_DENIED;
    }

    sub = (cmd->argc >= 1) ? cmd->argv[0] : "help";

    if (ieq(sub, "help") || ieq(sub, "?")) {
        c4_help(session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "list") || ieq(sub, "ls")) {
        unsigned found = 0;
        unsigned i;

        mailboxd_session_write_line(session, "Connect Four games:");
        for (i = 0; i < c4_max_games(); i++) {
            c4_game_t *g = c4_get_game(i);
            const char *st;

            if (g == NULL || !g->active) {
                continue;
            }
            if (g->result != C4_RESULT_NONE) {
                st = "finished";
            } else if (g->yellow_session == NULL) {
                st = "waiting";
            } else {
                st = "playing";
            }
            snprintf(line, sizeof(line), "  [%s] %s(X) vs %s(o)  %s  move %u",
                     g->game_id, g->red_name,
                     g->yellow_session ? g->yellow_name : "...",
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
        c4_game_t *g;

        if (c4_find_player_game(session) != NULL) {
            mailboxd_session_write_line(session,
                "Already in a game. /c4 resign or wait for finish + /c4 clear");
            return MAILBOXD_ERR_DENIED;
        }
        g = c4_create_game(session, mailboxd_session_username(session));
        if (g == NULL) {
            mailboxd_session_write_line(session,
                "No free game slots. /c4 clear to free finished games.");
            return MAILBOXD_ERR_DENIED;
        }
        snprintf(line, sizeof(line), "Game %s created. You are Red (X).",
                 g->game_id);
        mailboxd_session_write_line(session, line);
        mailboxd_session_write_line(session, "Waiting for Yellow (o): /c4 join");
        c4_send_board(g, session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "join")) {
        const char *id = (cmd->argc >= 2) ? cmd->argv[1] : NULL;
        c4_game_t *g;

        if (c4_find_player_game(session) != NULL) {
            mailboxd_session_write_line(session, "Already in a game.");
            return MAILBOXD_ERR_DENIED;
        }
        if (id != NULL) {
            g = c4_find_game_by_id(id);
            if (g == NULL || !g->active) {
                mailboxd_session_write_line(session, "Game not found.");
                return MAILBOXD_ERR_NOT_FOUND;
            }
            if (g->yellow_session != NULL) {
                mailboxd_session_write_line(session, "Game is full.");
                return MAILBOXD_ERR_DENIED;
            }
            if (g->result != C4_RESULT_NONE) {
                mailboxd_session_write_line(session, "Game already finished.");
                return MAILBOXD_ERR_DENIED;
            }
        } else {
            g = c4_find_open_game();
            if (g == NULL) {
                mailboxd_session_write_line(session, "No open games. /c4 new");
                return MAILBOXD_ERR_NOT_FOUND;
            }
        }
        if (!c4_join_game(g, session, mailboxd_session_username(session))) {
            mailboxd_session_write_line(session, "Cannot join that game.");
            return MAILBOXD_ERR_DENIED;
        }
        snprintf(line, sizeof(line), "Joined %s as Yellow (o). Red to drop.",
                 g->game_id);
        mailboxd_session_write_line(session, line);
        if (g->red_session != NULL) {
            snprintf(line, sizeof(line), "[%s] %s joined as Yellow (o).",
                     g->game_id, g->yellow_name);
            mailboxd_session_write_line(g->red_session, line);
        }
        c4_broadcast_board(g);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "watch") || ieq(sub, "spectate")) {
        const char *id = (cmd->argc >= 2) ? cmd->argv[1] : NULL;
        c4_game_t *g = NULL;

        if (id != NULL) {
            g = c4_find_game_by_id(id);
        } else {
            unsigned i;

            for (i = 0; i < c4_max_games(); i++) {
                c4_game_t *t = c4_get_game(i);

                if (t != NULL && t->active && t->yellow_session != NULL &&
                    t->result == C4_RESULT_NONE) {
                    g = t;
                    break;
                }
            }
        }
        if (g == NULL || !g->active) {
            mailboxd_session_write_line(session, "No game to watch.");
            return MAILBOXD_ERR_NOT_FOUND;
        }
        if (!c4_add_spectator(g, session)) {
            mailboxd_session_write_line(session,
                "Cannot watch (player or spectator list full).");
            return MAILBOXD_ERR_DENIED;
        }
        snprintf(line, sizeof(line),
                 "Watching %s. Board refreshes every %us for spectators.",
                 g->game_id, C4_REFRESH_SEC);
        mailboxd_session_write_line(session, line);
        c4_send_board(g, session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "leave") || ieq(sub, "unwatch")) {
        unsigned i;

        for (i = 0; i < c4_max_games(); i++) {
            c4_game_t *g = c4_get_game(i);

            if (g != NULL && c4_is_spectator(g, session)) {
                c4_remove_spectator(g, session);
                mailboxd_session_write_line(session, "Stopped watching.");
                return MAILBOXD_OK;
            }
        }
        mailboxd_session_write_line(session, "Not watching any game.");
        return MAILBOXD_ERR_NOT_FOUND;
    }

    if (ieq(sub, "board") || ieq(sub, "show")) {
        unsigned i;

        for (i = 0; i < c4_max_games(); i++) {
            c4_game_t *g = c4_get_game(i);

            if (g != NULL && g->active &&
                (c4_is_player(g, session) || c4_is_spectator(g, session))) {
                c4_send_board(g, session);
                return MAILBOXD_OK;
            }
        }
        mailboxd_session_write_line(session,
            "Not in a game. /c4 new, join, or watch.");
        return MAILBOXD_ERR_NOT_FOUND;
    }

    if (ieq(sub, "drop") || ieq(sub, "d")) {
        c4_game_t *game;
        unsigned col;
        char err[64];

        if (cmd->argc < 2) {
            mailboxd_session_write_line(session,
                "Usage: /c4 drop <col>  (col 1-7)");
            return MAILBOXD_ERR_INVALID;
        }

        game = c4_find_player_game(session);
        if (game == NULL || game->result != C4_RESULT_NONE) {
            mailboxd_session_write_line(session, "No active game. /c4 new or /c4 join");
            return MAILBOXD_ERR_DENIED;
        }
        if ((game->turn == C4_TURN_RED && game->red_session != session) ||
            (game->turn == C4_TURN_YELLOW && game->yellow_session != session)) {
            mailboxd_session_write_line(session, "Not your turn.");
            return MAILBOXD_ERR_DENIED;
        }

        col = (unsigned)atoi(cmd->argv[1]);
        if (col < 1 || col > 7) {
            mailboxd_session_write_line(session, "Column must be 1-7.");
            return MAILBOXD_ERR_INVALID;
        }

        if (!c4_make_move(game, col - 1, err, sizeof(err))) {
            snprintf(line, sizeof(line), "Illegal: %s", err);
            mailboxd_session_write_line(session, line);
            return MAILBOXD_ERR_INVALID;
        }
        c4_broadcast_board(game);
        if (game->result != C4_RESULT_NONE) {
            announce_result(game);
        }
        return MAILBOXD_OK;
    }

    if (ieq(sub, "resign")) {
        c4_game_t *game = c4_find_player_game(session);
        const char *who;

        if (game == NULL) {
            mailboxd_session_write_line(session, "Not in a game.");
            return MAILBOXD_ERR_NOT_FOUND;
        }
        if (game->result != C4_RESULT_NONE) {
            mailboxd_session_write_line(session, "Game already finished.");
            return MAILBOXD_ERR_DENIED;
        }
        who = mailboxd_session_username(session);
        c4_resign(game, session);
        snprintf(line, sizeof(line), "[%s] %s resigned.", game->game_id,
                 who != NULL ? who : "?");
        if (game->red_session != NULL) {
            mailboxd_session_write_line(game->red_session, line);
        }
        if (game->yellow_session != NULL) {
            mailboxd_session_write_line(game->yellow_session, line);
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
        unsigned n = c4_clear_finished();

        snprintf(line, sizeof(line), "Cleared %u finished game(s).", n);
        mailboxd_session_write_line(session, line);
        return MAILBOXD_OK;
    }

    mailboxd_session_write_line(session, "Unknown c4 command. /c4 help");
    return MAILBOXD_ERR_NOT_FOUND;
}