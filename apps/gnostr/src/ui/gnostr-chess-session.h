/*
 * gnostr-chess-session.h - Chess Game Session Management
 *
 * GObject for managing active chess game sessions. Handles player types
 * (human/AI), game state transitions, move history, and async AI move
 * computation using GTask.
 *
 * This session object sits between the UI (GnostrChessBoard) and the
 * engine (ChessEngine), coordinating game flow, player turns, and
 * state changes.
 *
 * Usage:
 *   GnostrChessSession *session = gnostr_chess_session_new();
 *   gnostr_chess_session_set_players(session, GNOSTR_CHESS_PLAYER_HUMAN,
 *                                     GNOSTR_CHESS_PLAYER_AI);
 *   gnostr_chess_session_set_ai_depth(session, 5);
 *   gnostr_chess_session_start(session);
 *
 * Signals:
 * - "state-changed" (GnostrChessState new_state)
 *     Emitted when game state transitions (setup -> playing -> finished)
 *
 * - "move-made" (const gchar *san, gint move_number)
 *     Emitted after each move is made (human or AI)
 *
 * - "game-over" (const gchar *result, const gchar *reason)
 *     Emitted when game ends (checkmate, stalemate, resignation, draw)
 *
 * - "turn-changed" (gboolean is_white_turn)
 *     Emitted when turn changes between players
 *
 * - "ai-thinking" (gboolean is_thinking)
 *     Emitted when AI computation starts/stops (for UI spinners)
 */

#ifndef GNOSTR_CHESS_SESSION_H
#define GNOSTR_CHESS_SESSION_H

#include <glib-object.h>
#include <gio/gio.h>
#include "../util/chess_engine.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHESS_SESSION (gnostr_chess_session_get_type())

G_DECLARE_FINAL_TYPE(GnostrChessSession, gnostr_chess_session, GNOSTR, CHESS_SESSION, GObject)

/**
 * GnostrChessState:
 * @GNOSTR_CHESS_STATE_SETUP: Game is being configured (players, settings)
 * @GNOSTR_CHESS_STATE_PLAYING: Game is in progress
 * @GNOSTR_CHESS_STATE_FINISHED: Game has ended
 *
 * States for the chess game session lifecycle.
 */
typedef enum {
    GNOSTR_CHESS_STATE_SETUP,
    GNOSTR_CHESS_STATE_PLAYING,
    GNOSTR_CHESS_STATE_FINISHED
} GnostrChessState;

/**
 * GnostrChessPlayerType:
 * @GNOSTR_CHESS_PLAYER_HUMAN: Human player (requires UI input)
 * @GNOSTR_CHESS_PLAYER_AI: AI player (computed by chess engine)
 *
 * Types of chess players.
 */
typedef enum {
    GNOSTR_CHESS_PLAYER_HUMAN,
    GNOSTR_CHESS_PLAYER_AI
} GnostrChessPlayerType;

typedef struct _GnostrChessSession GnostrChessSession;

/* ============================================================================
 * Object Creation
 * ============================================================================ */

/**
 * gnostr_chess_session_new:
 *
 * Creates a new chess game session in SETUP state.
 * Use set_players() and start() to begin the game.
 *
 * Returns: (transfer full): A new GnostrChessSession.
 */
GnostrChessSession *gnostr_chess_session_new(void);

/* ============================================================================
 * Setup Methods (valid in SETUP state)
 * ============================================================================ */

/**
 * gnostr_chess_session_set_players:
 * @self: The chess session.
 * @white_type: Player type for white (human or AI).
 * @black_type: Player type for black (human or AI).
 *
 * Configures the player types for the game.
 * Must be called before gnostr_chess_session_start().
 */
void gnostr_chess_session_set_players(GnostrChessSession *self,
                                       GnostrChessPlayerType white_type,
                                       GnostrChessPlayerType black_type);

/**
 * gnostr_chess_session_set_ai_depth:
 * @self: The chess session.
 * @depth: Search depth for AI (2-10, default 4).
 *
 * Sets the AI search depth. Higher values are stronger but slower.
 * Clamped to valid range [2, 10].
 */
void gnostr_chess_session_set_ai_depth(GnostrChessSession *self, gint depth);

/**
 * gnostr_chess_session_start:
 * @self: The chess session.
 *
 * Starts the game. Transitions state from SETUP to PLAYING.
 * If white is AI, automatically triggers first AI move.
 */
void gnostr_chess_session_start(GnostrChessSession *self);

/* ============================================================================
 * Gameplay Methods (valid in PLAYING state)
 * ============================================================================ */

/**
 * gnostr_chess_session_make_move:
 * @self: The chess session.
 * @from: Source square in algebraic notation (e.g., "e2").
 * @to: Destination square in algebraic notation (e.g., "e4").
 * @promotion: Promotion piece ('q', 'r', 'b', 'n') or 0 for none.
 *
 * Makes a human player move. Only valid when it's a human player's turn.
 * Emits "move-made" and "turn-changed" signals on success.
 * Automatically triggers AI move if opponent is AI.
 *
 * Returns: TRUE if move was legal and executed.
 */
gboolean gnostr_chess_session_make_move(GnostrChessSession *self,
                                         const gchar *from,
                                         const gchar *to,
                                         gchar promotion);

/**
 * gnostr_chess_session_request_ai_move:
 * @self: The chess session.
 *
 * Requests an AI move computation. This runs asynchronously using GTask.
 * Emits "ai-thinking" with TRUE when starting, FALSE when done.
 * Emits "move-made" when the AI move is complete.
 *
 * Only valid when it's an AI player's turn.
 */
void gnostr_chess_session_request_ai_move(GnostrChessSession *self);

/**
 * gnostr_chess_session_is_human_turn:
 * @self: The chess session.
 *
 * Checks if it's currently a human player's turn.
 *
 * Returns: TRUE if a human should make the next move.
 */
gboolean gnostr_chess_session_is_human_turn(GnostrChessSession *self);

/* ============================================================================
 * State Queries
 * ============================================================================ */

/**
 * gnostr_chess_session_get_engine:
 * @self: The chess session.
 *
 * Gets the underlying chess engine for direct queries.
 * The engine is owned by the session and should not be freed.
 *
 * Returns: (transfer none): The ChessEngine, or NULL if not started.
 */
ChessEngine *gnostr_chess_session_get_engine(GnostrChessSession *self);

/**
 * gnostr_chess_session_get_move_history:
 * @self: The chess session.
 *
 * Gets the list of moves made in SAN notation.
 *
 * Returns: (transfer full) (element-type utf8): List of SAN strings.
 *          Free with g_list_free_full(list, g_free).
 */
GList *gnostr_chess_session_get_move_history(GnostrChessSession *self);

/**
 * gnostr_chess_session_get_move_count:
 * @self: The chess session.
 *
 * Gets the number of half-moves (plies) made.
 *
 * Returns: Number of moves made.
 */
gint gnostr_chess_session_get_move_count(GnostrChessSession *self);

/**
 * gnostr_chess_session_export_pgn:
 * @self: The chess session.
 *
 * Exports the current game as a PGN string.
 *
 * Returns: (transfer full): PGN text, or NULL if no game.
 *          Caller must free with g_free().
 */
gchar *gnostr_chess_session_export_pgn(GnostrChessSession *self);

/**
 * gnostr_chess_session_get_state:
 * @self: The chess session.
 *
 * Gets the current game state.
 *
 * Returns: Current GnostrChessState.
 */
GnostrChessState gnostr_chess_session_get_state(GnostrChessSession *self);

/**
 * gnostr_chess_session_get_result:
 * @self: The chess session.
 *
 * Gets the game result if finished.
 *
 * Returns: (transfer none) (nullable): Result string ("1-0", "0-1", "1/2-1/2")
 *          or NULL if game not finished.
 */
const gchar *gnostr_chess_session_get_result(GnostrChessSession *self);

/**
 * gnostr_chess_session_is_white_turn:
 * @self: The chess session.
 *
 * Checks if it's white's turn to move.
 *
 * Returns: TRUE if white to move, FALSE if black.
 */
gboolean gnostr_chess_session_is_white_turn(GnostrChessSession *self);

/* ============================================================================
 * Game Actions
 * ============================================================================ */

/**
 * gnostr_chess_session_resign:
 * @self: The chess session.
 *
 * Resigns the game for the current player.
 * Ends the game with opponent winning.
 * Emits "game-over" signal.
 */
void gnostr_chess_session_resign(GnostrChessSession *self);

/**
 * gnostr_chess_session_offer_draw:
 * @self: The chess session.
 *
 * Offers/accepts a draw. In human vs human games, this requires
 * both players to call this. In human vs AI, AI auto-accepts.
 * Emits "game-over" signal if draw is accepted.
 */
void gnostr_chess_session_offer_draw(GnostrChessSession *self);

/**
 * gnostr_chess_session_reset:
 * @self: The chess session.
 *
 * Resets the session to SETUP state with default settings.
 * Clears move history and game result.
 */
void gnostr_chess_session_reset(GnostrChessSession *self);

/* ============================================================================
 * Properties
 * ============================================================================ */

/**
 * gnostr_chess_session_get_white_player:
 * @self: The chess session.
 *
 * Returns: Player type for white.
 */
GnostrChessPlayerType gnostr_chess_session_get_white_player(GnostrChessSession *self);

/**
 * gnostr_chess_session_get_black_player:
 * @self: The chess session.
 *
 * Returns: Player type for black.
 */
GnostrChessPlayerType gnostr_chess_session_get_black_player(GnostrChessSession *self);

/**
 * gnostr_chess_session_get_ai_depth:
 * @self: The chess session.
 *
 * Returns: Current AI search depth.
 */
gint gnostr_chess_session_get_ai_depth(GnostrChessSession *self);

G_END_DECLS

#endif /* GNOSTR_CHESS_SESSION_H */
