/*
 * Entertain plugin — chess_cmd.c
 * /chess · /play · /mv command handler.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "entertain_chess.h"
#include "mailboxd/session.h"
#include "mailboxd/service.h"
#include "mailboxd/command.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

mailboxd_result_t entertain_cmd_chess(struct mailboxd_service *service,
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

static void chess_help(mailboxd_session_t *session)
{
    char line[96];

    mailboxd_session_write_line(session, "Chess (Entertain area):");
    mailboxd_session_write_line(session, "  /chess new           Create game (you = White)");
    mailboxd_session_write_line(session, "  /chess join [id]     Join as Black");
    mailboxd_session_write_line(session, "  /chess watch [id]    Spectate");
    mailboxd_session_write_line(session, "  /chess board         Show your board");
    mailboxd_session_write_line(session, "  /chess move e2 e4    Move (or e2e4)");
    mailboxd_session_write_line(session, "  /mv e2 e4            Move alias");
    mailboxd_session_write_line(session, "  /chess move e7 e8 q  Promote (q/r/b/n)");
    mailboxd_session_write_line(session, "  /chess resign        Resign");
    mailboxd_session_write_line(session, "  /chess draw          Offer / accept draw");
    mailboxd_session_write_line(session, "  /chess list          List games");
    mailboxd_session_write_line(session, "  /chess leave         Stop spectating");
    mailboxd_session_write_line(session, "  /chess clear         Free finished game slots");
    mailboxd_session_write_line(session, "  /play …              Alias for /chess");
    snprintf(line, sizeof(line),
             "Notes: no castling/en passant. Spectator refresh %us.",
             CHESS_REFRESH_SEC);
    mailboxd_session_write_line(session, line);
}

static void announce_result(chess_game_t *g)
{
    char line[128];
    const char *res;
    unsigned j;

    if (g == NULL || g->result == CHESS_RESULT_NONE) {
        return;
    }
    switch (g->result) {
    case CHESS_WHITE_WINS:
        res = "White wins";
        break;
    case CHESS_BLACK_WINS:
        res = "Black wins";
        break;
    case CHESS_DRAW_STALEMATE:
        res = "Stalemate";
        break;
    case CHESS_DRAW_AGREEMENT:
        res = "Draw agreed";
        break;
    default:
        res = "Game over";
        break;
    }
    snprintf(line, sizeof(line), "[%s] %s", g->game_id, res);
    if (g->white_session != NULL) {
        mailboxd_session_write_line(g->white_session, line);
    }
    if (g->black_session != NULL) {
        mailboxd_session_write_line(g->black_session, line);
    }
    for (j = 0; j < g->spectator_count; j++) {
        mailboxd_session_write_line(g->spectators[j], line);
    }
}

static int parse_move_args(const mailboxd_parsed_command_t *cmd, unsigned start,
                           char *from, size_t from_len, char *to, size_t to_len,
                           char *promo)
{
    const char *a;
    const char *b;

    *promo = 0;
    if (cmd == NULL || from == NULL || to == NULL || from_len < 3 || to_len < 3) {
        return 0;
    }

    if (cmd->argc < start + 1) {
        return 0;
    }
    a = cmd->argv[start];
    if (a == NULL) {
        return 0;
    }

    /* Compact: e2e4 or e7e8q */
    if (strlen(a) >= 4 && cmd->argc == start + 1) {
        if (!isalpha((unsigned char)a[0]) || !isdigit((unsigned char)a[1]) ||
            !isalpha((unsigned char)a[2]) || !isdigit((unsigned char)a[3])) {
            return 0;
        }
        from[0] = (char)tolower((unsigned char)a[0]);
        from[1] = a[1];
        from[2] = '\0';
        to[0] = (char)tolower((unsigned char)a[2]);
        to[1] = a[3];
        to[2] = '\0';
        if (a[4] != '\0') {
            *promo = a[4];
        }
        return 1;
    }

    if (cmd->argc < start + 2) {
        return 0;
    }
    b = cmd->argv[start + 1];
    if (b == NULL) {
        return 0;
    }
    snprintf(from, from_len, "%s", a);
    snprintf(to, to_len, "%s", b);
    if (cmd->argc >= start + 3 && cmd->argv[start + 2] != NULL &&
        cmd->argv[start + 2][0] != '\0') {
        *promo = cmd->argv[start + 2][0];
    }
    return 1;
}

static mailboxd_result_t do_move(mailboxd_session_t *session,
                              const mailboxd_parsed_command_t *cmd,
                              unsigned arg_start)
{
    chess_game_t *game;
    char from[8];
    char to[8];
    char promo = 0;
    char err[64];
    char line[96];

    if (!parse_move_args(cmd, arg_start, from, sizeof(from), to, sizeof(to),
                         &promo)) {
        mailboxd_session_write_line(session,
            "Usage: /chess move e2 e4  |  /mv e2 e4  |  /mv e2e4");
        return MAILBOXD_ERR_INVALID;
    }

    game = chess_find_player_game(session);
    if (game == NULL || game->result != CHESS_RESULT_NONE) {
        mailboxd_session_write_line(session, "No active game. /chess new or /chess join");
        return MAILBOXD_ERR_DENIED;
    }
    if (chess_player_color(game, session) != game->turn) {
        mailboxd_session_write_line(session, "Not your turn.");
        return MAILBOXD_ERR_DENIED;
    }
    if (!chess_make_move(game, from, to, promo, err, sizeof(err))) {
        snprintf(line, sizeof(line), "Illegal: %s", err);
        mailboxd_session_write_line(session, line);
        return MAILBOXD_ERR_INVALID;
    }
    chess_broadcast_board(game);
    if (game->result != CHESS_RESULT_NONE) {
        announce_result(game);
    }
    return MAILBOXD_OK;
}

mailboxd_result_t entertain_cmd_chess(struct mailboxd_service *service,
                                   struct mailboxd_session *session,
                                   const struct mailboxd_parsed_command *cmd)
{
    const char *sub;
    char line[160];
    int mv_shortcut = 0;

    (void)service;

    if (!mailboxd_session_logged_in(session)) {
        mailboxd_session_write_line(session, "Log in to play chess.");
        return MAILBOXD_ERR_DENIED;
    }

    /* /mv … → treat as move with args at argv[0..] */
    if (cmd->verb != NULL && ieq(cmd->verb, "mv")) {
        mv_shortcut = 1;
        sub = "move";
    } else {
        sub = (cmd->argc >= 1) ? cmd->argv[0] : "help";
    }

    if (ieq(sub, "help") || ieq(sub, "?")) {
        chess_help(session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "status")) {
        snprintf(line, sizeof(line), "Chess: max %u concurrent games",
                 chess_max_games());
        mailboxd_session_write_line(session, line);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "list") || ieq(sub, "ls")) {
        unsigned found = 0;
        unsigned i;

        mailboxd_session_write_line(session, "Chess games:");
        for (i = 0; i < chess_max_games(); i++) {
            chess_game_t *g = chess_get_game(i);
            const char *st;

            if (g == NULL) {
                continue;
            }
            if (g->result != CHESS_RESULT_NONE) {
                st = "finished";
            } else if (g->black_session == NULL) {
                st = "waiting";
            } else {
                st = "playing";
            }
            snprintf(line, sizeof(line), "  [%s] %s(W) vs %s(B)  %s  move %u",
                     g->game_id, g->white_name,
                     g->black_session ? g->black_name : "...",
                     st, g->move_number);
            mailboxd_session_write_line(session, line);
            found++;
        }
        if (!found) {
            mailboxd_session_write_line(session, "  No active games.");
        }
        return MAILBOXD_OK;
    }

    if (ieq(sub, "new") || ieq(sub, "create")) {
        chess_game_t *g;

        if (chess_find_player_game(session) != NULL) {
            mailboxd_session_write_line(session,
                "Already in a game. /chess resign or wait for finish + /chess clear");
            return MAILBOXD_ERR_DENIED;
        }
        g = chess_create_game(session, mailboxd_session_username(session));
        if (g == NULL) {
            mailboxd_session_write_line(session,
                "No free game slots. /chess clear to free finished games.");
            return MAILBOXD_ERR_DENIED;
        }
        snprintf(line, sizeof(line), "Game %s created. You are White.",
                 g->game_id);
        mailboxd_session_write_line(session, line);
        mailboxd_session_write_line(session, "Waiting for Black: /chess join");
        chess_send_board(g, session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "join")) {
        const char *id = (!mv_shortcut && cmd->argc >= 2) ? cmd->argv[1] : NULL;
        chess_game_t *g;

        if (chess_find_player_game(session) != NULL) {
            mailboxd_session_write_line(session, "Already in a game.");
            return MAILBOXD_ERR_DENIED;
        }
        if (id != NULL) {
            g = chess_find_game_by_id(id);
            if (g == NULL || !g->active) {
                mailboxd_session_write_line(session, "Game not found.");
                return MAILBOXD_ERR_NOT_FOUND;
            }
            if (g->black_session != NULL) {
                mailboxd_session_write_line(session, "Game is full.");
                return MAILBOXD_ERR_DENIED;
            }
            if (g->result != CHESS_RESULT_NONE) {
                mailboxd_session_write_line(session, "Game already finished.");
                return MAILBOXD_ERR_DENIED;
            }
        } else {
            g = chess_find_open_game();
            if (g == NULL) {
                mailboxd_session_write_line(session, "No open games. /chess new");
                return MAILBOXD_ERR_NOT_FOUND;
            }
        }
        if (!chess_join_game(g, session, mailboxd_session_username(session))) {
            mailboxd_session_write_line(session, "Cannot join that game.");
            return MAILBOXD_ERR_DENIED;
        }
        snprintf(line, sizeof(line), "Joined %s as Black. White to move.",
                 g->game_id);
        mailboxd_session_write_line(session, line);
        if (g->white_session != NULL) {
            snprintf(line, sizeof(line), "[%s] %s joined as Black.",
                     g->game_id, g->black_name);
            mailboxd_session_write_line(g->white_session, line);
        }
        chess_broadcast_board(g);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "watch") || ieq(sub, "spectate")) {
        const char *id = (!mv_shortcut && cmd->argc >= 2) ? cmd->argv[1] : NULL;
        chess_game_t *g = NULL;

        if (id != NULL) {
            g = chess_find_game_by_id(id);
        } else {
            unsigned i;

            for (i = 0; i < chess_max_games(); i++) {
                chess_game_t *t = chess_get_game(i);

                if (t != NULL && t->black_session != NULL &&
                    t->result == CHESS_RESULT_NONE) {
                    g = t;
                    break;
                }
            }
        }
        if (g == NULL || !g->active) {
            mailboxd_session_write_line(session, "No game to watch.");
            return MAILBOXD_ERR_NOT_FOUND;
        }
        if (!chess_add_spectator(g, session)) {
            mailboxd_session_write_line(session,
                "Cannot watch (player or spectator list full).");
            return MAILBOXD_ERR_DENIED;
        }
        snprintf(line, sizeof(line),
                 "Watching %s. Board refreshes every %us for spectators.",
                 g->game_id, CHESS_REFRESH_SEC);
        mailboxd_session_write_line(session, line);
        chess_send_board(g, session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "leave") || ieq(sub, "unwatch")) {
        unsigned i;

        for (i = 0; i < chess_max_games(); i++) {
            chess_game_t *g = chess_get_game(i);

            if (g != NULL && chess_is_spectator(g, session)) {
                chess_remove_spectator(g, session);
                mailboxd_session_write_line(session, "Stopped watching.");
                return MAILBOXD_OK;
            }
        }
        mailboxd_session_write_line(session, "Not watching any game.");
        return MAILBOXD_ERR_NOT_FOUND;
    }

    if (ieq(sub, "board") || ieq(sub, "show")) {
        unsigned i;

        for (i = 0; i < chess_max_games(); i++) {
            chess_game_t *g = chess_get_game(i);

            if (g != NULL &&
                (chess_is_player(g, session) || chess_is_spectator(g, session))) {
                chess_send_board(g, session);
                return MAILBOXD_OK;
            }
        }
        mailboxd_session_write_line(session,
            "Not in a game. /chess new, join, or watch.");
        return MAILBOXD_ERR_NOT_FOUND;
    }

    if (ieq(sub, "move") || ieq(sub, "m")) {
        return do_move(session, cmd, mv_shortcut ? 0u : 1u);
    }

    if (ieq(sub, "resign")) {
        chess_game_t *game = chess_find_player_game(session);
        const char *who;

        if (game == NULL) {
            mailboxd_session_write_line(session, "Not in a game.");
            return MAILBOXD_ERR_NOT_FOUND;
        }
        if (game->result != CHESS_RESULT_NONE) {
            mailboxd_session_write_line(session, "Game already finished.");
            return MAILBOXD_ERR_DENIED;
        }
        who = mailboxd_session_username(session);
        chess_resign(game, session);
        snprintf(line, sizeof(line), "[%s] %s resigned.", game->game_id,
                 who != NULL ? who : "?");
        if (game->white_session != NULL) {
            mailboxd_session_write_line(game->white_session, line);
        }
        if (game->black_session != NULL) {
            mailboxd_session_write_line(game->black_session, line);
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

    if (ieq(sub, "draw")) {
        chess_game_t *game = chess_find_player_game(session);
        struct mailboxd_session *other;
        int agreed;

        if (game == NULL) {
            mailboxd_session_write_line(session, "Not in a game.");
            return MAILBOXD_ERR_NOT_FOUND;
        }
        if (game->result != CHESS_RESULT_NONE) {
            mailboxd_session_write_line(session, "Game already finished.");
            return MAILBOXD_ERR_DENIED;
        }
        agreed = chess_offer_draw(game, session);
        if (agreed) {
            announce_result(game);
            chess_broadcast_board(game);
            return MAILBOXD_OK;
        }
        mailboxd_session_write_line(session, "Draw offered. Opponent: /chess draw");
        other = (game->white_session == session) ? game->black_session
                                                 : game->white_session;
        if (other != NULL) {
            snprintf(line, sizeof(line),
                     "[%s] Draw offered by %s — /chess draw to accept",
                     game->game_id,
                     mailboxd_session_username(session)
                         ? mailboxd_session_username(session)
                         : "?");
            mailboxd_session_write_line(other, line);
        }
        return MAILBOXD_OK;
    }

    if (ieq(sub, "clear") || ieq(sub, "reset")) {
        unsigned n = chess_clear_finished();

        snprintf(line, sizeof(line), "Cleared %u finished game(s).", n);
        mailboxd_session_write_line(session, line);
        return MAILBOXD_OK;
    }

    mailboxd_session_write_line(session, "Unknown chess command. /chess help");
    return MAILBOXD_ERR_NOT_FOUND;
}
