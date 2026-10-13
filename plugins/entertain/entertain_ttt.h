/*
 * Entertain plugin — tic-tac-toe (internal)
 * Tic-Tac-Toe for MailboxD Entertain area (multiplayer).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ENTERTAIN_TTT_H
#define ENTERTAIN_TTT_H

#include "mailboxd/types.h"

struct mailboxd_session;
struct mailboxd_service;

#define TTT_BOARD_SIZE    3
#define TTT_GAME_MAX      16
#define TTT_SPECTATOR_MAX 32
#define TTT_REFRESH_SEC   30u
#define TTT_DEFAULT_MAX   8u
#define TTT_NAME_MAX      32

typedef enum {
    TTT_RESULT_NONE = 0,
    TTT_X_WINS,
    TTT_O_WINS,
    TTT_DRAW
} ttt_result_t;

typedef enum {
    TTT_TURN_X = 1,
    TTT_TURN_O = 0
} ttt_turn_t;

typedef struct ttt_game {
    int                 active;
    char                x_name[TTT_NAME_MAX];
    char                o_name[TTT_NAME_MAX];
    struct mailboxd_session *x_session;
    struct mailboxd_session *o_session;
    struct mailboxd_session *spectators[TTT_SPECTATOR_MAX];
    unsigned            spectator_count;
    char                board[TTT_BOARD_SIZE][TTT_BOARD_SIZE];
    ttt_turn_t          turn;
    unsigned            move_count;
    ttt_result_t        result;
    unsigned            tick_counter;
    char                game_id[8];
} ttt_game_t;

/* Lifecycle */
void ttt_init(unsigned max_games);
void ttt_shutdown(void);
unsigned ttt_max_games(void);

/* Game management */
ttt_game_t *ttt_create_game(struct mailboxd_session *x, const char *name);
ttt_game_t *ttt_find_open_game(void);
ttt_game_t *ttt_find_game_by_id(const char *id);
ttt_game_t *ttt_get_game(unsigned index);
ttt_game_t *ttt_find_player_game(const struct mailboxd_session *s);
int  ttt_join_game(ttt_game_t *g, struct mailboxd_session *o, const char *name);
int  ttt_add_spectator(ttt_game_t *g, struct mailboxd_session *spec);
void ttt_remove_spectator(ttt_game_t *g, struct mailboxd_session *spec);
int  ttt_is_player(const ttt_game_t *g, const struct mailboxd_session *s);
int  ttt_is_spectator(const ttt_game_t *g, const struct mailboxd_session *s);
void ttt_release_game(ttt_game_t *g);
unsigned ttt_clear_finished(void);

/* Moves + display */
int  ttt_make_move(ttt_game_t *g, unsigned row, unsigned col,
                   char *err, size_t errlen);
void ttt_send_board(ttt_game_t *g, struct mailboxd_session *s);
void ttt_broadcast_board(ttt_game_t *g);
void ttt_resign(ttt_game_t *g, struct mailboxd_session *who);

/* Tick */
void ttt_tick(struct mailboxd_service *service);

#endif /* ENTERTAIN_TTT_H */