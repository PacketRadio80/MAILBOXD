/*
 * Entertain plugin — chess.c
 * Board, move legality (no castling / en passant), checkmate, SAN, tick.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "entertain_chess.h"
#include "mailboxd/session.h"
#include "mailboxd/log.h"
#include "mailboxd/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static chess_game_t g_games[CHESS_GAME_MAX];
static unsigned g_max_games = CHESS_DEFAULT_MAX;

/* ── Board init ────────────────────────────────────────────── */

static void board_init(chess_board_t *b)
{
    static const char bk[] = "RNBQKBNR";
    unsigned f;

    memset(b, 0, sizeof(*b));
    for (f = 0; f < 8; f++) {
        b->sq[0][f] = bk[f];
        b->sq[1][f] = 'P';
        b->sq[6][f] = 'p';
        b->sq[7][f] = (char)tolower((unsigned char)bk[f]);
    }
    for (unsigned r = 2; r < 6; r++) {
        for (f = 0; f < 8; f++) {
            b->sq[r][f] = CHESS_EMPTY;
        }
    }
}

/* ── Lifecycle ─────────────────────────────────────────────── */

void chess_init(unsigned max_games)
{
    unsigned i;

    memset(g_games, 0, sizeof(g_games));
    if (max_games == 0 || max_games > CHESS_GAME_MAX) {
        max_games = CHESS_DEFAULT_MAX;
    }
    g_max_games = max_games;
    for (i = 0; i < CHESS_GAME_MAX; i++) {
        snprintf(g_games[i].game_id, sizeof(g_games[i].game_id), "G%u", i + 1);
        g_games[i].active = 0;
    }
}

void chess_shutdown(void)
{
    unsigned i;

    for (i = 0; i < CHESS_GAME_MAX; i++) {
        g_games[i].active = 0;
    }
}

unsigned chess_max_games(void)
{
    return g_max_games;
}

/* ── Helpers ───────────────────────────────────────────────── */

static int parse_sq(const char *s, unsigned *r, unsigned *f)
{
    char c0;
    char c1;

    if (s == NULL || s[0] == '\0' || s[1] == '\0') {
        return 0;
    }
    c0 = (char)tolower((unsigned char)s[0]);
    c1 = s[1];
    if (c0 < 'a' || c0 > 'h' || c1 < '1' || c1 > '8') {
        return 0;
    }
    *f = (unsigned)(c0 - 'a');
    *r = (unsigned)(c1 - '1');
    return 1;
}

static chess_color_t pcol(char p)
{
    if (p == CHESS_EMPTY) {
        return CHESS_COLOR_NONE;
    }
    return (p >= 'A' && p <= 'Z') ? CHESS_WHITE : CHESS_BLACK;
}

static char pup(char p)
{
    return (char)toupper((unsigned char)p);
}

static int inb(unsigned r, unsigned f)
{
    return r < 8 && f < 8;
}

static const char *color_name(chess_color_t c)
{
    if (c == CHESS_WHITE) {
        return "White";
    }
    if (c == CHESS_BLACK) {
        return "Black";
    }
    return "?";
}

/* ── Check detection ───────────────────────────────────────── */

static int sq_attacked(const chess_board_t *b, unsigned rk, unsigned fl,
                       chess_color_t by)
{
    unsigned r;
    unsigned f;
    char P;
    char N;
    char B;
    char R;
    char Q;
    char K;

    if (by == CHESS_WHITE) {
        P = 'P'; N = 'N'; B = 'B'; R = 'R'; Q = 'Q'; K = 'K';
    } else {
        P = 'p'; N = 'n'; B = 'b'; R = 'r'; Q = 'q'; K = 'k';
    }

    if (by == CHESS_WHITE) {
        if (rk > 0 && fl > 0 && b->sq[rk - 1][fl - 1] == P) {
            return 1;
        }
        if (rk > 0 && fl < 7 && b->sq[rk - 1][fl + 1] == P) {
            return 1;
        }
    } else {
        if (rk < 7 && fl > 0 && b->sq[rk + 1][fl - 1] == P) {
            return 1;
        }
        if (rk < 7 && fl < 7 && b->sq[rk + 1][fl + 1] == P) {
            return 1;
        }
    }

    {
        static const int kr[] = {2, 2, -2, -2, 1, 1, -1, -1};
        static const int kf[] = {1, -1, 1, -1, 2, -2, 2, -2};
        unsigned i;

        for (i = 0; i < 8; i++) {
            int nr = (int)rk + kr[i];
            int nf = (int)fl + kf[i];

            if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8 &&
                b->sq[nr][nf] == N) {
                return 1;
            }
        }
    }

    {
        static const int dr[] = {1, 1, -1, -1};
        static const int df[] = {1, -1, 1, -1};
        unsigned d;

        for (d = 0; d < 4; d++) {
            r = rk;
            f = fl;
            for (;;) {
                r = (unsigned)((int)r + dr[d]);
                f = (unsigned)((int)f + df[d]);
                if (!inb(r, f)) {
                    break;
                }
                if (b->sq[r][f] != CHESS_EMPTY) {
                    if (b->sq[r][f] == B || b->sq[r][f] == Q) {
                        return 1;
                    }
                    break;
                }
            }
        }
    }

    {
        static const int dr[] = {1, -1, 0, 0};
        static const int df[] = {0, 0, 1, -1};
        unsigned d;

        for (d = 0; d < 4; d++) {
            r = rk;
            f = fl;
            for (;;) {
                r = (unsigned)((int)r + dr[d]);
                f = (unsigned)((int)f + df[d]);
                if (!inb(r, f)) {
                    break;
                }
                if (b->sq[r][f] != CHESS_EMPTY) {
                    if (b->sq[r][f] == R || b->sq[r][f] == Q) {
                        return 1;
                    }
                    break;
                }
            }
        }
    }

    {
        static const int dr[] = {1, 1, 1, 0, 0, -1, -1, -1};
        static const int df[] = {1, 0, -1, 1, -1, 1, 0, -1};
        unsigned d;

        for (d = 0; d < 8; d++) {
            int nr = (int)rk + dr[d];
            int nf = (int)fl + df[d];

            if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8 &&
                b->sq[nr][nf] == K) {
                return 1;
            }
        }
    }

    return 0;
}

static int find_king(const chess_board_t *b, chess_color_t c,
                     unsigned *kr, unsigned *kf)
{
    char k = (c == CHESS_WHITE) ? 'K' : 'k';
    unsigned r;
    unsigned f;

    for (r = 0; r < 8; r++) {
        for (f = 0; f < 8; f++) {
            if (b->sq[r][f] == k) {
                *kr = r;
                *kf = f;
                return 1;
            }
        }
    }
    return 0;
}

static int in_check(const chess_board_t *b, chess_color_t side)
{
    unsigned kr;
    unsigned kf;
    chess_color_t opp = (side == CHESS_WHITE) ? CHESS_BLACK : CHESS_WHITE;

    if (!find_king(b, side, &kr, &kf)) {
        return 0;
    }
    return sq_attacked(b, kr, kf, opp);
}

/* ── Move legality ─────────────────────────────────────────── */

static int pseudo_pawn(const chess_board_t *b, unsigned fr, unsigned ff,
                       unsigned tr, unsigned tf, chess_color_t side)
{
    int dir = (side == CHESS_WHITE) ? 1 : -1;
    int start = (side == CHESS_WHITE) ? 1 : 6;
    int dr = (int)tr - (int)fr;
    int df = (int)tf - (int)ff;

    if (df == 0) {
        if (dr == dir && b->sq[tr][tf] == CHESS_EMPTY) {
            return 1;
        }
        if (dr == 2 * dir && (int)fr == start &&
            b->sq[fr + (unsigned)dir][ff] == CHESS_EMPTY &&
            b->sq[tr][tf] == CHESS_EMPTY) {
            return 1;
        }
    }
    if ((df == 1 || df == -1) && dr == dir &&
        b->sq[tr][tf] != CHESS_EMPTY) {
        return 1;
    }
    return 0;
}

static int pseudo_knight(unsigned fr, unsigned ff, unsigned tr, unsigned tf)
{
    int dr = abs((int)tr - (int)fr);
    int df = abs((int)tf - (int)ff);

    return (dr == 2 && df == 1) || (dr == 1 && df == 2);
}

static int slide_clear(const chess_board_t *b, unsigned fr, unsigned ff,
                       unsigned tr, unsigned tf)
{
    int dr = 0;
    int df = 0;
    unsigned r;
    unsigned f;

    if ((int)tr > (int)fr) {
        dr = 1;
    } else if ((int)tr < (int)fr) {
        dr = -1;
    }
    if ((int)tf > (int)ff) {
        df = 1;
    } else if ((int)tf < (int)ff) {
        df = -1;
    }
    r = (unsigned)((int)fr + dr);
    f = (unsigned)((int)ff + df);
    while (r != tr || f != tf) {
        if (!inb(r, f) || b->sq[r][f] != CHESS_EMPTY) {
            return 0;
        }
        r = (unsigned)((int)r + dr);
        f = (unsigned)((int)f + df);
    }
    return 1;
}

static int pseudo_legal(const chess_board_t *b, unsigned fr, unsigned ff,
                        unsigned tr, unsigned tf, chess_color_t side)
{
    char pc = b->sq[fr][ff];
    int dr = abs((int)tr - (int)fr);
    int df = abs((int)tf - (int)ff);

    if (pc == CHESS_EMPTY || pcol(pc) != side) {
        return 0;
    }
    if (pcol(b->sq[tr][tf]) == side) {
        return 0;
    }
    switch (pup(pc)) {
    case 'P':
        return pseudo_pawn(b, fr, ff, tr, tf, side);
    case 'N':
        return pseudo_knight(fr, ff, tr, tf);
    case 'B':
        return dr == df && dr > 0 && slide_clear(b, fr, ff, tr, tf);
    case 'R':
        return (fr == tr || ff == tf) && slide_clear(b, fr, ff, tr, tf);
    case 'Q':
        return ((fr == tr || ff == tf) || (dr == df)) &&
               slide_clear(b, fr, ff, tr, tf);
    case 'K':
        return dr <= 1 && df <= 1 && (dr + df > 0);
    default:
        break;
    }
    return 0;
}

static int is_legal(chess_board_t *b, unsigned fr, unsigned ff,
                    unsigned tr, unsigned tf, chess_color_t side)
{
    chess_board_t tmp;

    if (!pseudo_legal(b, fr, ff, tr, tf, side)) {
        return 0;
    }
    memcpy(&tmp, b, sizeof(tmp));
    tmp.sq[tr][tf] = tmp.sq[fr][ff];
    tmp.sq[fr][ff] = CHESS_EMPTY;
    if (pup(tmp.sq[tr][tf]) == 'P' &&
        ((side == CHESS_WHITE && tr == 7) ||
         (side == CHESS_BLACK && tr == 0))) {
        tmp.sq[tr][tf] = (side == CHESS_WHITE) ? 'Q' : 'q';
    }
    return !in_check(&tmp, side);
}

static int has_any_legal(chess_board_t *b, chess_color_t side)
{
    unsigned fr;
    unsigned ff;
    unsigned tr;
    unsigned tf;

    for (fr = 0; fr < 8; fr++) {
        for (ff = 0; ff < 8; ff++) {
            if (pcol(b->sq[fr][ff]) != side) {
                continue;
            }
            for (tr = 0; tr < 8; tr++) {
                for (tf = 0; tf < 8; tf++) {
                    if (is_legal(b, fr, ff, tr, tf, side)) {
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

/* ── SAN ───────────────────────────────────────────────────── */

static void make_san(char *out, size_t len, char pc, unsigned ff,
                     unsigned tr, unsigned tf, int cap, int chk,
                     int mate, char promo)
{
    int n;

    if (pup(pc) == 'P') {
        if (cap) {
            n = snprintf(out, len, "%cx%c%d", 'a' + (int)ff,
                         'a' + (int)tf, (int)(tr + 1));
        } else {
            n = snprintf(out, len, "%c%d", 'a' + (int)tf, (int)(tr + 1));
        }
    } else {
        n = snprintf(out, len, "%c%c%d", pup(pc), 'a' + (int)tf,
                     (int)(tr + 1));
    }
    if (n < 0) {
        n = 0;
    }
    if (promo) {
        snprintf(out + n, len - (size_t)n, "=%c", pup(promo));
    } else if (mate) {
        snprintf(out + n, len - (size_t)n, "#");
    } else if (chk) {
        snprintf(out + n, len - (size_t)n, "+");
    }
}

/* ── Game management ───────────────────────────────────────── */

chess_game_t *chess_get_game(unsigned i)
{
    if (i >= g_max_games || !g_games[i].active) {
        return NULL;
    }
    return &g_games[i];
}

void chess_release_game(chess_game_t *g)
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

unsigned chess_clear_finished(void)
{
    unsigned i;
    unsigned n = 0;

    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active && g_games[i].result != CHESS_RESULT_NONE) {
            chess_release_game(&g_games[i]);
            n++;
        }
    }
    return n;
}

chess_game_t *chess_create_game(struct mailboxd_session *white, const char *name)
{
    unsigned i;
    chess_game_t *slot = NULL;

    for (i = 0; i < g_max_games; i++) {
        if (!g_games[i].active) {
            slot = &g_games[i];
            break;
        }
        if (g_games[i].result != CHESS_RESULT_NONE) {
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
    mailboxd_strlcpy(slot->white_name, name != NULL ? name : "?", CHESS_NAME_MAX);
    slot->white_session = white;
    board_init(&slot->board);
    slot->turn = CHESS_WHITE;
    slot->move_number = 1;
    mailboxd_log_info("[chess] game %s created by %s", slot->game_id,
                   slot->white_name);
    return slot;
}

chess_game_t *chess_find_open_game(void)
{
    unsigned i;

    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active && g_games[i].black_session == NULL &&
            g_games[i].result == CHESS_RESULT_NONE) {
            return &g_games[i];
        }
    }
    return NULL;
}

chess_game_t *chess_find_game_by_id(const char *id)
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

chess_game_t *chess_find_player_game(const struct mailboxd_session *s)
{
    unsigned i;

    if (s == NULL) {
        return NULL;
    }
    for (i = 0; i < g_max_games; i++) {
        if (g_games[i].active && chess_is_player(&g_games[i], s)) {
            return &g_games[i];
        }
    }
    return NULL;
}

int chess_join_game(chess_game_t *g, struct mailboxd_session *black,
                    const char *name)
{
    if (g == NULL || black == NULL || g->black_session != NULL ||
        g->result != CHESS_RESULT_NONE) {
        return 0;
    }
    if (g->white_session == black) {
        return 0;
    }
    g->black_session = black;
    mailboxd_strlcpy(g->black_name, name != NULL ? name : "?", CHESS_NAME_MAX);
    mailboxd_log_info("[chess] %s joined %s as black", g->black_name, g->game_id);
    return 1;
}

int chess_add_spectator(chess_game_t *g, struct mailboxd_session *spec)
{
    unsigned i;

    if (g == NULL || spec == NULL) {
        return 0;
    }
    if (chess_is_player(g, spec)) {
        return 0;
    }
    for (i = 0; i < g->spectator_count; i++) {
        if (g->spectators[i] == spec) {
            return 1;
        }
    }
    if (g->spectator_count >= CHESS_SPECTATOR_MAX) {
        return 0;
    }
    g->spectators[g->spectator_count++] = spec;
    return 1;
}

void chess_remove_spectator(chess_game_t *g, struct mailboxd_session *spec)
{
    unsigned i;

    if (g == NULL || spec == NULL) {
        return;
    }
    for (i = 0; i < g->spectator_count; i++) {
        if (g->spectators[i] == spec) {
            g->spectators[i] = g->spectators[--g->spectator_count];
            return;
        }
    }
}

int chess_is_player(const chess_game_t *g, const struct mailboxd_session *s)
{
    return g != NULL && s != NULL &&
           (g->white_session == s || g->black_session == s);
}

int chess_is_spectator(const chess_game_t *g, const struct mailboxd_session *s)
{
    unsigned i;

    if (g == NULL || s == NULL) {
        return 0;
    }
    for (i = 0; i < g->spectator_count; i++) {
        if (g->spectators[i] == s) {
            return 1;
        }
    }
    return 0;
}

chess_color_t chess_player_color(const chess_game_t *g,
                                 const struct mailboxd_session *s)
{
    if (g == NULL || s == NULL) {
        return CHESS_COLOR_NONE;
    }
    if (g->white_session == s) {
        return CHESS_WHITE;
    }
    if (g->black_session == s) {
        return CHESS_BLACK;
    }
    return CHESS_COLOR_NONE;
}

/* ── Move execution ────────────────────────────────────────── */

int chess_make_move(chess_game_t *g, const char *from, const char *to,
                    char promote, char *err, size_t errlen)
{
    unsigned fr;
    unsigned ff;
    unsigned tr;
    unsigned tf;
    char piece;
    char captured;
    int check;
    int mate;
    chess_color_t side;
    chess_color_t opp;
    char promo_piece = 0;

    if (g == NULL || g->result != CHESS_RESULT_NONE) {
        if (err != NULL) {
            snprintf(err, errlen, "Game is over");
        }
        return 0;
    }
    if (g->black_session == NULL) {
        if (err != NULL) {
            snprintf(err, errlen, "Waiting for Black");
        }
        return 0;
    }

    side = g->turn;
    if (!parse_sq(from, &fr, &ff)) {
        if (err != NULL) {
            snprintf(err, errlen, "Bad square '%s'", from != NULL ? from : "");
        }
        return 0;
    }
    if (!parse_sq(to, &tr, &tf)) {
        if (err != NULL) {
            snprintf(err, errlen, "Bad square '%s'", to != NULL ? to : "");
        }
        return 0;
    }

    piece = g->board.sq[fr][ff];
    if (pcol(piece) != side) {
        if (err != NULL) {
            snprintf(err, errlen, "Not your piece");
        }
        return 0;
    }
    if (!is_legal(&g->board, fr, ff, tr, tf, side)) {
        if (err != NULL) {
            snprintf(err, errlen, "Illegal move");
        }
        return 0;
    }

    captured = g->board.sq[tr][tf];
    g->board.sq[tr][tf] = piece;
    g->board.sq[fr][ff] = CHESS_EMPTY;

    if (pup(piece) == 'P' &&
        ((side == CHESS_WHITE && tr == 7) ||
         (side == CHESS_BLACK && tr == 0))) {
        char pp = promote ? pup(promote) : 'Q';

        if (pp != 'Q' && pp != 'R' && pp != 'B' && pp != 'N') {
            pp = 'Q';
        }
        promo_piece = pp;
        g->board.sq[tr][tf] =
            (side == CHESS_WHITE) ? pp : (char)tolower((unsigned char)pp);
    }

    opp = (side == CHESS_WHITE) ? CHESS_BLACK : CHESS_WHITE;
    check = in_check(&g->board, opp);
    mate = check && !has_any_legal(&g->board, opp);

    make_san(g->last_move_san, sizeof(g->last_move_san), piece, ff, tr, tf,
             captured != CHESS_EMPTY, check, mate, promo_piece);

    if (mate) {
        g->result = (side == CHESS_WHITE) ? CHESS_WHITE_WINS : CHESS_BLACK_WINS;
    } else if (!has_any_legal(&g->board, opp)) {
        g->result = CHESS_DRAW_STALEMATE;
    }

    g->turn = opp;
    if (opp == CHESS_WHITE) {
        g->move_number++;
    }
    g->tick_counter = 0;
    g->draw_offered_by = CHESS_COLOR_NONE;

    mailboxd_log_info("[chess] %s: %s %s", g->game_id,
                   (side == CHESS_WHITE) ? g->white_name : g->black_name,
                   g->last_move_san);
    return 1;
}

/* ── Resign / Draw ─────────────────────────────────────────── */

void chess_resign(chess_game_t *g, struct mailboxd_session *who)
{
    if (g == NULL || g->result != CHESS_RESULT_NONE) {
        return;
    }
    if (g->white_session == who) {
        g->result = CHESS_BLACK_WINS;
    } else if (g->black_session == who) {
        g->result = CHESS_WHITE_WINS;
    }
    g->draw_offered_by = CHESS_COLOR_NONE;
}

int chess_offer_draw(chess_game_t *g, struct mailboxd_session *who)
{
    chess_color_t me;

    if (g == NULL || who == NULL || g->result != CHESS_RESULT_NONE) {
        return 0;
    }
    me = chess_player_color(g, who);
    if (me == CHESS_COLOR_NONE) {
        return 0;
    }
    if (g->draw_offered_by != CHESS_COLOR_NONE &&
        g->draw_offered_by != me) {
        g->result = CHESS_DRAW_AGREEMENT;
        g->draw_offered_by = CHESS_COLOR_NONE;
        return 1;
    }
    g->draw_offered_by = me;
    return 0;
}

/* ── Board rendering ───────────────────────────────────────── */

static void render_board(const chess_board_t *b, struct mailboxd_session *s,
                         const char *label)
{
    char line[96];
    unsigned r;
    unsigned f;

    if (label != NULL && label[0] != '\0') {
        mailboxd_session_write_line(s, label);
    }
    mailboxd_session_write_line(s, "  a b c d e f g h");
    for (r = 0; r < 8; r++) {
        unsigned rk = 7 - r;
        int n = snprintf(line, sizeof(line), "%u ", rk + 1);

        for (f = 0; f < 8; f++) {
            char p = b->sq[rk][f];
            char ch = (p == CHESS_EMPTY) ? '.' : p;

            n += snprintf(line + n, sizeof(line) - (size_t)n, "%c ", ch);
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%u", rk + 1);
        mailboxd_session_write_line(s, line);
    }
    mailboxd_session_write_line(s, "  a b c d e f g h");
    mailboxd_session_write_line(s, "  (White=UPPER  Black=lower)");
}

void chess_send_board(chess_game_t *g, struct mailboxd_session *s)
{
    char label[192];
    const char *turn_name;

    if (g == NULL || s == NULL) {
        return;
    }
    turn_name = (g->turn == CHESS_WHITE) ? g->white_name : g->black_name;
    if (g->result != CHESS_RESULT_NONE) {
        const char *rs;

        switch (g->result) {
        case CHESS_WHITE_WINS:
            rs = "1-0 White wins";
            break;
        case CHESS_BLACK_WINS:
            rs = "0-1 Black wins";
            break;
        case CHESS_DRAW_STALEMATE:
            rs = "1/2-1/2 Stalemate";
            break;
        case CHESS_DRAW_AGREEMENT:
            rs = "1/2-1/2 Draw";
            break;
        default:
            rs = "Over";
            break;
        }
        snprintf(label, sizeof(label), "[%s] %s vs %s  %s  Move %u",
                 g->game_id, g->white_name,
                 g->black_session ? g->black_name : "...", rs, g->move_number);
    } else {
        snprintf(label, sizeof(label),
                 "[%s] %s(W) vs %s(B)  %s to move%s%s  Move %u",
                 g->game_id, g->white_name,
                 g->black_session ? g->black_name : "...", turn_name,
                 g->last_move_san[0] ? "  last=" : "",
                 g->last_move_san[0] ? g->last_move_san : "",
                 g->move_number);
    }
    render_board(&g->board, s, label);
    if (g->draw_offered_by != CHESS_COLOR_NONE &&
        g->result == CHESS_RESULT_NONE) {
        snprintf(label, sizeof(label), "Draw offered by %s — /chess draw to accept",
                 color_name(g->draw_offered_by));
        mailboxd_session_write_line(s, label);
    }
}

void chess_broadcast_board(chess_game_t *g)
{
    unsigned j;

    if (g == NULL) {
        return;
    }
    if (g->white_session != NULL) {
        chess_send_board(g, g->white_session);
    }
    if (g->black_session != NULL) {
        chess_send_board(g, g->black_session);
    }
    for (j = 0; j < g->spectator_count; j++) {
        chess_send_board(g, g->spectators[j]);
    }
}

/* ── Tick — board refresh for spectators ───────────────────── */

void chess_tick(struct mailboxd_service *service)
{
    unsigned i;
    unsigned j;

    (void)service;
    for (i = 0; i < g_max_games; i++) {
        chess_game_t *g = &g_games[i];

        if (!g->active || g->black_session == NULL ||
            g->result != CHESS_RESULT_NONE || g->spectator_count == 0) {
            continue;
        }
        g->tick_counter++;
        if (g->tick_counter < CHESS_REFRESH_SEC) {
            continue;
        }
        g->tick_counter = 0;
        for (j = 0; j < g->spectator_count; j++) {
            chess_send_board(g, g->spectators[j]);
        }
    }
}
