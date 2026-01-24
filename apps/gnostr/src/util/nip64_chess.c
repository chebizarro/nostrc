/*
 * nip64_chess.c - NIP-64 Chess (PGN) Event Utilities
 *
 * Implementation of PGN parsing and chess game state management.
 */

#include "nip64_chess.h"
#include <string.h>
#include <ctype.h>
#include <jansson.h>

/* Unicode chess pieces (white pieces on top, black pieces below) */
static const gchar *PIECE_UNICODE[7][3] = {
  /* NONE */   { " ", " ", " " },
  /* PAWN */   { "", "\xe2\x99\x99", "\xe2\x99\x9f" },  /* White: U+2659, Black: U+265F */
  /* KNIGHT */ { "", "\xe2\x99\x98", "\xe2\x99\x9e" },  /* White: U+2658, Black: U+265E */
  /* BISHOP */ { "", "\xe2\x99\x97", "\xe2\x99\x9d" },  /* White: U+2657, Black: U+265D */
  /* ROOK */   { "", "\xe2\x99\x96", "\xe2\x99\x9c" },  /* White: U+2656, Black: U+265C */
  /* QUEEN */  { "", "\xe2\x99\x95", "\xe2\x99\x9b" },  /* White: U+2655, Black: U+265B */
  /* KING */   { "", "\xe2\x99\x94", "\xe2\x99\x9a" }   /* White: U+2654, Black: U+265A */
};

/* Piece character mapping */
static const gchar PIECE_CHARS[] = " PNBRQK";

/* ---- Memory management ---- */

GnostrChessGame *gnostr_chess_game_new(void) {
  GnostrChessGame *game = g_new0(GnostrChessGame, 1);
  game->current_ply = 0;
  game->last_move_from = -1;
  game->last_move_to = -1;
  game->result = GNOSTR_CHESS_RESULT_UNKNOWN;
  gnostr_chess_setup_initial_position(game->board);
  return game;
}

void gnostr_chess_move_free(GnostrChessMove *move) {
  if (!move) return;
  g_free(move->san);
  g_free(move->from);
  g_free(move->to);
  g_free(move->promotion);
  g_free(move->comment);
  g_free(move);
}

void gnostr_chess_game_free(GnostrChessGame *game) {
  if (!game) return;

  g_free(game->event_id);
  g_free(game->pubkey);
  g_free(game->event_name);
  g_free(game->site);
  g_free(game->date);
  g_free(game->round);
  g_free(game->white_player);
  g_free(game->black_player);
  g_free(game->result_string);
  g_free(game->eco);
  g_free(game->opening);
  g_free(game->white_elo);
  g_free(game->black_elo);
  g_free(game->time_control);
  g_free(game->termination);
  g_free(game->pgn_text);

  if (game->moves) {
    for (gsize i = 0; i < game->moves_count; i++) {
      gnostr_chess_move_free(game->moves[i]);
    }
    g_free(game->moves);
  }

  g_free(game);
}

/* ---- Board utilities ---- */

void gnostr_chess_setup_initial_position(GnostrChessSquare board[CHESS_SQUARES]) {
  memset(board, 0, sizeof(GnostrChessSquare) * CHESS_SQUARES);

  /* White pieces on ranks 1-2 */
  GnostrChessPiece back_rank[] = {
    GNOSTR_CHESS_PIECE_ROOK, GNOSTR_CHESS_PIECE_KNIGHT, GNOSTR_CHESS_PIECE_BISHOP,
    GNOSTR_CHESS_PIECE_QUEEN, GNOSTR_CHESS_PIECE_KING,
    GNOSTR_CHESS_PIECE_BISHOP, GNOSTR_CHESS_PIECE_KNIGHT, GNOSTR_CHESS_PIECE_ROOK
  };

  for (int file = 0; file < 8; file++) {
    /* White back rank (rank 1, index 0-7) */
    board[file].piece = back_rank[file];
    board[file].color = GNOSTR_CHESS_COLOR_WHITE;

    /* White pawns (rank 2, index 8-15) */
    board[8 + file].piece = GNOSTR_CHESS_PIECE_PAWN;
    board[8 + file].color = GNOSTR_CHESS_COLOR_WHITE;

    /* Black pawns (rank 7, index 48-55) */
    board[48 + file].piece = GNOSTR_CHESS_PIECE_PAWN;
    board[48 + file].color = GNOSTR_CHESS_COLOR_BLACK;

    /* Black back rank (rank 8, index 56-63) */
    board[56 + file].piece = back_rank[file];
    board[56 + file].color = GNOSTR_CHESS_COLOR_BLACK;
  }
}

gint gnostr_chess_square_to_index(gint file, gint rank) {
  if (file < 0 || file > 7 || rank < 0 || rank > 7) return -1;
  return rank * 8 + file;
}

gint gnostr_chess_index_to_file(gint index) {
  if (index < 0 || index > 63) return -1;
  return index % 8;
}

gint gnostr_chess_index_to_rank(gint index) {
  if (index < 0 || index > 63) return -1;
  return index / 8;
}

gchar *gnostr_chess_square_name(gint file, gint rank) {
  if (file < 0 || file > 7 || rank < 0 || rank > 7) return NULL;
  return g_strdup_printf("%c%d", 'a' + file, rank + 1);
}

gboolean gnostr_chess_parse_square(const char *name, gint *out_file, gint *out_rank) {
  if (!name || strlen(name) != 2) return FALSE;

  gint file = g_ascii_tolower(name[0]) - 'a';
  gint rank = name[1] - '1';

  if (file < 0 || file > 7 || rank < 0 || rank > 7) return FALSE;

  if (out_file) *out_file = file;
  if (out_rank) *out_rank = rank;
  return TRUE;
}

gchar gnostr_chess_piece_char(GnostrChessPiece piece) {
  if (piece < 0 || piece > 6) return ' ';
  return PIECE_CHARS[piece];
}

const gchar *gnostr_chess_piece_unicode(GnostrChessPiece piece, GnostrChessColor color) {
  if (piece < 0 || piece > 6 || color < 0 || color > 2) {
    return " ";
  }
  return PIECE_UNICODE[piece][color];
}

const GnostrChessSquare *gnostr_chess_get_piece_at(GnostrChessGame *game, gint file, gint rank) {
  g_return_val_if_fail(game != NULL, NULL);
  gint index = gnostr_chess_square_to_index(file, rank);
  if (index < 0) return NULL;
  return &game->board[index];
}

const GnostrChessSquare *gnostr_chess_get_piece_at_index(GnostrChessGame *game, gint index) {
  g_return_val_if_fail(game != NULL, NULL);
  if (index < 0 || index > 63) return NULL;
  return &game->board[index];
}

const gchar *gnostr_chess_result_to_string(GnostrChessResult result) {
  switch (result) {
    case GNOSTR_CHESS_RESULT_WHITE_WINS: return "1-0";
    case GNOSTR_CHESS_RESULT_BLACK_WINS: return "0-1";
    case GNOSTR_CHESS_RESULT_DRAW:       return "1/2-1/2";
    default:                             return "*";
  }
}

gboolean gnostr_chess_is_chess_event(gint kind) {
  return kind == NOSTR_KIND_CHESS;
}

/* ---- PGN Parsing ---- */

/* Helper: Parse a PGN header tag line like [Event "World Championship"] */
static gboolean parse_pgn_header(const gchar *line, gchar **out_name, gchar **out_value) {
  if (!line || line[0] != '[') return FALSE;

  /* Find the tag name and value */
  const gchar *name_start = line + 1;
  const gchar *space = strchr(name_start, ' ');
  if (!space) return FALSE;

  /* Extract name */
  gchar *name = g_strndup(name_start, space - name_start);

  /* Find quoted value */
  const gchar *quote1 = strchr(space, '"');
  if (!quote1) {
    g_free(name);
    return FALSE;
  }

  const gchar *quote2 = strrchr(quote1 + 1, '"');
  if (!quote2 || quote2 <= quote1) {
    g_free(name);
    return FALSE;
  }

  gchar *value = g_strndup(quote1 + 1, quote2 - quote1 - 1);

  *out_name = name;
  *out_value = value;
  return TRUE;
}

/* Helper: Parse result string */
static GnostrChessResult parse_result_string(const gchar *result) {
  if (!result) return GNOSTR_CHESS_RESULT_UNKNOWN;
  if (g_strcmp0(result, "1-0") == 0) return GNOSTR_CHESS_RESULT_WHITE_WINS;
  if (g_strcmp0(result, "0-1") == 0) return GNOSTR_CHESS_RESULT_BLACK_WINS;
  if (g_strcmp0(result, "1/2-1/2") == 0) return GNOSTR_CHESS_RESULT_DRAW;
  return GNOSTR_CHESS_RESULT_UNKNOWN;
}

/* Helper: Parse a single SAN move and create a GnostrChessMove */
static GnostrChessMove *parse_san_move(const gchar *san, GnostrChessColor color) {
  if (!san || !*san) return NULL;

  GnostrChessMove *move = g_new0(GnostrChessMove, 1);
  move->san = g_strdup(san);
  move->color = color;

  /* Check for castling */
  if (g_strcmp0(san, "O-O") == 0 || g_strcmp0(san, "0-0") == 0) {
    move->is_castling_kingside = TRUE;
    move->piece = GNOSTR_CHESS_PIECE_KING;
    return move;
  }
  if (g_strcmp0(san, "O-O-O") == 0 || g_strcmp0(san, "0-0-0") == 0) {
    move->is_castling_queenside = TRUE;
    move->piece = GNOSTR_CHESS_PIECE_KING;
    return move;
  }

  /* Parse the move notation */
  const gchar *p = san;
  gint len = strlen(san);

  /* Check for check/checkmate at end */
  if (len > 0 && (san[len - 1] == '+' || san[len - 1] == '#')) {
    move->is_check = (san[len - 1] == '+');
    move->is_checkmate = (san[len - 1] == '#');
    len--;
  }

  /* Check for promotion (=Q, =R, =B, =N) */
  if (len > 2 && san[len - 2] == '=') {
    move->promotion = g_strndup(san + len - 1, 1);
    len -= 2;
  }

  /* Identify piece type */
  if (p[0] >= 'A' && p[0] <= 'Z' && p[0] != 'O') {
    switch (p[0]) {
      case 'K': move->piece = GNOSTR_CHESS_PIECE_KING; break;
      case 'Q': move->piece = GNOSTR_CHESS_PIECE_QUEEN; break;
      case 'R': move->piece = GNOSTR_CHESS_PIECE_ROOK; break;
      case 'B': move->piece = GNOSTR_CHESS_PIECE_BISHOP; break;
      case 'N': move->piece = GNOSTR_CHESS_PIECE_KNIGHT; break;
      default: move->piece = GNOSTR_CHESS_PIECE_PAWN; break;
    }
    p++;
  } else {
    move->piece = GNOSTR_CHESS_PIECE_PAWN;
  }

  /* Check for capture */
  if (strchr(p, 'x') != NULL) {
    move->is_capture = TRUE;
  }

  /* Extract destination square (last two chars before check/promotion) */
  /* Find the destination square - it's the file+rank at the end */
  const gchar *dest_start = san;
  for (gint i = len - 1; i >= 0; i--) {
    if (san[i] >= 'a' && san[i] <= 'h' && i + 1 < len &&
        san[i + 1] >= '1' && san[i + 1] <= '8') {
      dest_start = san + i;
      break;
    }
  }

  if (dest_start && dest_start[0] >= 'a' && dest_start[0] <= 'h' &&
      dest_start[1] >= '1' && dest_start[1] <= '8') {
    move->to = g_strndup(dest_start, 2);
  }

  return move;
}

/* Helper: Skip whitespace and comments in PGN movetext */
static const gchar *skip_ws_and_comments(const gchar *p) {
  while (*p) {
    /* Skip whitespace */
    while (*p && g_ascii_isspace(*p)) p++;

    /* Skip comments in braces {} */
    if (*p == '{') {
      const gchar *end = strchr(p, '}');
      if (end) {
        p = end + 1;
        continue;
      }
    }

    /* Skip comments in parentheses () - variations */
    if (*p == '(') {
      int depth = 1;
      p++;
      while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        p++;
      }
      continue;
    }

    /* Skip comments starting with ; */
    if (*p == ';') {
      while (*p && *p != '\n') p++;
      continue;
    }

    break;
  }
  return p;
}

/* Helper: Parse movetext section */
static void parse_movetext(GnostrChessGame *game, const gchar *movetext) {
  GPtrArray *moves = g_ptr_array_new();
  const gchar *p = movetext;
  GnostrChessColor current_color = GNOSTR_CHESS_COLOR_WHITE;

  while (*p) {
    p = skip_ws_and_comments(p);
    if (!*p) break;

    /* Skip move numbers (e.g., "1." or "1...") */
    if (g_ascii_isdigit(*p)) {
      while (*p && g_ascii_isdigit(*p)) p++;
      while (*p == '.') p++;
      p = skip_ws_and_comments(p);
      if (!*p) break;
    }

    /* Check for result */
    if (g_str_has_prefix(p, "1-0") || g_str_has_prefix(p, "0-1") ||
        g_str_has_prefix(p, "1/2-1/2") || *p == '*') {
      break;
    }

    /* Extract next move token */
    const gchar *move_start = p;
    while (*p && !g_ascii_isspace(*p) && *p != '{' && *p != '(' && *p != ')') {
      p++;
    }

    if (p > move_start) {
      gchar *san = g_strndup(move_start, p - move_start);
      GnostrChessMove *move = parse_san_move(san, current_color);
      g_free(san);

      if (move) {
        g_ptr_array_add(moves, move);
        /* Alternate colors */
        current_color = (current_color == GNOSTR_CHESS_COLOR_WHITE)
                          ? GNOSTR_CHESS_COLOR_BLACK
                          : GNOSTR_CHESS_COLOR_WHITE;
      }
    }
  }

  /* Store moves in game */
  game->moves_count = moves->len;
  game->moves = g_new0(GnostrChessMove *, moves->len + 1);
  for (guint i = 0; i < moves->len; i++) {
    game->moves[i] = g_ptr_array_index(moves, i);
  }
  g_ptr_array_free(moves, TRUE);
}

GnostrChessGame *gnostr_chess_parse_pgn(const char *pgn_text) {
  if (!pgn_text || !*pgn_text) return NULL;

  GnostrChessGame *game = gnostr_chess_game_new();
  game->pgn_text = g_strdup(pgn_text);

  /* Split into lines for header parsing */
  gchar **lines = g_strsplit(pgn_text, "\n", -1);
  GString *movetext = g_string_new(NULL);
  gboolean in_movetext = FALSE;

  for (gint i = 0; lines[i]; i++) {
    gchar *line = g_strstrip(lines[i]);

    if (!*line) {
      /* Empty line marks start of movetext after headers */
      if (!in_movetext && i > 0) {
        in_movetext = TRUE;
      }
      continue;
    }

    if (line[0] == '[' && !in_movetext) {
      /* Parse header */
      gchar *name = NULL, *value = NULL;
      if (parse_pgn_header(line, &name, &value)) {
        if (g_strcmp0(name, "Event") == 0) {
          game->event_name = value;
          value = NULL;
        } else if (g_strcmp0(name, "Site") == 0) {
          game->site = value;
          value = NULL;
        } else if (g_strcmp0(name, "Date") == 0) {
          game->date = value;
          value = NULL;
        } else if (g_strcmp0(name, "Round") == 0) {
          game->round = value;
          value = NULL;
        } else if (g_strcmp0(name, "White") == 0) {
          game->white_player = value;
          value = NULL;
        } else if (g_strcmp0(name, "Black") == 0) {
          game->black_player = value;
          value = NULL;
        } else if (g_strcmp0(name, "Result") == 0) {
          game->result_string = value;
          game->result = parse_result_string(value);
          value = NULL;
        } else if (g_strcmp0(name, "ECO") == 0) {
          game->eco = value;
          value = NULL;
        } else if (g_strcmp0(name, "Opening") == 0) {
          game->opening = value;
          value = NULL;
        } else if (g_strcmp0(name, "WhiteElo") == 0) {
          game->white_elo = value;
          value = NULL;
        } else if (g_strcmp0(name, "BlackElo") == 0) {
          game->black_elo = value;
          value = NULL;
        } else if (g_strcmp0(name, "TimeControl") == 0) {
          game->time_control = value;
          value = NULL;
        } else if (g_strcmp0(name, "Termination") == 0) {
          game->termination = value;
          value = NULL;
        }
        g_free(name);
        g_free(value);
      }
    } else {
      /* Movetext */
      in_movetext = TRUE;
      if (movetext->len > 0) {
        g_string_append_c(movetext, ' ');
      }
      g_string_append(movetext, line);
    }
  }

  g_strfreev(lines);

  /* Parse the movetext */
  if (movetext->len > 0) {
    parse_movetext(game, movetext->str);
  }
  g_string_free(movetext, TRUE);

  return game;
}

GnostrChessGame *gnostr_chess_parse_from_json(const char *event_json) {
  if (!event_json || !*event_json) return NULL;

  json_error_t err;
  json_t *root = json_loads(event_json, 0, &err);
  if (!root) {
    g_debug("nip64: JSON parse error: %s", err.text);
    return NULL;
  }

  /* Check kind */
  json_t *kind_j = json_object_get(root, "kind");
  if (!kind_j || !json_is_integer(kind_j) || json_integer_value(kind_j) != NOSTR_KIND_CHESS) {
    json_decref(root);
    return NULL;
  }

  /* Get content (PGN text) */
  json_t *content_j = json_object_get(root, "content");
  if (!content_j || !json_is_string(content_j)) {
    json_decref(root);
    return NULL;
  }

  const char *pgn_text = json_string_value(content_j);
  GnostrChessGame *game = gnostr_chess_parse_pgn(pgn_text);

  if (game) {
    /* Extract event metadata */
    json_t *id_j = json_object_get(root, "id");
    if (id_j && json_is_string(id_j)) {
      game->event_id = g_strdup(json_string_value(id_j));
    }

    json_t *pubkey_j = json_object_get(root, "pubkey");
    if (pubkey_j && json_is_string(pubkey_j)) {
      game->pubkey = g_strdup(json_string_value(pubkey_j));
    }

    json_t *created_at_j = json_object_get(root, "created_at");
    if (created_at_j && json_is_integer(created_at_j)) {
      game->created_at = json_integer_value(created_at_j);
    }
  }

  json_decref(root);
  return game;
}

/* ---- Move execution ---- */

/* Helper: Find a piece that can make the move (for disambiguation) */
static gint find_piece_for_move(GnostrChessSquare board[CHESS_SQUARES],
                                 GnostrChessMove *move,
                                 gint to_file, gint to_rank) {
  GnostrChessPiece piece = move->piece;
  GnostrChessColor color = move->color;

  /* For each square, check if it contains the right piece and can reach the target */
  for (gint i = 0; i < 64; i++) {
    if (board[i].piece != piece || board[i].color != color) continue;

    gint from_file = gnostr_chess_index_to_file(i);
    gint from_rank = gnostr_chess_index_to_rank(i);

    /* Check if the piece can reach the target (simplified, doesn't check blocking) */
    gboolean can_reach = FALSE;

    switch (piece) {
      case GNOSTR_CHESS_PIECE_PAWN: {
        gint direction = (color == GNOSTR_CHESS_COLOR_WHITE) ? 1 : -1;
        if (move->is_capture) {
          /* Diagonal capture */
          can_reach = (abs(from_file - to_file) == 1) &&
                      (to_rank - from_rank == direction);
        } else {
          /* Forward move */
          can_reach = (from_file == to_file) &&
                      ((to_rank - from_rank == direction) ||
                       ((from_rank == (color == GNOSTR_CHESS_COLOR_WHITE ? 1 : 6)) &&
                        (to_rank - from_rank == 2 * direction)));
        }
        break;
      }
      case GNOSTR_CHESS_PIECE_KNIGHT:
        can_reach = (abs(from_file - to_file) == 2 && abs(from_rank - to_rank) == 1) ||
                    (abs(from_file - to_file) == 1 && abs(from_rank - to_rank) == 2);
        break;
      case GNOSTR_CHESS_PIECE_BISHOP:
        can_reach = (abs(from_file - to_file) == abs(from_rank - to_rank));
        break;
      case GNOSTR_CHESS_PIECE_ROOK:
        can_reach = (from_file == to_file || from_rank == to_rank);
        break;
      case GNOSTR_CHESS_PIECE_QUEEN:
        can_reach = (from_file == to_file || from_rank == to_rank ||
                     abs(from_file - to_file) == abs(from_rank - to_rank));
        break;
      case GNOSTR_CHESS_PIECE_KING:
        can_reach = (abs(from_file - to_file) <= 1 && abs(from_rank - to_rank) <= 1);
        break;
      default:
        break;
    }

    if (can_reach) {
      return i;
    }
  }

  return -1;
}

/* Helper: Execute a move on the board */
static void execute_move(GnostrChessSquare board[CHESS_SQUARES], GnostrChessMove *move,
                         gint *out_from, gint *out_to) {
  *out_from = -1;
  *out_to = -1;

  if (move->is_castling_kingside) {
    gint rank = (move->color == GNOSTR_CHESS_COLOR_WHITE) ? 0 : 7;
    gint king_from = gnostr_chess_square_to_index(4, rank);
    gint king_to = gnostr_chess_square_to_index(6, rank);
    gint rook_from = gnostr_chess_square_to_index(7, rank);
    gint rook_to = gnostr_chess_square_to_index(5, rank);

    board[king_to] = board[king_from];
    board[king_from].piece = GNOSTR_CHESS_PIECE_NONE;
    board[king_from].color = GNOSTR_CHESS_COLOR_NONE;
    board[rook_to] = board[rook_from];
    board[rook_from].piece = GNOSTR_CHESS_PIECE_NONE;
    board[rook_from].color = GNOSTR_CHESS_COLOR_NONE;

    *out_from = king_from;
    *out_to = king_to;
    return;
  }

  if (move->is_castling_queenside) {
    gint rank = (move->color == GNOSTR_CHESS_COLOR_WHITE) ? 0 : 7;
    gint king_from = gnostr_chess_square_to_index(4, rank);
    gint king_to = gnostr_chess_square_to_index(2, rank);
    gint rook_from = gnostr_chess_square_to_index(0, rank);
    gint rook_to = gnostr_chess_square_to_index(3, rank);

    board[king_to] = board[king_from];
    board[king_from].piece = GNOSTR_CHESS_PIECE_NONE;
    board[king_from].color = GNOSTR_CHESS_COLOR_NONE;
    board[rook_to] = board[rook_from];
    board[rook_from].piece = GNOSTR_CHESS_PIECE_NONE;
    board[rook_from].color = GNOSTR_CHESS_COLOR_NONE;

    *out_from = king_from;
    *out_to = king_to;
    return;
  }

  /* Parse destination square */
  if (!move->to) return;

  gint to_file, to_rank;
  if (!gnostr_chess_parse_square(move->to, &to_file, &to_rank)) return;

  gint to_index = gnostr_chess_square_to_index(to_file, to_rank);

  /* Find the piece that made this move */
  gint from_index = find_piece_for_move(board, move, to_file, to_rank);
  if (from_index < 0) return;

  /* Execute the move */
  board[to_index] = board[from_index];
  board[from_index].piece = GNOSTR_CHESS_PIECE_NONE;
  board[from_index].color = GNOSTR_CHESS_COLOR_NONE;

  /* Handle promotion */
  if (move->promotion && move->promotion[0]) {
    switch (g_ascii_toupper(move->promotion[0])) {
      case 'Q': board[to_index].piece = GNOSTR_CHESS_PIECE_QUEEN; break;
      case 'R': board[to_index].piece = GNOSTR_CHESS_PIECE_ROOK; break;
      case 'B': board[to_index].piece = GNOSTR_CHESS_PIECE_BISHOP; break;
      case 'N': board[to_index].piece = GNOSTR_CHESS_PIECE_KNIGHT; break;
      default: break;
    }
  }

  *out_from = from_index;
  *out_to = to_index;
}

/* ---- Navigation ---- */

gboolean gnostr_chess_game_set_position(GnostrChessGame *game, gint ply) {
  g_return_val_if_fail(game != NULL, FALSE);

  if (ply < 0 || ply > (gint)game->moves_count) {
    return FALSE;
  }

  /* Reset to starting position */
  gnostr_chess_setup_initial_position(game->board);
  game->last_move_from = -1;
  game->last_move_to = -1;

  /* Replay moves up to the target ply */
  for (gint i = 0; i < ply; i++) {
    gint from, to;
    execute_move(game->board, game->moves[i], &from, &to);
    game->last_move_from = from;
    game->last_move_to = to;
  }

  game->current_ply = ply;
  return TRUE;
}

void gnostr_chess_game_first(GnostrChessGame *game) {
  g_return_if_fail(game != NULL);
  gnostr_chess_game_set_position(game, 0);
}

void gnostr_chess_game_last(GnostrChessGame *game) {
  g_return_if_fail(game != NULL);
  gnostr_chess_game_set_position(game, game->moves_count);
}

gboolean gnostr_chess_game_prev(GnostrChessGame *game) {
  g_return_val_if_fail(game != NULL, FALSE);
  if (game->current_ply <= 0) return FALSE;
  return gnostr_chess_game_set_position(game, game->current_ply - 1);
}

gboolean gnostr_chess_game_next(GnostrChessGame *game) {
  g_return_val_if_fail(game != NULL, FALSE);
  if (game->current_ply >= (gint)game->moves_count) return FALSE;
  return gnostr_chess_game_set_position(game, game->current_ply + 1);
}

GnostrChessMove *gnostr_chess_game_get_current_move(GnostrChessGame *game) {
  g_return_val_if_fail(game != NULL, NULL);
  if (game->current_ply <= 0 || game->current_ply > (gint)game->moves_count) {
    return NULL;
  }
  return game->moves[game->current_ply - 1];
}

GnostrChessMove *gnostr_chess_game_get_move_at(GnostrChessGame *game, gint ply) {
  g_return_val_if_fail(game != NULL, NULL);
  if (ply < 1 || ply > (gint)game->moves_count) {
    return NULL;
  }
  return game->moves[ply - 1];
}

/* ---- Formatting ---- */

gchar *gnostr_chess_format_moves(GnostrChessGame *game, gint up_to_ply) {
  g_return_val_if_fail(game != NULL, NULL);

  if (game->moves_count == 0) {
    return g_strdup("");
  }

  gint limit = (up_to_ply < 0) ? game->moves_count : MIN(up_to_ply, (gint)game->moves_count);
  GString *result = g_string_new(NULL);

  for (gint i = 0; i < limit; i++) {
    GnostrChessMove *move = game->moves[i];
    if (!move || !move->san) continue;

    /* Add move number for white's moves */
    if (i % 2 == 0) {
      if (result->len > 0) {
        g_string_append_c(result, ' ');
      }
      g_string_append_printf(result, "%d. ", (i / 2) + 1);
    } else {
      g_string_append_c(result, ' ');
    }

    g_string_append(result, move->san);
  }

  /* Append result if at end */
  if (up_to_ply < 0 || up_to_ply >= (gint)game->moves_count) {
    if (game->result != GNOSTR_CHESS_RESULT_UNKNOWN) {
      g_string_append_c(result, ' ');
      g_string_append(result, gnostr_chess_result_to_string(game->result));
    }
  }

  return g_string_free(result, FALSE);
}

gchar *gnostr_chess_game_export_pgn(GnostrChessGame *game) {
  g_return_val_if_fail(game != NULL, NULL);

  GString *pgn = g_string_new(NULL);

  /* Headers */
  g_string_append_printf(pgn, "[Event \"%s\"]\n",
    game->event_name ? game->event_name : "?");
  g_string_append_printf(pgn, "[Site \"%s\"]\n",
    game->site ? game->site : "?");
  g_string_append_printf(pgn, "[Date \"%s\"]\n",
    game->date ? game->date : "????.??.??");
  g_string_append_printf(pgn, "[Round \"%s\"]\n",
    game->round ? game->round : "?");
  g_string_append_printf(pgn, "[White \"%s\"]\n",
    game->white_player ? game->white_player : "?");
  g_string_append_printf(pgn, "[Black \"%s\"]\n",
    game->black_player ? game->black_player : "?");
  g_string_append_printf(pgn, "[Result \"%s\"]\n",
    gnostr_chess_result_to_string(game->result));

  if (game->eco) {
    g_string_append_printf(pgn, "[ECO \"%s\"]\n", game->eco);
  }
  if (game->opening) {
    g_string_append_printf(pgn, "[Opening \"%s\"]\n", game->opening);
  }
  if (game->white_elo) {
    g_string_append_printf(pgn, "[WhiteElo \"%s\"]\n", game->white_elo);
  }
  if (game->black_elo) {
    g_string_append_printf(pgn, "[BlackElo \"%s\"]\n", game->black_elo);
  }
  if (game->time_control) {
    g_string_append_printf(pgn, "[TimeControl \"%s\"]\n", game->time_control);
  }
  if (game->termination) {
    g_string_append_printf(pgn, "[Termination \"%s\"]\n", game->termination);
  }

  /* Blank line before moves */
  g_string_append_c(pgn, '\n');

  /* Moves */
  gchar *moves_str = gnostr_chess_format_moves(game, -1);
  if (moves_str && *moves_str) {
    /* Wrap lines at 80 characters */
    gint line_len = 0;
    const gchar *p = moves_str;
    while (*p) {
      const gchar *space = strchr(p, ' ');
      gint word_len = space ? (space - p) : (gint)strlen(p);

      if (line_len > 0 && line_len + 1 + word_len > 80) {
        g_string_append_c(pgn, '\n');
        line_len = 0;
      } else if (line_len > 0) {
        g_string_append_c(pgn, ' ');
        line_len++;
      }

      g_string_append_len(pgn, p, word_len);
      line_len += word_len;

      p += word_len;
      if (*p == ' ') p++;
    }
    g_string_append_c(pgn, '\n');
  }
  g_free(moves_str);

  return g_string_free(pgn, FALSE);
}
