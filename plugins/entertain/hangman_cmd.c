/*
 * Entertain plugin — hangman_cmd.c
 * /hangman · /hm · /hang command handler.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "entertain_hangman.h"
#include "mailboxd/session.h"
#include "mailboxd/service.h"
#include "mailboxd/command.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

mailboxd_result_t entertain_cmd_hangman(struct mailboxd_service *service,
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

static void hangman_help(mailboxd_session_t *session)
{
    mailboxd_session_write_line(session, "Hangman (Entertain area, single-player):");
    mailboxd_session_write_line(session, "  /hm new              Start a new game");
    mailboxd_session_write_line(session, "  /hm <letter>         Guess a letter");
    mailboxd_session_write_line(session, "  /hm word <word>      Guess the whole word");
    mailboxd_session_write_line(session, "  /hm board            Show current board");
    mailboxd_session_write_line(session, "  /hm clear            Free finished games");
    mailboxd_session_write_line(session, "  /hangman …           Alias for /hm");
    mailboxd_session_write_line(session, "  /hang …              Alias for /hm");
    mailboxd_session_write_line(session, "  7 wrong letters = hanged!");
}

mailboxd_result_t entertain_cmd_hangman(struct mailboxd_service *service,
                                        struct mailboxd_session *session,
                                        const struct mailboxd_parsed_command *cmd)
{
    const char *sub;
    char line[128];

    (void)service;

    if (!mailboxd_session_logged_in(session)) {
        mailboxd_session_write_line(session, "Log in to play Hangman.");
        return MAILBOXD_ERR_DENIED;
    }

    sub = (cmd->argc >= 1) ? cmd->argv[0] : "help";

    if (ieq(sub, "help") || ieq(sub, "?")) {
        hangman_help(session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "new") || ieq(sub, "create") || ieq(sub, "start")) {
        hangman_game_t *g;

        if (hangman_find_player_game(session) != NULL) {
            mailboxd_session_write_line(session,
                "Already in a game. /hm board or wait for finish + /hm clear");
            return MAILBOXD_ERR_DENIED;
        }
        g = hangman_create_game(session);
        if (g == NULL) {
            mailboxd_session_write_line(session,
                "No free game slots. /hm clear to free finished games.");
            return MAILBOXD_ERR_DENIED;
        }
        snprintf(line, sizeof(line), "Game %s started. Guess the word!",
                 g->game_id);
        mailboxd_session_write_line(session, line);
        hangman_send_board(g, session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "board") || ieq(sub, "show")) {
        hangman_game_t *g = hangman_find_player_game(session);

        if (g == NULL) {
            mailboxd_session_write_line(session,
                "No active game. /hm new to start.");
            return MAILBOXD_ERR_NOT_FOUND;
        }
        hangman_send_board(g, session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "word") || ieq(sub, "w")) {
        hangman_game_t *g;
        const char *w;

        if (cmd->argc < 2) {
            mailboxd_session_write_line(session,
                "Usage: /hm word <word>");
            return MAILBOXD_ERR_INVALID;
        }
        g = hangman_find_player_game(session);
        if (g == NULL || g->result != HANGMAN_PLAYING) {
            mailboxd_session_write_line(session, "No active game. /hm new");
            return MAILBOXD_ERR_NOT_FOUND;
        }

        w = cmd->argv[1];
        if (hangman_guess_word(g, w)) {
            hangman_send_board(g, session);
            return MAILBOXD_OK;
        }
        hangman_send_board(g, session);
        return MAILBOXD_OK;
    }

    if (ieq(sub, "clear") || ieq(sub, "reset")) {
        unsigned n = hangman_clear_finished();

        snprintf(line, sizeof(line), "Cleared %u finished game(s).", n);
        mailboxd_session_write_line(session, line);
        return MAILBOXD_OK;
    }

    /* Single letter guess: /hm a  or  /hm e */
    if (strlen(sub) == 1 && isalpha((unsigned char)sub[0])) {
        hangman_game_t *g = hangman_find_player_game(session);
        char letter = sub[0];
        int rc;

        if (g == NULL || g->result != HANGMAN_PLAYING) {
            mailboxd_session_write_line(session, "No active game. /hm new");
            return MAILBOXD_ERR_NOT_FOUND;
        }

        rc = hangman_guess_letter(g, letter);
        if (rc == -1) {
            mailboxd_session_write_line(session, "Please guess A-Z.");
            return MAILBOXD_ERR_INVALID;
        }
        if (rc == -2) {
            mailboxd_session_write_line(session, "Already guessed.");
            return MAILBOXD_ERR_DENIED;
        }
        hangman_send_board(g, session);
        return MAILBOXD_OK;
    }

    mailboxd_session_write_line(session, "Unknown hangman command. /hm help");
    return MAILBOXD_ERR_NOT_FOUND;
}