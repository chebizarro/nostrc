/*
 * gnostr-chess-board.h - Interactive Chess Board Widget
 *
 * GTK4 widget for interactive chess play. Extends the display functionality
 * of GnostrChessCard with click-to-move interaction, legal move highlighting,
 * and integration with a chess engine for move validation.
 *
 * DESIGN STATUS: Scaffold only - awaiting chess engine port
 * See DESIGN-gnostr-chess-board.md for full design document.
 *
 * Features (planned):
 * - Click-to-move piece interaction
 * - Legal move highlighting (dots for moves, rings for captures)
 * - Selected piece highlighting
 * - Pawn promotion dialog
 * - Drag-and-drop support (future)
 * - Move animation (future)
 *
 * Signals:
 * - "piece-selected" (gint file, gint rank, gint piece_type)
 *     Emitted when user selects a piece
 *
 * - "piece-deselected" ()
 *     Emitted when selection is cleared
 *
 * - "move-made" (const gchar *san, const gchar *uci)
 *     Emitted after a legal move is executed
 *     san: Standard Algebraic Notation (e.g., "Nf3", "O-O")
 *     uci: Universal Chess Interface format (e.g., "g1f3", "e1g1")
 *
 * - "move-invalid" (const gchar *attempted)
 *     Emitted when user attempts an illegal move
 *
 * - "promotion-required" (gint from_square, gint to_square)
 *     Emitted when pawn reaches promotion rank
 *
 * - "game-over" (gint result)
 *     Emitted when checkmate, stalemate, or draw is detected
 *     result: GnostrChessResult enum value
 */

#ifndef GNOSTR_CHESS_BOARD_H
#define GNOSTR_CHESS_BOARD_H

#include <gtk/gtk.h>
#include "../util/nip64_chess.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHESS_BOARD (gnostr_chess_board_get_type())

G_DECLARE_FINAL_TYPE(GnostrChessBoard, gnostr_chess_board, GNOSTR, CHESS_BOARD, GtkWidget)

typedef struct _GnostrChessBoard GnostrChessBoard;

/* ============================================================================
 * Widget Creation
 * ============================================================================ */

/**
 * gnostr_chess_board_new:
 *
 * Creates a new interactive chess board widget.
 * The board starts empty; use set_game() or set_fen() to set a position.
 *
 * Returns: (transfer full): A new GnostrChessBoard widget.
 */
GnostrChessBoard *gnostr_chess_board_new(void);

/* ============================================================================
 * Position Setup
 * ============================================================================ */

/**
 * gnostr_chess_board_set_game:
 * @self: The chess board widget.
 * @game: (transfer none): A GnostrChessGame to display.
 *
 * Sets the game to display. The board will show the current position
 * from the game and allow navigation through moves.
 *
 * For interactive play, the game should be at the latest position.
 */
void gnostr_chess_board_set_game(GnostrChessBoard *self, GnostrChessGame *game);

/**
 * gnostr_chess_board_set_fen:
 * @self: The chess board widget.
 * @fen: FEN string describing the position.
 *
 * Sets the board position from a FEN string.
 * This creates a new internal game state.
 *
 * Returns: TRUE if FEN was valid and position was set.
 */
gboolean gnostr_chess_board_set_fen(GnostrChessBoard *self, const gchar *fen);

/**
 * gnostr_chess_board_reset:
 * @self: The chess board widget.
 *
 * Resets to the standard starting position.
 */
void gnostr_chess_board_reset(GnostrChessBoard *self);

/**
 * gnostr_chess_board_get_fen:
 * @self: The chess board widget.
 *
 * Gets the current position as a FEN string.
 *
 * Returns: (transfer full) (nullable): FEN string, or NULL if no position.
 *          Caller must free with g_free().
 */
gchar *gnostr_chess_board_get_fen(GnostrChessBoard *self);

/* ============================================================================
 * Interactive Mode
 * ============================================================================ */

/**
 * gnostr_chess_board_set_interactive:
 * @self: The chess board widget.
 * @interactive: Whether to enable interactive play.
 *
 * When interactive, the board accepts click input for making moves.
 * When not interactive, the board is display-only (replay mode).
 *
 * Default: FALSE
 */
void gnostr_chess_board_set_interactive(GnostrChessBoard *self, gboolean interactive);

/**
 * gnostr_chess_board_is_interactive:
 * @self: The chess board widget.
 *
 * Returns: TRUE if interactive mode is enabled.
 */
gboolean gnostr_chess_board_is_interactive(GnostrChessBoard *self);

/**
 * gnostr_chess_board_set_player_color:
 * @self: The chess board widget.
 * @color: The color the local player controls (WHITE, BLACK, or NONE for both).
 *
 * Sets which pieces the user can move. Use GNOSTR_CHESS_COLOR_NONE to allow
 * moving either side (analysis mode).
 *
 * Default: GNOSTR_CHESS_COLOR_NONE (can move both)
 */
void gnostr_chess_board_set_player_color(GnostrChessBoard *self, GnostrChessColor color);

/**
 * gnostr_chess_board_get_player_color:
 * @self: The chess board widget.
 *
 * Returns: The color the local player controls.
 */
GnostrChessColor gnostr_chess_board_get_player_color(GnostrChessBoard *self);

/* ============================================================================
 * Move Execution
 * ============================================================================ */

/**
 * gnostr_chess_board_make_move:
 * @self: The chess board widget.
 * @from_file: Source file (0-7 for a-h).
 * @from_rank: Source rank (0-7 for 1-8).
 * @to_file: Destination file.
 * @to_rank: Destination rank.
 * @promotion: Promotion piece ('q', 'r', 'b', 'n') or '\0' for none.
 *
 * Attempts to make a move on the board. If the move is legal, it will
 * be executed and "move-made" signal emitted. If illegal, "move-invalid"
 * is emitted.
 *
 * Note: Requires chess engine to be initialized.
 *
 * Returns: TRUE if move was legal and executed.
 */
gboolean gnostr_chess_board_make_move(GnostrChessBoard *self,
                                       gint from_file, gint from_rank,
                                       gint to_file, gint to_rank,
                                       gchar promotion);

/**
 * gnostr_chess_board_make_move_san:
 * @self: The chess board widget.
 * @san: Move in Standard Algebraic Notation (e.g., "Nf3", "e4", "O-O").
 *
 * Attempts to make a move using SAN notation.
 *
 * Returns: TRUE if move was legal and executed.
 */
gboolean gnostr_chess_board_make_move_san(GnostrChessBoard *self, const gchar *san);

/**
 * gnostr_chess_board_make_move_uci:
 * @self: The chess board widget.
 * @uci: Move in UCI format (e.g., "g1f3", "e2e4", "e1g1" for O-O).
 *
 * Attempts to make a move using UCI notation.
 *
 * Returns: TRUE if move was legal and executed.
 */
gboolean gnostr_chess_board_make_move_uci(GnostrChessBoard *self, const gchar *uci);

/**
 * gnostr_chess_board_undo_move:
 * @self: The chess board widget.
 *
 * Undoes the last move if possible.
 *
 * Returns: TRUE if a move was undone.
 */
gboolean gnostr_chess_board_undo_move(GnostrChessBoard *self);

/* ============================================================================
 * Selection State
 * ============================================================================ */

/**
 * gnostr_chess_board_get_selected_square:
 * @self: The chess board widget.
 * @out_file: (out) (optional): Output for selected file.
 * @out_rank: (out) (optional): Output for selected rank.
 *
 * Gets the currently selected square, if any.
 *
 * Returns: TRUE if a square is selected.
 */
gboolean gnostr_chess_board_get_selected_square(GnostrChessBoard *self,
                                                  gint *out_file,
                                                  gint *out_rank);

/**
 * gnostr_chess_board_clear_selection:
 * @self: The chess board widget.
 *
 * Clears any piece selection.
 */
void gnostr_chess_board_clear_selection(GnostrChessBoard *self);

/* ============================================================================
 * Game State Queries
 * ============================================================================ */

/**
 * gnostr_chess_board_get_side_to_move:
 * @self: The chess board widget.
 *
 * Returns: Which color is to move (WHITE or BLACK).
 */
GnostrChessColor gnostr_chess_board_get_side_to_move(GnostrChessBoard *self);

/**
 * gnostr_chess_board_is_check:
 * @self: The chess board widget.
 *
 * Returns: TRUE if the side to move is in check.
 */
gboolean gnostr_chess_board_is_check(GnostrChessBoard *self);

/**
 * gnostr_chess_board_is_checkmate:
 * @self: The chess board widget.
 *
 * Returns: TRUE if the position is checkmate.
 */
gboolean gnostr_chess_board_is_checkmate(GnostrChessBoard *self);

/**
 * gnostr_chess_board_is_stalemate:
 * @self: The chess board widget.
 *
 * Returns: TRUE if the position is stalemate.
 */
gboolean gnostr_chess_board_is_stalemate(GnostrChessBoard *self);

/**
 * gnostr_chess_board_is_game_over:
 * @self: The chess board widget.
 *
 * Returns: TRUE if the game has ended (checkmate, stalemate, or draw).
 */
gboolean gnostr_chess_board_is_game_over(GnostrChessBoard *self);

/**
 * gnostr_chess_board_get_result:
 * @self: The chess board widget.
 *
 * Returns: Game result if game is over, UNKNOWN otherwise.
 */
GnostrChessResult gnostr_chess_board_get_result(GnostrChessBoard *self);

/* ============================================================================
 * Appearance
 * ============================================================================ */

/**
 * gnostr_chess_board_set_size:
 * @self: The chess board widget.
 * @size: Board size in pixels (square).
 *
 * Sets the board display size. Will be clamped to valid range.
 */
void gnostr_chess_board_set_size(GnostrChessBoard *self, gint size);

/**
 * gnostr_chess_board_get_size:
 * @self: The chess board widget.
 *
 * Returns: Current board size in pixels.
 */
gint gnostr_chess_board_get_size(GnostrChessBoard *self);

/**
 * gnostr_chess_board_set_flipped:
 * @self: The chess board widget.
 * @flipped: TRUE to show from Black's perspective.
 *
 * Sets board orientation. When flipped, rank 8 is at bottom.
 */
void gnostr_chess_board_set_flipped(GnostrChessBoard *self, gboolean flipped);

/**
 * gnostr_chess_board_is_flipped:
 * @self: The chess board widget.
 *
 * Returns: TRUE if board is shown from Black's perspective.
 */
gboolean gnostr_chess_board_is_flipped(GnostrChessBoard *self);

/**
 * gnostr_chess_board_set_show_coordinates:
 * @self: The chess board widget.
 * @show: Whether to show file/rank labels.
 *
 * Default: TRUE
 */
void gnostr_chess_board_set_show_coordinates(GnostrChessBoard *self, gboolean show);

/**
 * gnostr_chess_board_get_show_coordinates:
 * @self: The chess board widget.
 *
 * Returns: Whether file/rank labels are shown.
 */
gboolean gnostr_chess_board_get_show_coordinates(GnostrChessBoard *self);

/**
 * gnostr_chess_board_set_show_legal_moves:
 * @self: The chess board widget.
 * @show: Whether to highlight legal moves when a piece is selected.
 *
 * Default: TRUE
 */
void gnostr_chess_board_set_show_legal_moves(GnostrChessBoard *self, gboolean show);

/**
 * gnostr_chess_board_get_show_legal_moves:
 * @self: The chess board widget.
 *
 * Returns: Whether legal move highlighting is enabled.
 */
gboolean gnostr_chess_board_get_show_legal_moves(GnostrChessBoard *self);

/**
 * gnostr_chess_board_set_animate_moves:
 * @self: The chess board widget.
 * @animate: Whether to animate piece movement.
 *
 * Default: TRUE
 */
void gnostr_chess_board_set_animate_moves(GnostrChessBoard *self, gboolean animate);

/**
 * gnostr_chess_board_get_animate_moves:
 * @self: The chess board widget.
 *
 * Returns: Whether move animation is enabled.
 */
gboolean gnostr_chess_board_get_animate_moves(GnostrChessBoard *self);

/* ============================================================================
 * Board Colors (CSS variables preferred, but API available)
 * ============================================================================ */

/**
 * gnostr_chess_board_set_square_colors:
 * @self: The chess board widget.
 * @light: Light square color (hex, e.g., "#f0d9b5").
 * @dark: Dark square color (hex, e.g., "#b58863").
 *
 * Sets custom board colors. Pass NULL to use defaults.
 */
void gnostr_chess_board_set_square_colors(GnostrChessBoard *self,
                                           const gchar *light,
                                           const gchar *dark);

/* ============================================================================
 * Navigation (for replay mode)
 * ============================================================================ */

/**
 * gnostr_chess_board_go_to_ply:
 * @self: The chess board widget.
 * @ply: Ply number (0 = starting position).
 *
 * Navigates to a specific position in the game history.
 * Only works when a game is set.
 *
 * Returns: TRUE if navigation succeeded.
 */
gboolean gnostr_chess_board_go_to_ply(GnostrChessBoard *self, gint ply);

/**
 * gnostr_chess_board_get_current_ply:
 * @self: The chess board widget.
 *
 * Returns: Current ply number.
 */
gint gnostr_chess_board_get_current_ply(GnostrChessBoard *self);

/**
 * gnostr_chess_board_get_total_plies:
 * @self: The chess board widget.
 *
 * Returns: Total number of plies (half-moves) in the game.
 */
gint gnostr_chess_board_get_total_plies(GnostrChessBoard *self);

G_END_DECLS

#endif /* GNOSTR_CHESS_BOARD_H */
