/**
 * GNostr Chess Engine
 *
 * Based on Micro-Max concepts by H.G. Muller
 * https://home.hccnet.nl/h.g.muller/max-src2.html
 *
 * This implementation provides a clean, thread-safe API with all state
 * encapsulated in the ChessEngine struct.
 *
 * Board representation: 0x88 board
 * Piece encoding:
 * - Bits 0-2: piece type (1=pawn, 3=knight, 4=king, 5=bishop, 6=rook, 7=queen)
 * - Bit 3 (8): white color
 * - Bit 4 (16): black color
 * - Bit 5 (32): piece has moved (for castling rights)
 */

#include "chess_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Hash table size - must be power of 2 minus 8 */
#define HASH_SIZE 65536

/* Piece type constants */
#define PAWN   1
#define KNIGHT 3
#define KING   4
#define BISHOP 5
#define ROOK   6
#define QUEEN  7

/* Color constants */
#define WHITE  8
#define BLACK  16
#define MOVED  32

/* Search constants */
#define INF 30000

/* Hash table entry */
typedef struct {
    unsigned int key;
    int score;
    int depth;
    int best_from;
    int best_to;
    int flags;
} HashEntry;

/* Engine state */
struct ChessEngine {
    HashEntry *hash_table;
    char board[128];         /* 0x88 board */
    int side;                /* Side to move: WHITE or BLACK */
    int ep_square;           /* En passant target square (0 if none) */
    int halfmove;            /* Halfmove clock */
    int fullmove;            /* Fullmove number */
    int nodes;               /* Search node counter */
    int best_from;           /* Best move from square */
    int best_to;             /* Best move to square */
};

/* Piece values for evaluation */
static const int piece_value[] = {0, 100, 0, 320, 0, 330, 500, 900};

/* Knight move offsets */
static const int knight_offsets[] = {-33, -31, -18, -14, 14, 18, 31, 33};

/* King/Queen directions */
static const int king_offsets[] = {-17, -16, -15, -1, 1, 15, 16, 17};

/* Bishop directions (diagonals) */
static const int bishop_offsets[] = {-17, -15, 15, 17};

/* Rook directions (orthogonals) */
static const int rook_offsets[] = {-16, -1, 1, 16};

/* Forward declarations */
static gboolean is_square_attacked(ChessEngine *e, int sq, int by_color);
static int alpha_beta(ChessEngine *e, int alpha, int beta, int depth);

/**
 * Convert algebraic notation to 0x88 square
 */
static int algebraic_to_square(const char *sq)
{
    if (!sq || strlen(sq) < 2)
        return -1;

    int file = tolower(sq[0]) - 'a';
    int rank = sq[1] - '1';

    if (file < 0 || file > 7 || rank < 0 || rank > 7)
        return -1;

    return rank * 16 + file;
}

/**
 * Convert 0x88 square to algebraic notation
 */
static char *square_to_algebraic(int sq)
{
    if (sq < 0 || (sq & 0x88))
        return NULL;

    char *result = g_malloc(3);
    result[0] = 'a' + (sq & 7);
    result[1] = '1' + (sq >> 4);
    result[2] = '\0';
    return result;
}

/**
 * Check if a square is attacked by a given color
 */
static gboolean is_square_attacked(ChessEngine *e, int sq, int by_color)
{
    if (sq < 0 || (sq & 0x88))
        return FALSE;

    /* Knight attacks */
    for (int i = 0; i < 8; i++) {
        int from = sq + knight_offsets[i];
        if (!(from & 0x88)) {
            char piece = e->board[from];
            if ((piece & by_color) && (piece & 7) == KNIGHT)
                return TRUE;
        }
    }

    /* Pawn attacks */
    int pawn_dir = (by_color == WHITE) ? 16 : -16;
    int pawn_caps[] = {pawn_dir - 1, pawn_dir + 1};
    for (int i = 0; i < 2; i++) {
        int from = sq - pawn_caps[i];
        if (!(from & 0x88)) {
            char piece = e->board[from];
            if ((piece & by_color) && (piece & 7) == PAWN)
                return TRUE;
        }
    }

    /* King attacks (adjacent squares) */
    for (int i = 0; i < 8; i++) {
        int from = sq + king_offsets[i];
        if (!(from & 0x88)) {
            char piece = e->board[from];
            if ((piece & by_color) && (piece & 7) == KING)
                return TRUE;
        }
    }

    /* Sliding piece attacks (rook, bishop, queen) */
    for (int i = 0; i < 8; i++) {
        int dir = king_offsets[i];
        int from = sq + dir;
        gboolean is_diagonal = (i == 0 || i == 2 || i == 5 || i == 7);

        while (!(from & 0x88)) {
            char piece = e->board[from];
            if (piece) {
                if (piece & by_color) {
                    int pt = piece & 7;
                    if (pt == QUEEN)
                        return TRUE;
                    if (is_diagonal && pt == BISHOP)
                        return TRUE;
                    if (!is_diagonal && pt == ROOK)
                        return TRUE;
                }
                break;  /* Blocked */
            }
            from += dir;
        }
    }

    return FALSE;
}

/**
 * Find the king square for a given color
 */
static int find_king(ChessEngine *e, int color)
{
    for (int sq = 0; sq < 128; sq++) {
        if (sq & 0x88)
            continue;
        char piece = e->board[sq];
        if ((piece & color) && (piece & 7) == KING)
            return sq;
    }
    return -1;
}

/**
 * Check if the given side is in check
 */
static gboolean is_in_check(ChessEngine *e, int color)
{
    int king_sq = find_king(e, color);
    if (king_sq < 0)
        return FALSE;
    return is_square_attacked(e, king_sq, 24 - color);
}

/**
 * Generate all pseudo-legal moves for the current position
 * Returns a list of (from, to) pairs encoded as from*256 + to
 */
static GList *generate_moves(ChessEngine *e)
{
    GList *moves = NULL;
    int side = e->side;
    int opp = 24 - side;

    for (int from = 0; from < 128; from++) {
        if (from & 0x88)
            continue;

        char piece = e->board[from];
        if (!(piece & side))
            continue;

        int pt = piece & 7;

        if (pt == PAWN) {
            int dir = (side == WHITE) ? 16 : -16;
            int start_rank = (side == WHITE) ? 1 : 6;
            int promo_rank = (side == WHITE) ? 7 : 0;

            /* Single push */
            int to = from + dir;
            if (!(to & 0x88) && !e->board[to]) {
                moves = g_list_append(moves, GINT_TO_POINTER(from * 256 + to));

                /* Double push from starting rank */
                if ((from >> 4) == start_rank) {
                    int to2 = from + 2 * dir;
                    if (!(to2 & 0x88) && !e->board[to2]) {
                        moves = g_list_append(moves, GINT_TO_POINTER(from * 256 + to2));
                    }
                }
            }

            /* Captures */
            int caps[] = {dir - 1, dir + 1};
            for (int c = 0; c < 2; c++) {
                to = from + caps[c];
                if (!(to & 0x88)) {
                    char target = e->board[to];
                    if ((target & opp) || to == e->ep_square) {
                        moves = g_list_append(moves, GINT_TO_POINTER(from * 256 + to));
                    }
                }
            }
        }
        else if (pt == KNIGHT) {
            for (int i = 0; i < 8; i++) {
                int to = from + knight_offsets[i];
                if (!(to & 0x88) && !(e->board[to] & side)) {
                    moves = g_list_append(moves, GINT_TO_POINTER(from * 256 + to));
                }
            }
        }
        else if (pt == KING) {
            for (int i = 0; i < 8; i++) {
                int to = from + king_offsets[i];
                if (!(to & 0x88) && !(e->board[to] & side)) {
                    moves = g_list_append(moves, GINT_TO_POINTER(from * 256 + to));
                }
            }

            /* Castling */
            if (!(piece & MOVED) && !is_in_check(e, side)) {
                /* Kingside */
                int rook_sq = from + 3;
                if (!(rook_sq & 0x88) && (e->board[rook_sq] & 7) == ROOK &&
                    (e->board[rook_sq] & side) && !(e->board[rook_sq] & MOVED) &&
                    !e->board[from + 1] && !e->board[from + 2] &&
                    !is_square_attacked(e, from + 1, opp) &&
                    !is_square_attacked(e, from + 2, opp)) {
                    moves = g_list_append(moves, GINT_TO_POINTER(from * 256 + from + 2));
                }

                /* Queenside */
                rook_sq = from - 4;
                if (!(rook_sq & 0x88) && (e->board[rook_sq] & 7) == ROOK &&
                    (e->board[rook_sq] & side) && !(e->board[rook_sq] & MOVED) &&
                    !e->board[from - 1] && !e->board[from - 2] && !e->board[from - 3] &&
                    !is_square_attacked(e, from - 1, opp) &&
                    !is_square_attacked(e, from - 2, opp)) {
                    moves = g_list_append(moves, GINT_TO_POINTER(from * 256 + from - 2));
                }
            }
        }
        else if (pt == BISHOP || pt == ROOK || pt == QUEEN) {
            const int *dirs;
            int num_dirs;

            if (pt == BISHOP) {
                dirs = bishop_offsets;
                num_dirs = 4;
            } else if (pt == ROOK) {
                dirs = rook_offsets;
                num_dirs = 4;
            } else {
                dirs = king_offsets;
                num_dirs = 8;
            }

            for (int d = 0; d < num_dirs; d++) {
                int dir = dirs[d];
                int to = from + dir;
                while (!(to & 0x88)) {
                    char target = e->board[to];
                    if (target & side)
                        break;  /* Own piece blocks */
                    moves = g_list_append(moves, GINT_TO_POINTER(from * 256 + to));
                    if (target & opp)
                        break;  /* Capture ends ray */
                    to += dir;
                }
            }
        }
    }

    return moves;
}

/**
 * Filter pseudo-legal moves to get only legal moves
 */
static GList *get_legal_moves(ChessEngine *e)
{
    GList *pseudo = generate_moves(e);
    GList *legal = NULL;
    int side = e->side;

    for (GList *l = pseudo; l; l = l->next) {
        int move = GPOINTER_TO_INT(l->data);
        int from = move / 256;
        int to = move % 256;

        /* Try the move */
        char from_piece = e->board[from];
        char to_piece = e->board[to];
        char ep_captured = 0;
        int old_ep = e->ep_square;

        e->board[from] = 0;
        e->board[to] = from_piece | MOVED;

        /* Handle en passant capture */
        if ((from_piece & 7) == PAWN && to == e->ep_square) {
            int dir = (side == WHITE) ? 16 : -16;
            int victim_sq = to - dir;
            ep_captured = e->board[victim_sq];
            e->board[victim_sq] = 0;
        }

        /* Handle castling - move the rook */
        if ((from_piece & 7) == KING && abs(to - from) == 2) {
            if (to > from) {
                /* Kingside */
                e->board[from + 1] = e->board[from + 3] | MOVED;
                e->board[from + 3] = 0;
            } else {
                /* Queenside */
                e->board[from - 1] = e->board[from - 4] | MOVED;
                e->board[from - 4] = 0;
            }
        }

        /* Check if this leaves us in check */
        if (!is_in_check(e, side)) {
            legal = g_list_append(legal, GINT_TO_POINTER(move));
        }

        /* Undo the move */
        e->board[from] = from_piece;
        e->board[to] = to_piece;
        e->ep_square = old_ep;

        /* Undo en passant capture */
        if (ep_captured) {
            int dir = (side == WHITE) ? 16 : -16;
            int victim_sq = to - dir;
            e->board[victim_sq] = ep_captured;
        }

        /* Undo castling */
        if ((from_piece & 7) == KING && abs(to - from) == 2) {
            if (to > from) {
                e->board[from + 3] = (e->board[from + 1] & ~MOVED);
                e->board[from + 1] = 0;
            } else {
                e->board[from - 4] = (e->board[from - 1] & ~MOVED);
                e->board[from - 1] = 0;
            }
        }
    }

    g_list_free(pseudo);
    return legal;
}

/**
 * Simple evaluation function
 */
static int evaluate(ChessEngine *e)
{
    int score = 0;

    for (int sq = 0; sq < 128; sq++) {
        if (sq & 0x88)
            continue;

        char piece = e->board[sq];
        if (!piece)
            continue;

        int pt = piece & 7;
        int value = piece_value[pt];

        /* Piece-square bonus (center control) */
        int file = sq & 7;
        int rank = sq >> 4;
        int center_bonus = (3 - abs(file - 3)) + (3 - abs(rank - 3));

        value += center_bonus * 5;

        if (piece & WHITE)
            score += value;
        else
            score -= value;
    }

    return (e->side == WHITE) ? score : -score;
}

/**
 * Alpha-beta search
 */
static int alpha_beta(ChessEngine *e, int alpha, int beta, int depth)
{
    e->nodes++;

    if (depth <= 0)
        return evaluate(e);

    GList *moves = get_legal_moves(e);

    if (!moves) {
        /* No legal moves - checkmate or stalemate */
        if (is_in_check(e, e->side))
            return -INF + (10 - depth);  /* Checkmate */
        return 0;  /* Stalemate */
    }

    int best_score = -INF;
    int best_from = -1, best_to = -1;

    for (GList *l = moves; l; l = l->next) {
        int move = GPOINTER_TO_INT(l->data);
        int from = move / 256;
        int to = move % 256;

        /* Make move */
        char from_piece = e->board[from];
        char to_piece = e->board[to];
        char ep_captured = 0;
        int old_ep = e->ep_square;
        int old_side = e->side;

        e->board[from] = 0;
        e->board[to] = from_piece | MOVED;

        /* Handle pawn promotion */
        if ((from_piece & 7) == PAWN) {
            int promo_rank = (old_side == WHITE) ? 7 : 0;
            if ((to >> 4) == promo_rank) {
                e->board[to] = QUEEN + old_side + MOVED;
            }
        }

        /* Set en passant square for double pawn push */
        e->ep_square = 0;
        if ((from_piece & 7) == PAWN && abs(to - from) == 32) {
            e->ep_square = (from + to) / 2;
        }

        /* Handle en passant capture */
        if ((from_piece & 7) == PAWN && to == old_ep) {
            int dir = (old_side == WHITE) ? 16 : -16;
            int victim_sq = to - dir;
            ep_captured = e->board[victim_sq];
            e->board[victim_sq] = 0;
        }

        /* Handle castling */
        if ((from_piece & 7) == KING && abs(to - from) == 2) {
            if (to > from) {
                e->board[from + 1] = e->board[from + 3] | MOVED;
                e->board[from + 3] = 0;
            } else {
                e->board[from - 1] = e->board[from - 4] | MOVED;
                e->board[from - 4] = 0;
            }
        }

        e->side = 24 - e->side;

        int score = -alpha_beta(e, -beta, -alpha, depth - 1);

        /* Undo move */
        e->board[from] = from_piece;
        e->board[to] = to_piece;
        e->ep_square = old_ep;
        e->side = old_side;

        if (ep_captured) {
            int dir = (old_side == WHITE) ? 16 : -16;
            int victim_sq = to - dir;
            e->board[victim_sq] = ep_captured;
        }

        if ((from_piece & 7) == KING && abs(to - from) == 2) {
            if (to > from) {
                e->board[from + 3] = (e->board[from + 1] & ~MOVED);
                e->board[from + 1] = 0;
            } else {
                e->board[from - 4] = (e->board[from - 1] & ~MOVED);
                e->board[from - 1] = 0;
            }
        }

        if (score > best_score) {
            best_score = score;
            best_from = from;
            best_to = to;
        }

        if (score > alpha)
            alpha = score;

        if (alpha >= beta)
            break;
    }

    g_list_free(moves);

    e->best_from = best_from;
    e->best_to = best_to;

    return best_score;
}

/* Public API implementation */

ChessEngine *chess_engine_new(void)
{
    ChessEngine *e = g_new0(ChessEngine, 1);
    e->hash_table = g_new0(HashEntry, HASH_SIZE);
    chess_engine_reset(e);
    return e;
}

void chess_engine_free(ChessEngine *engine)
{
    if (!engine)
        return;
    g_free(engine->hash_table);
    g_free(engine);
}

void chess_engine_reset(ChessEngine *e)
{
    if (!e)
        return;

    memset(e->board, 0, sizeof(e->board));
    memset(e->hash_table, 0, sizeof(HashEntry) * HASH_SIZE);

    /* Initial piece types: RNBQKBNR */
    static const int initial_pieces[8] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};

    for (int i = 0; i < 8; i++) {
        /* White back rank */
        e->board[i] = initial_pieces[i] + WHITE;
        /* Black back rank */
        e->board[i + 112] = initial_pieces[i] + BLACK;
        /* White pawns */
        e->board[i + 16] = PAWN + WHITE;
        /* Black pawns */
        e->board[i + 96] = PAWN + BLACK;
    }

    e->side = WHITE;
    e->ep_square = 0;
    e->halfmove = 0;
    e->fullmove = 1;
    e->nodes = 0;
}

gboolean chess_engine_set_fen(ChessEngine *e, const char *fen)
{
    if (!e || !fen)
        return FALSE;

    memset(e->board, 0, sizeof(e->board));

    int rank = 7, file = 0;
    const char *p = fen;

    /* Parse piece placement */
    while (*p && *p != ' ') {
        if (*p == '/') {
            rank--;
            file = 0;
        } else if (*p >= '1' && *p <= '8') {
            file += *p - '0';
        } else {
            int sq = rank * 16 + file;
            int piece = 0;
            int color = isupper(*p) ? WHITE : BLACK;

            switch (tolower(*p)) {
                case 'p': piece = PAWN; break;
                case 'n': piece = KNIGHT; break;
                case 'k': piece = KING; break;
                case 'b': piece = BISHOP; break;
                case 'r': piece = ROOK; break;
                case 'q': piece = QUEEN; break;
                default: return FALSE;
            }

            e->board[sq] = piece + color + MOVED;  /* Assume moved for FEN */
            file++;
        }
        p++;
    }

    if (*p) p++;  /* Skip space */

    /* Side to move */
    if (*p == 'w' || *p == 'W')
        e->side = WHITE;
    else if (*p == 'b' || *p == 'B')
        e->side = BLACK;
    if (*p) p++;

    if (*p) p++;  /* Skip space */

    /* Castling rights - clear MOVED flag for unmoved pieces */
    while (*p && *p != ' ') {
        switch (*p) {
            case 'K':
                e->board[4] &= ~MOVED;
                e->board[7] &= ~MOVED;
                break;
            case 'Q':
                e->board[4] &= ~MOVED;
                e->board[0] &= ~MOVED;
                break;
            case 'k':
                e->board[116] &= ~MOVED;
                e->board[119] &= ~MOVED;
                break;
            case 'q':
                e->board[116] &= ~MOVED;
                e->board[112] &= ~MOVED;
                break;
        }
        p++;
    }

    if (*p) p++;  /* Skip space */

    /* En passant */
    if (*p && *p != '-') {
        e->ep_square = algebraic_to_square(p);
    } else {
        e->ep_square = 0;
    }

    return TRUE;
}

char *chess_engine_get_fen(ChessEngine *e)
{
    if (!e)
        return NULL;

    GString *fen = g_string_new(NULL);

    /* Piece placement */
    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            int sq = rank * 16 + file;
            char piece = e->board[sq];

            if (!piece) {
                empty++;
            } else {
                if (empty > 0) {
                    g_string_append_printf(fen, "%d", empty);
                    empty = 0;
                }

                int pt = piece & 7;
                int color = piece & (WHITE | BLACK);
                char c;

                switch (pt) {
                    case PAWN:   c = 'p'; break;
                    case KNIGHT: c = 'n'; break;
                    case KING:   c = 'k'; break;
                    case BISHOP: c = 'b'; break;
                    case ROOK:   c = 'r'; break;
                    case QUEEN:  c = 'q'; break;
                    default: c = '?'; break;
                }

                if (color == WHITE)
                    c = toupper(c);

                g_string_append_c(fen, c);
            }
        }
        if (empty > 0)
            g_string_append_printf(fen, "%d", empty);
        if (rank > 0)
            g_string_append_c(fen, '/');
    }

    /* Side to move */
    g_string_append(fen, e->side == WHITE ? " w " : " b ");

    /* Castling rights */
    gboolean has_castling = FALSE;
    if (!(e->board[4] & MOVED) && !(e->board[7] & MOVED) &&
        (e->board[4] & 7) == KING && (e->board[7] & 7) == ROOK) {
        g_string_append_c(fen, 'K');
        has_castling = TRUE;
    }
    if (!(e->board[4] & MOVED) && !(e->board[0] & MOVED) &&
        (e->board[4] & 7) == KING && (e->board[0] & 7) == ROOK) {
        g_string_append_c(fen, 'Q');
        has_castling = TRUE;
    }
    if (!(e->board[116] & MOVED) && !(e->board[119] & MOVED) &&
        (e->board[116] & 7) == KING && (e->board[119] & 7) == ROOK) {
        g_string_append_c(fen, 'k');
        has_castling = TRUE;
    }
    if (!(e->board[116] & MOVED) && !(e->board[112] & MOVED) &&
        (e->board[116] & 7) == KING && (e->board[112] & 7) == ROOK) {
        g_string_append_c(fen, 'q');
        has_castling = TRUE;
    }
    if (!has_castling)
        g_string_append_c(fen, '-');

    /* En passant */
    g_string_append_c(fen, ' ');
    if (e->ep_square > 0) {
        char *ep = square_to_algebraic(e->ep_square);
        g_string_append(fen, ep);
        g_free(ep);
    } else {
        g_string_append_c(fen, '-');
    }

    /* Halfmove and fullmove clocks */
    g_string_append_printf(fen, " %d %d", e->halfmove, e->fullmove);

    return g_string_free(fen, FALSE);
}

gboolean chess_engine_is_legal_move(ChessEngine *e, const char *from, const char *to)
{
    if (!e || !from || !to)
        return FALSE;

    int from_sq = algebraic_to_square(from);
    int to_sq = algebraic_to_square(to);

    if (from_sq < 0 || to_sq < 0)
        return FALSE;

    GList *moves = get_legal_moves(e);
    gboolean found = FALSE;

    for (GList *l = moves; l; l = l->next) {
        int move = GPOINTER_TO_INT(l->data);
        if (move / 256 == from_sq && move % 256 == to_sq) {
            found = TRUE;
            break;
        }
    }

    g_list_free(moves);
    return found;
}

GList *chess_engine_get_legal_moves(ChessEngine *e, const char *square)
{
    if (!e || !square)
        return NULL;

    int from_sq = algebraic_to_square(square);
    if (from_sq < 0)
        return NULL;

    char piece = e->board[from_sq];
    if (!(piece & e->side))
        return NULL;

    GList *all_moves = get_legal_moves(e);
    GList *result = NULL;

    for (GList *l = all_moves; l; l = l->next) {
        int move = GPOINTER_TO_INT(l->data);
        if (move / 256 == from_sq) {
            int to_sq = move % 256;
            char *to = square_to_algebraic(to_sq);
            if (to)
                result = g_list_append(result, to);
        }
    }

    g_list_free(all_moves);
    return result;
}

char *chess_engine_get_best_move(ChessEngine *e, int depth)
{
    if (!e || depth < 1)
        return NULL;

    if (depth > 10)
        depth = 10;

    e->nodes = 0;
    e->best_from = -1;
    e->best_to = -1;

    alpha_beta(e, -INF, INF, depth);

    if (e->best_from < 0 || e->best_to < 0)
        return NULL;

    char *from = square_to_algebraic(e->best_from);
    char *to = square_to_algebraic(e->best_to);

    if (!from || !to) {
        g_free(from);
        g_free(to);
        return NULL;
    }

    char *result = g_strdup_printf("%s%s", from, to);
    g_free(from);
    g_free(to);

    return result;
}

gboolean chess_engine_make_move(ChessEngine *e, const char *from, const char *to, char promotion)
{
    if (!e || !from || !to)
        return FALSE;

    int from_sq = algebraic_to_square(from);
    int to_sq = algebraic_to_square(to);

    if (from_sq < 0 || to_sq < 0)
        return FALSE;

    if (!chess_engine_is_legal_move(e, from, to))
        return FALSE;

    char from_piece = e->board[from_sq];
    int pt = from_piece & 7;

    /* Handle en passant capture */
    if (pt == PAWN && to_sq == e->ep_square) {
        int dir = (e->side == WHITE) ? 16 : -16;
        e->board[to_sq - dir] = 0;
    }

    /* Make the move */
    e->board[from_sq] = 0;
    e->board[to_sq] = from_piece | MOVED;

    /* Handle pawn promotion */
    if (pt == PAWN) {
        int promo_rank = (e->side == WHITE) ? 7 : 0;
        if ((to_sq >> 4) == promo_rank) {
            int promo_piece;
            switch (tolower(promotion)) {
                case 'r': promo_piece = ROOK; break;
                case 'b': promo_piece = BISHOP; break;
                case 'n': promo_piece = KNIGHT; break;
                default:  promo_piece = QUEEN; break;
            }
            e->board[to_sq] = promo_piece + e->side + MOVED;
        }

        /* Set en passant square for double push */
        if (abs(to_sq - from_sq) == 32) {
            e->ep_square = (from_sq + to_sq) / 2;
        } else {
            e->ep_square = 0;
        }
    } else {
        e->ep_square = 0;
    }

    /* Handle castling */
    if (pt == KING && abs(to_sq - from_sq) == 2) {
        if (to_sq > from_sq) {
            /* Kingside */
            e->board[from_sq + 1] = e->board[from_sq + 3] | MOVED;
            e->board[from_sq + 3] = 0;
        } else {
            /* Queenside */
            e->board[from_sq - 1] = e->board[from_sq - 4] | MOVED;
            e->board[from_sq - 4] = 0;
        }
    }

    /* Switch sides */
    e->side = 24 - e->side;

    return TRUE;
}

gboolean chess_engine_make_move_san(ChessEngine *e, const char *san)
{
    if (!e || !san)
        return FALSE;

    /* Handle castling */
    if (strcmp(san, "O-O") == 0 || strcmp(san, "0-0") == 0) {
        const char *from = (e->side == WHITE) ? "e1" : "e8";
        const char *to = (e->side == WHITE) ? "g1" : "g8";
        return chess_engine_make_move(e, from, to, 0);
    }
    if (strcmp(san, "O-O-O") == 0 || strcmp(san, "0-0-0") == 0) {
        const char *from = (e->side == WHITE) ? "e1" : "e8";
        const char *to = (e->side == WHITE) ? "c1" : "c8";
        return chess_engine_make_move(e, from, to, 0);
    }

    /* Parse SAN */
    const char *p = san;
    int piece_type = PAWN;
    int from_file = -1, from_rank = -1;
    int to_file = -1, to_rank = -1;
    char promotion = 0;

    /* Check for piece letter */
    if (*p >= 'A' && *p <= 'Z' && *p != 'O') {
        switch (*p) {
            case 'N': piece_type = KNIGHT; break;
            case 'K': piece_type = KING; break;
            case 'B': piece_type = BISHOP; break;
            case 'R': piece_type = ROOK; break;
            case 'Q': piece_type = QUEEN; break;
            default: return FALSE;
        }
        p++;
    }

    /* Parse rest of move */
    while (*p) {
        if (*p >= 'a' && *p <= 'h') {
            if (to_file >= 0)
                from_file = to_file;
            to_file = *p - 'a';
        } else if (*p >= '1' && *p <= '8') {
            if (to_rank >= 0)
                from_rank = to_rank;
            to_rank = *p - '1';
        } else if (*p == 'x') {
            /* Capture - ignore */
        } else if (*p == '=') {
            p++;
            if (*p)
                promotion = *p;
            break;
        } else if (*p == '+' || *p == '#') {
            /* Check/checkmate indicator */
        }
        p++;
    }

    if (to_file < 0 || to_rank < 0)
        return FALSE;

    int to_sq = to_rank * 16 + to_file;

    /* Find the piece that can make this move */
    GList *moves = get_legal_moves(e);

    for (GList *l = moves; l; l = l->next) {
        int move = GPOINTER_TO_INT(l->data);
        int from = move / 256;
        int to = move % 256;

        if (to != to_sq)
            continue;

        char piece = e->board[from];
        if ((piece & 7) != piece_type)
            continue;

        /* Check disambiguation */
        if (from_file >= 0 && (from & 7) != from_file)
            continue;
        if (from_rank >= 0 && (from >> 4) != from_rank)
            continue;

        /* Found the move */
        g_list_free(moves);

        char *from_alg = square_to_algebraic(from);
        char *to_alg = square_to_algebraic(to);
        gboolean result = chess_engine_make_move(e, from_alg, to_alg, promotion);
        g_free(from_alg);
        g_free(to_alg);
        return result;
    }

    g_list_free(moves);
    return FALSE;
}

gboolean chess_engine_is_check(ChessEngine *e)
{
    if (!e)
        return FALSE;
    return is_in_check(e, e->side);
}

gboolean chess_engine_is_checkmate(ChessEngine *e)
{
    if (!e)
        return FALSE;

    if (!is_in_check(e, e->side))
        return FALSE;

    GList *moves = get_legal_moves(e);
    gboolean no_moves = (moves == NULL);
    g_list_free(moves);

    return no_moves;
}

gboolean chess_engine_is_stalemate(ChessEngine *e)
{
    if (!e)
        return FALSE;

    if (is_in_check(e, e->side))
        return FALSE;

    GList *moves = get_legal_moves(e);
    gboolean no_moves = (moves == NULL);
    g_list_free(moves);

    return no_moves;
}

int chess_engine_get_side_to_move(ChessEngine *e)
{
    if (!e)
        return 0;
    return (e->side == WHITE) ? 0 : 1;
}

char chess_engine_get_piece_at(ChessEngine *e, const char *square)
{
    if (!e || !square)
        return '.';

    int sq = algebraic_to_square(square);
    if (sq < 0)
        return '.';

    char piece = e->board[sq];
    if (!piece)
        return '.';

    int pt = piece & 7;
    int color = piece & (WHITE | BLACK);
    char c;

    switch (pt) {
        case PAWN:   c = 'p'; break;
        case KNIGHT: c = 'n'; break;
        case KING:   c = 'k'; break;
        case BISHOP: c = 'b'; break;
        case ROOK:   c = 'r'; break;
        case QUEEN:  c = 'q'; break;
        default: return '.';
    }

    if (color == WHITE)
        c = toupper(c);

    return c;
}

void chess_engine_print_board(ChessEngine *e)
{
    if (!e)
        return;

    printf("\n  a b c d e f g h\n");
    for (int rank = 7; rank >= 0; rank--) {
        printf("%d ", rank + 1);
        for (int file = 0; file < 8; file++) {
            char sq_name[3] = {'a' + file, '1' + rank, '\0'};
            char piece = chess_engine_get_piece_at(e, sq_name);
            printf("%c ", piece);
        }
        printf("%d\n", rank + 1);
    }
    printf("  a b c d e f g h\n");
    printf("\n%s to move\n", e->side == WHITE ? "White" : "Black");
}
