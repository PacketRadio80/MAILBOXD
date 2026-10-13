/*
 * Entertain plugin — connect four (internal)
 * Connect Four for MailboxD Entertain area (multiplayer).
 * V3 convention: X = uppercase (Rot), o = lowercase (Gelb).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ENTERTAIN_C4_H
#define ENTERTAIN_C4_H

#include "mailboxd/types.h"

struct mailboxd_session;
struct mailboxd_service;

#define C4_COLS           7
#define C4_ROWS           6
#define C4_GAME_MAX       16
#define C4_SPECTATOR_MAX  32
#define C4_REFRESH_SEC    30u
#define C4_DEFAULT_MAX    8u
#define C4_NAME_MAX       32

typedef enum {
    C4_RESULT_NONE = 0,
    C4_RED_WINS,
    C4_YELLOW_WINS,
    C4_DRAW
} c4_result_t;

typedef enum {
    C4_TURN_RED = 1,
    C4_TURN_YELLOW = 0
} c4_turn_t;

typedef struct c4_game {
    int                 active;
    char                red_name[C4_NAME_MAX];
    char                yellow_name[C4_NAME_MAX];
    struct mailboxd_session *red_session;
    struct mailboxd_session *yellow_session;
    struct mailboxd_session *spectators[C4_SPECTATOR_MAX];
    unsigned            spectator_count;
    char                board[C4_ROWS][C4_COLS];
    c4_turn_t           turn;
    unsigned            move_count;
    c4_result_t         result;
    unsigned            tick_counter;
    char                game_id[8];
} c4_game_t;

/* Lifecycle */
void c4_init(unsigned max_games);
void c4_shutdown(void);
unsigned c4_max_games(void);

/* Game management */
c4_game_t *c4_create_game(struct mailboxd_session *red, const char *name);
c4_game_t *c4_find_open_game(void);
c4_game_t *c4_find_game_by_id(const char *id);
c4_game_t *c4_get_game(unsigned index);
c4_game_t *c4_find_player_game(const struct mailboxd_session *s);
int  c4_join_game(c4_game_t *g, struct mailboxd_session *yellow, const char *name);
int  c4_add_spectator(c4_game_t *g, struct mailboxd_session *spec);
void c4_remove_spectator(c4_game_t *g, struct mailboxd_session *spec);
int  c4_is_player(const c4_game_t *g, const struct mailboxd_session *s);
int  c4_is_spectator(const c4_game_t *g, const struct mailboxd_session *s);
void c4_release_game(c4_game_t *g);
unsigned c4_clear_finished(void);

/* Moves + display */
int  c4_make_move(c4_game_t *g, unsigned col,
                  char *err, size_t errlen);
void c4_send_board(c4_game_t *g, struct mailboxd_session *s);
void c4_broadcast_board(c4_game_t *g);
void c4_resign(c4_game_t *g, struct mailboxd_session *who);

/* Tick */
void c4_tick(struct mailboxd_service *service);

#endif /* ENTERTAIN_C4_H */