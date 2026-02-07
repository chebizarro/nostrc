/**
 * GNostr Chess Engine
 *
 * A chess engine based on Micro-Max by H.G. Muller.
 * Provides a clean API for:
 * - Position setup (FEN notation)
 * - Best move calculation
 * - Legal move validation
 * - Move application
 * - Game state queries (check/checkmate/stalemate)
 *
 * This implementation wraps the Micro-Max engine internals in a
 * self-contained struct for thread-safe operation.
 */

#ifndef CHESS_ENGINE_H
#define CHESS_ENGINE_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * ChessEngine:
 *
 * Opaque structure containing chess engine state.
 * All engine state is contained within this struct,
 * making it safe to use multiple engines concurrently.
 */
typedef struct ChessEngine ChessEngine;

/**
 * chess_engine_new:
 *
 * Create a new chess engine with the standard starting position.
 *
 * Returns: (transfer full): A new #ChessEngine instance.
 */
ChessEngine *chess_engine_new(void);

/**
 * chess_engine_free:
 * @engine: A #ChessEngine
 *
 * Free the chess engine and all associated resources.
 */
void chess_engine_free(ChessEngine *engine);

/* Position management */

/**
 * chess_engine_reset:
 * @e: A #ChessEngine
 *
 * Reset the engine to the standard starting position.
 */
void chess_engine_reset(ChessEngine *e);

/**
 * chess_engine_set_fen:
 * @e: A #ChessEngine
 * @fen: A FEN string describing the position
 *
 * Set the position from a FEN string.
 *
 * Returns: %TRUE if the FEN was valid and the position was set.
 */
gboolean chess_engine_set_fen(ChessEngine *e, const char *fen);

/**
 * chess_engine_get_fen:
 * @e: A #ChessEngine
 *
 * Get the current position as a FEN string.
 *
 * Returns: (transfer full): The FEN string. Free with g_free().
 */
char *chess_engine_get_fen(ChessEngine *e);

/* Move generation and validation */

/**
 * chess_engine_is_legal_move:
 * @e: A #ChessEngine
 * @from: Source square in algebraic notation (e.g., "e2")
 * @to: Destination square in algebraic notation (e.g., "e4")
 *
 * Check if a move is legal in the current position.
 *
 * Returns: %TRUE if the move is legal.
 */
gboolean chess_engine_is_legal_move(ChessEngine *e, const char *from, const char *to);

/**
 * chess_engine_get_legal_moves:
 * @e: A #ChessEngine
 * @square: A square in algebraic notation (e.g., "e2")
 *
 * Get all legal moves from the specified square.
 *
 * Returns: (transfer full) (element-type utf8): A list of destination
 *          squares in algebraic notation. Free with g_list_free_full()
 *          using g_free as the free function.
 */
GList *chess_engine_get_legal_moves(ChessEngine *e, const char *square);

/**
 * chess_engine_get_best_move:
 * @e: A #ChessEngine
 * @depth: Search depth (1-10 recommended)
 *
 * Calculate the best move in the current position.
 *
 * Returns: (transfer full) (nullable): The best move in coordinate notation
 *          (e.g., "e2e4") or %NULL if no legal moves exist. Free with g_free().
 */
char *chess_engine_get_best_move(ChessEngine *e, int depth);

/* Move execution */

/**
 * chess_engine_make_move:
 * @e: A #ChessEngine
 * @from: Source square in algebraic notation (e.g., "e2")
 * @to: Destination square in algebraic notation (e.g., "e4")
 * @promotion: Promotion piece ('q', 'r', 'b', 'n') or 0 for none
 *
 * Make a move on the board.
 *
 * Returns: %TRUE if the move was legal and executed.
 */
gboolean chess_engine_make_move(ChessEngine *e, const char *from, const char *to, char promotion);

/**
 * chess_engine_make_move_san:
 * @e: A #ChessEngine
 * @san: Move in Standard Algebraic Notation (e.g., "e4", "Nf3", "O-O")
 *
 * Make a move using Standard Algebraic Notation.
 *
 * Returns: %TRUE if the move was parsed, legal, and executed.
 */
gboolean chess_engine_make_move_san(ChessEngine *e, const char *san);

/* Game state */

/**
 * chess_engine_is_check:
 * @e: A #ChessEngine
 *
 * Check if the current side to move is in check.
 *
 * Returns: %TRUE if the side to move is in check.
 */
gboolean chess_engine_is_check(ChessEngine *e);

/**
 * chess_engine_is_checkmate:
 * @e: A #ChessEngine
 *
 * Check if the current position is checkmate.
 *
 * Returns: %TRUE if it is checkmate.
 */
gboolean chess_engine_is_checkmate(ChessEngine *e);

/**
 * chess_engine_is_stalemate:
 * @e: A #ChessEngine
 *
 * Check if the current position is stalemate.
 *
 * Returns: %TRUE if it is stalemate.
 */
gboolean chess_engine_is_stalemate(ChessEngine *e);

/**
 * chess_engine_get_side_to_move:
 * @e: A #ChessEngine
 *
 * Get the side to move.
 *
 * Returns: 0 for white, 1 for black.
 */
int chess_engine_get_side_to_move(ChessEngine *e);

/**
 * chess_engine_get_piece_at:
 * @e: A #ChessEngine
 * @square: A square in algebraic notation (e.g., "e4")
 *
 * Get the piece at the specified square.
 *
 * Returns: Piece character ('P', 'N', 'B', 'R', 'Q', 'K' for white,
 *          'p', 'n', 'b', 'r', 'q', 'k' for black) or '.' for empty.
 */
char chess_engine_get_piece_at(ChessEngine *e, const char *square);

/**
 * chess_engine_print_board:
 * @e: A #ChessEngine
 *
 * Print the current board position to stdout (for debugging).
 */
void chess_engine_print_board(ChessEngine *e);

G_END_DECLS

#endif /* CHESS_ENGINE_H */
