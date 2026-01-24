/*
 * nip64_chess.h - NIP-64 Chess (PGN) Event Utilities
 *
 * NIP-64 defines kind 64 events for sharing chess games in PGN format.
 * This module provides utilities for parsing PGN content, managing game state,
 * and navigating through game moves.
 *
 * Event structure:
 * - kind: 64
 * - content: Complete PGN text of the chess game
 * - tags: Optional metadata (e.g., ["t", "chess"], ["subject", "..."])
 *
 * PGN Format:
 * - Header tags: [Event "..."], [Site "..."], [Date "..."], etc.
 * - Move text: 1. e4 e5 2. Nf3 Nc6 ...
 * - Result: 1-0, 0-1, 1/2-1/2, or *
 */

#ifndef NIP64_CHESS_H
#define NIP64_CHESS_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* Kind number for chess events */
#define NOSTR_KIND_CHESS 64

/* Board dimensions */
#define CHESS_BOARD_SIZE 8
#define CHESS_SQUARES 64

/**
 * GnostrChessPiece:
 * Enumeration of chess pieces.
 */
typedef enum {
  GNOSTR_CHESS_PIECE_NONE = 0,
  GNOSTR_CHESS_PIECE_PAWN,
  GNOSTR_CHESS_PIECE_KNIGHT,
  GNOSTR_CHESS_PIECE_BISHOP,
  GNOSTR_CHESS_PIECE_ROOK,
  GNOSTR_CHESS_PIECE_QUEEN,
  GNOSTR_CHESS_PIECE_KING
} GnostrChessPiece;

/**
 * GnostrChessColor:
 * Enumeration of piece colors.
 */
typedef enum {
  GNOSTR_CHESS_COLOR_NONE = 0,
  GNOSTR_CHESS_COLOR_WHITE,
  GNOSTR_CHESS_COLOR_BLACK
} GnostrChessColor;

/**
 * GnostrChessSquare:
 * Structure representing a square's contents.
 */
typedef struct {
  GnostrChessPiece piece;
  GnostrChessColor color;
} GnostrChessSquare;

/**
 * GnostrChessMove:
 * Structure representing a single move in the game.
 */
typedef struct {
  gchar *san;              /* Standard Algebraic Notation (e.g., "e4", "Nf3", "O-O") */
  gchar *from;             /* Source square (e.g., "e2") - may be NULL for castling */
  gchar *to;               /* Destination square (e.g., "e4") */
  GnostrChessPiece piece;  /* Piece type moved */
  GnostrChessColor color;  /* Color of piece moved */
  gboolean is_capture;     /* Whether this is a capture */
  gboolean is_check;       /* Whether this gives check */
  gboolean is_checkmate;   /* Whether this is checkmate */
  gboolean is_castling_kingside;   /* O-O */
  gboolean is_castling_queenside;  /* O-O-O */
  gchar *promotion;        /* Piece promoted to (e.g., "Q") or NULL */
  gchar *comment;          /* Optional comment for this move */
} GnostrChessMove;

/**
 * GnostrChessResult:
 * Game result enumeration.
 */
typedef enum {
  GNOSTR_CHESS_RESULT_UNKNOWN = 0,  /* Game in progress or unknown */
  GNOSTR_CHESS_RESULT_WHITE_WINS,   /* 1-0 */
  GNOSTR_CHESS_RESULT_BLACK_WINS,   /* 0-1 */
  GNOSTR_CHESS_RESULT_DRAW          /* 1/2-1/2 */
} GnostrChessResult;

/**
 * GnostrChessGame:
 * Structure containing a complete parsed chess game.
 * All strings and arrays are owned by the structure.
 * Free with gnostr_chess_game_free().
 */
typedef struct {
  /* Event metadata */
  gchar *event_id;         /* Nostr event ID (hex) */
  gchar *pubkey;           /* Author pubkey (hex) */
  gint64 created_at;       /* Event timestamp */

  /* PGN Header Tags */
  gchar *event_name;       /* [Event "..."] */
  gchar *site;             /* [Site "..."] */
  gchar *date;             /* [Date "..."] */
  gchar *round;            /* [Round "..."] */
  gchar *white_player;     /* [White "..."] */
  gchar *black_player;     /* [Black "..."] */
  GnostrChessResult result; /* Parsed result */
  gchar *result_string;    /* Original result string */
  gchar *eco;              /* [ECO "..."] - Opening code */
  gchar *opening;          /* [Opening "..."] - Opening name */

  /* Optional header tags */
  gchar *white_elo;        /* [WhiteElo "..."] */
  gchar *black_elo;        /* [BlackElo "..."] */
  gchar *time_control;     /* [TimeControl "..."] */
  gchar *termination;      /* [Termination "..."] */

  /* Moves */
  GnostrChessMove **moves; /* NULL-terminated array of moves */
  gsize moves_count;       /* Total number of half-moves (plies) */

  /* Current position state (for navigation) */
  gint current_ply;        /* Current ply position (0 = starting position) */
  GnostrChessSquare board[CHESS_SQUARES];  /* Current board state */

  /* Last move highlighting */
  gint last_move_from;     /* Square index of last move source (-1 if none) */
  gint last_move_to;       /* Square index of last move destination (-1 if none) */

  /* Raw PGN text */
  gchar *pgn_text;         /* Original PGN content */
} GnostrChessGame;

/**
 * gnostr_chess_game_new:
 *
 * Creates a new empty chess game structure.
 * Use gnostr_chess_game_free() to free.
 *
 * Returns: (transfer full): New game structure.
 */
GnostrChessGame *gnostr_chess_game_new(void);

/**
 * gnostr_chess_game_free:
 * @game: The game to free, may be NULL.
 *
 * Frees a chess game structure and all its contents.
 */
void gnostr_chess_game_free(GnostrChessGame *game);

/**
 * gnostr_chess_move_free:
 * @move: The move to free, may be NULL.
 *
 * Frees a chess move structure.
 */
void gnostr_chess_move_free(GnostrChessMove *move);

/**
 * gnostr_chess_parse_pgn:
 * @pgn_text: PGN text to parse.
 *
 * Parses a complete PGN game text into a game structure.
 * Handles standard PGN format with headers and move text.
 *
 * Returns: (transfer full) (nullable): Parsed game or NULL on error.
 */
GnostrChessGame *gnostr_chess_parse_pgn(const char *pgn_text);

/**
 * gnostr_chess_parse_from_json:
 * @event_json: JSON string containing the complete Nostr event.
 *
 * Parses a kind 64 chess event from JSON.
 * The JSON should be a complete Nostr event object.
 *
 * Returns: (transfer full) (nullable): Parsed game or NULL on error.
 */
GnostrChessGame *gnostr_chess_parse_from_json(const char *event_json);

/**
 * gnostr_chess_game_set_position:
 * @game: The chess game.
 * @ply: Ply number to navigate to (0 = starting position).
 *
 * Sets the current position to the specified ply and updates the board.
 * Ply 0 is the starting position, ply 1 is after White's first move, etc.
 *
 * Returns: TRUE if successful, FALSE if ply is out of range.
 */
gboolean gnostr_chess_game_set_position(GnostrChessGame *game, gint ply);

/**
 * gnostr_chess_game_first:
 * @game: The chess game.
 *
 * Moves to the starting position (ply 0).
 */
void gnostr_chess_game_first(GnostrChessGame *game);

/**
 * gnostr_chess_game_last:
 * @game: The chess game.
 *
 * Moves to the final position.
 */
void gnostr_chess_game_last(GnostrChessGame *game);

/**
 * gnostr_chess_game_prev:
 * @game: The chess game.
 *
 * Moves back one ply if possible.
 *
 * Returns: TRUE if moved, FALSE if already at start.
 */
gboolean gnostr_chess_game_prev(GnostrChessGame *game);

/**
 * gnostr_chess_game_next:
 * @game: The chess game.
 *
 * Moves forward one ply if possible.
 *
 * Returns: TRUE if moved, FALSE if already at end.
 */
gboolean gnostr_chess_game_next(GnostrChessGame *game);

/**
 * gnostr_chess_game_get_current_move:
 * @game: The chess game.
 *
 * Gets the move that led to the current position.
 *
 * Returns: (transfer none) (nullable): Current move or NULL if at start.
 */
GnostrChessMove *gnostr_chess_game_get_current_move(GnostrChessGame *game);

/**
 * gnostr_chess_game_get_move_at:
 * @game: The chess game.
 * @ply: Ply number (1-based, as each ply is a half-move).
 *
 * Gets the move at the specified ply.
 *
 * Returns: (transfer none) (nullable): Move or NULL if invalid ply.
 */
GnostrChessMove *gnostr_chess_game_get_move_at(GnostrChessGame *game, gint ply);

/**
 * gnostr_chess_get_piece_at:
 * @game: The chess game.
 * @file: File (0-7 for a-h).
 * @rank: Rank (0-7 for 1-8).
 *
 * Gets the piece at the specified square in the current position.
 *
 * Returns: Pointer to square contents.
 */
const GnostrChessSquare *gnostr_chess_get_piece_at(GnostrChessGame *game, gint file, gint rank);

/**
 * gnostr_chess_get_piece_at_index:
 * @game: The chess game.
 * @index: Square index (0-63, a1=0, h8=63).
 *
 * Gets the piece at the specified square index.
 *
 * Returns: Pointer to square contents.
 */
const GnostrChessSquare *gnostr_chess_get_piece_at_index(GnostrChessGame *game, gint index);

/**
 * gnostr_chess_square_to_index:
 * @file: File (0-7 for a-h).
 * @rank: Rank (0-7 for 1-8).
 *
 * Converts file and rank to square index.
 *
 * Returns: Square index (0-63).
 */
gint gnostr_chess_square_to_index(gint file, gint rank);

/**
 * gnostr_chess_index_to_file:
 * @index: Square index (0-63).
 *
 * Gets the file from a square index.
 *
 * Returns: File (0-7 for a-h).
 */
gint gnostr_chess_index_to_file(gint index);

/**
 * gnostr_chess_index_to_rank:
 * @index: Square index (0-63).
 *
 * Gets the rank from a square index.
 *
 * Returns: Rank (0-7 for 1-8).
 */
gint gnostr_chess_index_to_rank(gint index);

/**
 * gnostr_chess_square_name:
 * @file: File (0-7 for a-h).
 * @rank: Rank (0-7 for 1-8).
 *
 * Gets the algebraic notation for a square.
 *
 * Returns: (transfer full): Square name (e.g., "e4"). Caller frees.
 */
gchar *gnostr_chess_square_name(gint file, gint rank);

/**
 * gnostr_chess_parse_square:
 * @name: Square name (e.g., "e4").
 * @out_file: (out) (optional): Output for file.
 * @out_rank: (out) (optional): Output for rank.
 *
 * Parses a square name into file and rank.
 *
 * Returns: TRUE if valid, FALSE otherwise.
 */
gboolean gnostr_chess_parse_square(const char *name, gint *out_file, gint *out_rank);

/**
 * gnostr_chess_piece_char:
 * @piece: Piece type.
 *
 * Gets the single-character representation of a piece.
 *
 * Returns: Character (K, Q, R, B, N, P or space for none).
 */
gchar gnostr_chess_piece_char(GnostrChessPiece piece);

/**
 * gnostr_chess_piece_unicode:
 * @piece: Piece type.
 * @color: Piece color.
 *
 * Gets the Unicode chess symbol for a piece.
 *
 * Returns: (transfer none): Unicode string for the piece.
 */
const gchar *gnostr_chess_piece_unicode(GnostrChessPiece piece, GnostrChessColor color);

/**
 * gnostr_chess_result_to_string:
 * @result: Game result.
 *
 * Gets the PGN result string.
 *
 * Returns: (transfer none): Result string ("1-0", "0-1", "1/2-1/2", or "*").
 */
const gchar *gnostr_chess_result_to_string(GnostrChessResult result);

/**
 * gnostr_chess_format_moves:
 * @game: The chess game.
 * @up_to_ply: Format moves up to this ply (-1 for all moves).
 *
 * Formats the move list in standard notation (e.g., "1. e4 e5 2. Nf3 Nc6").
 *
 * Returns: (transfer full): Formatted move string. Caller frees.
 */
gchar *gnostr_chess_format_moves(GnostrChessGame *game, gint up_to_ply);

/**
 * gnostr_chess_is_chess_event:
 * @kind: Event kind number.
 *
 * Returns: TRUE if kind is a chess event (64).
 */
gboolean gnostr_chess_is_chess_event(gint kind);

/**
 * gnostr_chess_setup_initial_position:
 * @board: Array of 64 squares to initialize.
 *
 * Sets up the standard starting position.
 */
void gnostr_chess_setup_initial_position(GnostrChessSquare board[CHESS_SQUARES]);

/**
 * gnostr_chess_game_export_pgn:
 * @game: The chess game.
 *
 * Exports the game as a complete PGN string with headers and moves.
 *
 * Returns: (transfer full): PGN text. Caller frees.
 */
gchar *gnostr_chess_game_export_pgn(GnostrChessGame *game);

G_END_DECLS

#endif /* NIP64_CHESS_H */
