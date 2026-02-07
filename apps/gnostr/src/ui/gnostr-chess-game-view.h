/*
 * gnostr-chess-game-view.h - Complete Chess Game View
 *
 * Container widget that combines the chess board with game controls for
 * playing chess against an AI opponent. Coordinates between the interactive
 * board widget and the session manager for a complete game experience.
 *
 * Features:
 * - Interactive chess board with click-to-move
 * - AI opponent with configurable difficulty
 * - Status display (whose turn, thinking indicator)
 * - Move history list
 * - Game control buttons (new game, resign, flip board)
 *
 * Signals:
 * - "game-started" ()
 *     Emitted when a new game begins
 *
 * - "game-ended" (const gchar *result, const gchar *reason)
 *     Emitted when game ends (checkmate, stalemate, resignation)
 *
 * - "move-played" (const gchar *san, gint move_number)
 *     Emitted after each move is played
 */

#ifndef GNOSTR_CHESS_GAME_VIEW_H
#define GNOSTR_CHESS_GAME_VIEW_H

#include <gtk/gtk.h>
#include "gnostr-chess-board.h"
#include "gnostr-chess-session.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHESS_GAME_VIEW (gnostr_chess_game_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrChessGameView, gnostr_chess_game_view, GNOSTR, CHESS_GAME_VIEW, GtkWidget)

typedef struct _GnostrChessGameView GnostrChessGameView;

/**
 * GnostrChessDifficulty:
 * @GNOSTR_CHESS_DIFFICULTY_BEGINNER: Depth 2 (~800 ELO)
 * @GNOSTR_CHESS_DIFFICULTY_INTERMEDIATE: Depth 4 (~1200 ELO)
 * @GNOSTR_CHESS_DIFFICULTY_ADVANCED: Depth 6 (~1600 ELO)
 * @GNOSTR_CHESS_DIFFICULTY_EXPERT: Depth 8 (~1800 ELO)
 *
 * AI difficulty levels.
 */
typedef enum {
    GNOSTR_CHESS_DIFFICULTY_BEGINNER = 2,
    GNOSTR_CHESS_DIFFICULTY_INTERMEDIATE = 4,
    GNOSTR_CHESS_DIFFICULTY_ADVANCED = 6,
    GNOSTR_CHESS_DIFFICULTY_EXPERT = 8
} GnostrChessDifficulty;

/* ============================================================================
 * Widget Creation
 * ============================================================================ */

/**
 * gnostr_chess_game_view_new:
 *
 * Creates a new chess game view widget.
 *
 * Returns: (transfer full): A new GnostrChessGameView widget.
 */
GnostrChessGameView *gnostr_chess_game_view_new(void);

/* ============================================================================
 * Game Setup
 * ============================================================================ */

/**
 * gnostr_chess_game_view_new_game:
 * @self: The game view widget.
 * @play_as_white: TRUE if human plays white, FALSE for black.
 * @difficulty: AI difficulty level.
 *
 * Starts a new game against the AI.
 */
void gnostr_chess_game_view_new_game(GnostrChessGameView *self,
                                      gboolean play_as_white,
                                      GnostrChessDifficulty difficulty);

/**
 * gnostr_chess_game_view_new_game_human_vs_human:
 * @self: The game view widget.
 *
 * Starts a new game for two human players.
 */
void gnostr_chess_game_view_new_game_human_vs_human(GnostrChessGameView *self);

/**
 * gnostr_chess_game_view_new_game_ai_vs_ai:
 * @self: The game view widget.
 * @difficulty: AI difficulty level for both players.
 *
 * Starts a new AI vs AI game (for demonstration).
 */
void gnostr_chess_game_view_new_game_ai_vs_ai(GnostrChessGameView *self,
                                               GnostrChessDifficulty difficulty);

/* ============================================================================
 * Game Actions
 * ============================================================================ */

/**
 * gnostr_chess_game_view_resign:
 * @self: The game view widget.
 *
 * Resigns the current game (human player loses).
 */
void gnostr_chess_game_view_resign(GnostrChessGameView *self);

/**
 * gnostr_chess_game_view_offer_draw:
 * @self: The game view widget.
 *
 * Offers a draw.
 */
void gnostr_chess_game_view_offer_draw(GnostrChessGameView *self);

/**
 * gnostr_chess_game_view_flip_board:
 * @self: The game view widget.
 *
 * Toggles board orientation.
 */
void gnostr_chess_game_view_flip_board(GnostrChessGameView *self);

/* ============================================================================
 * State Queries
 * ============================================================================ */

/**
 * gnostr_chess_game_view_is_game_active:
 * @self: The game view widget.
 *
 * Returns: TRUE if a game is currently in progress.
 */
gboolean gnostr_chess_game_view_is_game_active(GnostrChessGameView *self);

/**
 * gnostr_chess_game_view_is_thinking:
 * @self: The game view widget.
 *
 * Returns: TRUE if AI is currently computing a move.
 */
gboolean gnostr_chess_game_view_is_thinking(GnostrChessGameView *self);

/**
 * gnostr_chess_game_view_get_session:
 * @self: The game view widget.
 *
 * Gets the underlying session object.
 *
 * Returns: (transfer none): The GnostrChessSession.
 */
GnostrChessSession *gnostr_chess_game_view_get_session(GnostrChessGameView *self);

/**
 * gnostr_chess_game_view_get_board:
 * @self: The game view widget.
 *
 * Gets the underlying board widget.
 *
 * Returns: (transfer none): The GnostrChessBoard widget.
 */
GnostrChessBoard *gnostr_chess_game_view_get_board(GnostrChessGameView *self);

/**
 * gnostr_chess_game_view_export_pgn:
 * @self: The game view widget.
 *
 * Exports the current game as PGN.
 *
 * Returns: (transfer full) (nullable): PGN string, or NULL.
 *          Caller must free with g_free().
 */
gchar *gnostr_chess_game_view_export_pgn(GnostrChessGameView *self);

/* ============================================================================
 * Appearance
 * ============================================================================ */

/**
 * gnostr_chess_game_view_set_board_size:
 * @self: The game view widget.
 * @size: Board size in pixels.
 *
 * Sets the chess board display size.
 */
void gnostr_chess_game_view_set_board_size(GnostrChessGameView *self, gint size);

/**
 * gnostr_chess_game_view_set_show_move_list:
 * @self: The game view widget.
 * @show: Whether to show the move list panel.
 *
 * Shows or hides the move history panel.
 */
void gnostr_chess_game_view_set_show_move_list(GnostrChessGameView *self,
                                                 gboolean show);

G_END_DECLS

#endif /* GNOSTR_CHESS_GAME_VIEW_H */
