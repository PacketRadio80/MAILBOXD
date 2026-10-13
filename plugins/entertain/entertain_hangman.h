/*
 * Entertain plugin — hangman (internal)
 * Hangman for MailboxD Entertain area (single-player).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ENTERTAIN_HANGMAN_H
#define ENTERTAIN_HANGMAN_H

#include "mailboxd/types.h"

struct mailboxd_session;
struct mailboxd_service;

#define HANGMAN_MAX_WRONG    7
#define HANGMAN_WORD_MAX     32
#define HANGMAN_WRONG_MAX    26
#define HANGMAN_SESSION_MAX  32
#define HANGMAN_DEFAULT_MAX  16u
#define HANGMAN_GALLOWS_ROWS 6

typedef enum {
    HANGMAN_PLAYING = 0,
    HANGMAN_WON,
    HANGMAN_LOST
} hangman_result_t;

typedef struct hangman_game {
    int                 active;
    char                word[HANGMAN_WORD_MAX];
    char                display[HANGMAN_WORD_MAX];
    char                wrong[HANGMAN_WRONG_MAX + 1];
    unsigned            wrong_count;
    hangman_result_t    result;
    struct mailboxd_session *session;
    char                game_id[8];
} hangman_game_t;

/* Lifecycle */
void hangman_init(unsigned max_games);
void hangman_shutdown(void);
unsigned hangman_max_games(void);

/* Game management */
hangman_game_t *hangman_create_game(struct mailboxd_session *s);
hangman_game_t *hangman_find_player_game(const struct mailboxd_session *s);
hangman_game_t *hangman_get_game(unsigned index);
void hangman_release_game(hangman_game_t *g);
unsigned hangman_clear_finished(void);

/* Gameplay */
int  hangman_guess_letter(hangman_game_t *g, char letter);
int  hangman_guess_word(hangman_game_t *g, const char *word);
void hangman_send_board(hangman_game_t *g, struct mailboxd_session *s);

/* Tick */
void hangman_tick(struct mailboxd_service *service);

#endif /* ENTERTAIN_HANGMAN_H */