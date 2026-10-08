/*
 * Entertain plugin — chess (internal)
 * Chess for MailboxD Entertain area (Main only).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ENTERTAIN_CHESS_H
#define ENTERTAIN_CHESS_H

#include "mailboxd/types.h"

#include <stddef.h>

struct mailboxd_session;
struct mailboxd_service;

#define CHESS_BOARD_SIZE    8
#define CHESS_GAME_MAX      16
#define CHESS_SPECTATOR_MAX 32
#define CHESS_REFRESH_SEC   30u
#define CHESS_DEFAULT_MAX   8u
#define CHESS_NAME_MAX      32
#define CHESS_SAN_MAX       16
#define CHESS_EMPTY         '.'

typedef enum {
    CHESS_COLOR_NONE = 0,
    CHESS_WHITE,
    CHESS_BLACK
} chess_color_t;

typedef enum {
    CHESS_RESULT_NONE = 0,
    CHESS_WHITE_WINS,
    CHESS_BLACK_WINS,
    CHESS_DRAW_STALEMATE,
    CHESS_DRAW_AGREEMENT
} chess_result_t;

typedef struct chess_board {
    char sq[CHESS_BOARD_SIZE][CHESS_BOARD_SIZE];
} chess_board_t;

typedef struct chess_game {
    int                 active;
    char                white_name[CHESS_NAME_MAX];
    char                black_name[CHESS_NAME_MAX];
    struct mailboxd_session *white_session;
    struct mailboxd_session *black_session;
    struct mailboxd_session *spectators[CHESS_SPECTATOR_MAX];
    unsigned            spectator_count;
    chess_board_t       board;
    chess_color_t       turn;
    unsigned            move_number;
    chess_result_t      result;
    char                last_move_san[CHESS_SAN_MAX];
    unsigned            tick_counter;
    char                game_id[8];
    chess_color_t       draw_offered_by; /* NONE = no pending offer */
} chess_game_t;

/* Lifecycle */
void chess_init(unsigned max_games);
void chess_shutdown(void);
unsigned chess_max_games(void);

/* Game management */
chess_game_t *chess_create_game(struct mailboxd_session *white, const char *name);
chess_game_t *chess_find_open_game(void);
chess_game_t *chess_find_game_by_id(const char *id);
chess_game_t *chess_get_game(unsigned index);
chess_game_t *chess_find_player_game(const struct mailboxd_session *s);
int  chess_join_game(chess_game_t *g, struct mailboxd_session *black, const char *name);
int  chess_add_spectator(chess_game_t *g, struct mailboxd_session *spec);
void chess_remove_spectator(chess_game_t *g, struct mailboxd_session *spec);
int  chess_is_player(const chess_game_t *g, const struct mailboxd_session *s);
int  chess_is_spectator(const chess_game_t *g, const struct mailboxd_session *s);
chess_color_t chess_player_color(const chess_game_t *g, const struct mailboxd_session *s);
void chess_release_game(chess_game_t *g);
unsigned chess_clear_finished(void);

/* Moves + display */
int  chess_make_move(chess_game_t *g, const char *from, const char *to,
                     char promote, char *err, size_t errlen);
void chess_send_board(chess_game_t *g, struct mailboxd_session *s);
void chess_broadcast_board(chess_game_t *g);
void chess_resign(chess_game_t *g, struct mailboxd_session *who);
/** Returns 1 if draw agreed, 0 if offer pending / rejected. */
int  chess_offer_draw(chess_game_t *g, struct mailboxd_session *who);

/* Tick — call from plugin tick every ~1s */
void chess_tick(struct mailboxd_service *service);

#endif /* ENTERTAIN_CHESS_H */
