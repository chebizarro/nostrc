/*
 * gnostr-chess-board.c - Interactive Chess Board Widget
 *
 * GTK4 widget for interactive chess play with legal move validation.
 * Supports click-to-move interaction, legal move highlighting,
 * and pawn promotion dialogs.
 *
 * See DESIGN-gnostr-chess-board.md for design details.
 */

#include "gnostr-chess-board.h"
#include "../util/chess_engine.h"
#include "../util/nip64_chess.h"
#include <math.h>

/* Default sizes */
#define DEFAULT_BOARD_SIZE 320
#define MIN_BOARD_SIZE 200
#define MAX_BOARD_SIZE 800

/* Board colors */
#define LIGHT_SQUARE_COLOR "#f0d9b5"
#define DARK_SQUARE_COLOR "#b58863"
#define SELECTED_SQUARE_COLOR "rgba(20, 85, 30, 0.5)"
#define LAST_MOVE_FROM_COLOR "rgba(155, 199, 0, 0.5)"
#define LAST_MOVE_TO_COLOR "rgba(155, 199, 0, 0.7)"
#define LEGAL_MOVE_DOT_COLOR "rgba(0, 0, 0, 0.15)"
#define LEGAL_CAPTURE_COLOR "rgba(0, 0, 0, 0.15)"
#define CHECK_HIGHLIGHT_COLOR "rgba(255, 0, 0, 0.4)"

/* Legal move dot size as fraction of square */
#define LEGAL_MOVE_DOT_RADIUS 0.15

struct _GnostrChessBoard {
  GtkWidget parent_instance;

  /* Drawing area */
  GtkWidget *board_drawing;

  /* Appearance settings */
  gint board_size;
  gboolean board_flipped;
  gboolean show_coordinates;
  gboolean show_legal_moves;
  gboolean animate_moves;
  gchar *light_square_color;
  gchar *dark_square_color;

  /* Game state */
  GnostrChessGame *game;
  ChessEngine *engine;
  gboolean is_interactive;
  GnostrChessColor player_color;  /* Which color the player controls */

  /* Selection state */
  gint selected_file;  /* -1 if none */
  gint selected_rank;

  /* Legal moves for selected piece */
  GList *legal_move_targets;  /* List of target square names */

  /* Pending promotion */
  gint pending_promotion_from_file;
  gint pending_promotion_from_rank;
  gint pending_promotion_to_file;
  gint pending_promotion_to_rank;
  GtkWidget *promotion_popover;

  /* Click gesture */
  GtkGesture *click_gesture;
};

G_DEFINE_TYPE(GnostrChessBoard, gnostr_chess_board, GTK_TYPE_WIDGET)

/* Signal IDs */
enum {
  SIGNAL_PIECE_SELECTED,
  SIGNAL_PIECE_DESELECTED,
  SIGNAL_MOVE_MADE,
  SIGNAL_ILLEGAL_MOVE_ATTEMPTED,
  SIGNAL_PROMOTION_REQUIRED,
  SIGNAL_GAME_OVER,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Forward declarations */
static void draw_board(GtkDrawingArea *area, cairo_t *cr,
                       int width, int height, gpointer user_data);
static void on_board_pressed(GtkGestureClick *gesture, gint n_press,
                             double x, double y, gpointer user_data);
static void coords_to_square(GnostrChessBoard *self, double x, double y,
                             gint *out_file, gint *out_rank);
static void update_legal_moves(GnostrChessBoard *self);
static void clear_selection(GnostrChessBoard *self);
static gboolean is_legal_move_target(GnostrChessBoard *self, gint file, gint rank);
static gboolean try_make_move(GnostrChessBoard *self, gint to_file, gint to_rank);
static void show_promotion_dialog(GnostrChessBoard *self,
                                   gint from_file, gint from_rank,
                                   gint to_file, gint to_rank);
static void sync_engine_position(GnostrChessBoard *self);
static gboolean is_promotion_move(GnostrChessBoard *self, gint from_file, gint from_rank,
                                   gint to_file, gint to_rank);

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static gboolean parse_hex_color(const char *hex, double *r, double *g, double *b) {
  if (!hex || hex[0] != '#' || strlen(hex) != 7) return FALSE;

  unsigned int ri, gi, bi;
  if (sscanf(hex + 1, "%2x%2x%2x", &ri, &gi, &bi) != 3) return FALSE;

  *r = ri / 255.0;
  *g = gi / 255.0;
  *b = bi / 255.0;
  return TRUE;
}

static void coords_to_square(GnostrChessBoard *self, double x, double y,
                             gint *out_file, gint *out_rank) {
  gint width = gtk_widget_get_width(self->board_drawing);
  gint height = gtk_widget_get_height(self->board_drawing);
  gint square_size = MIN(width, height) / 8;
  gint board_width = square_size * 8;
  gint board_height = square_size * 8;
  gint offset_x = (width - board_width) / 2;
  gint offset_y = (height - board_height) / 2;

  gint file = (gint)((x - offset_x) / square_size);
  gint rank = 7 - (gint)((y - offset_y) / square_size);

  if (self->board_flipped) {
    file = 7 - file;
    rank = 7 - rank;
  }

  *out_file = CLAMP(file, 0, 7);
  *out_rank = CLAMP(rank, 0, 7);
}

static gchar *file_rank_to_algebraic(gint file, gint rank) {
  return g_strdup_printf("%c%d", 'a' + file, rank + 1);
}

static void sync_engine_position(GnostrChessBoard *self) {
  if (!self->engine || !self->game) return;

  /* Build FEN from current game position */
  GString *fen = g_string_new(NULL);

  /* Piece placement */
  for (gint rank = 7; rank >= 0; rank--) {
    gint empty = 0;
    for (gint file = 0; file < 8; file++) {
      const GnostrChessSquare *sq = gnostr_chess_get_piece_at(self->game, file, rank);
      if (sq->piece == GNOSTR_CHESS_PIECE_NONE) {
        empty++;
      } else {
        if (empty > 0) {
          g_string_append_printf(fen, "%d", empty);
          empty = 0;
        }
        gchar piece_char = gnostr_chess_piece_char(sq->piece);
        if (sq->color == GNOSTR_CHESS_COLOR_BLACK) {
          piece_char = g_ascii_tolower(piece_char);
        }
        g_string_append_c(fen, piece_char);
      }
    }
    if (empty > 0) {
      g_string_append_printf(fen, "%d", empty);
    }
    if (rank > 0) {
      g_string_append_c(fen, '/');
    }
  }

  /* Side to move */
  gboolean white_to_move = (self->game->current_ply % 2 == 0);
  g_string_append_printf(fen, " %c ", white_to_move ? 'w' : 'b');

  /* Castling rights - simplified, assume all rights initially */
  g_string_append(fen, "KQkq ");

  /* En passant - simplified, set to '-' */
  g_string_append(fen, "- ");

  /* Halfmove clock and fullmove number */
  g_string_append_printf(fen, "0 %d", (self->game->current_ply / 2) + 1);

  chess_engine_set_fen(self->engine, fen->str);
  g_string_free(fen, TRUE);
}

static void update_legal_moves(GnostrChessBoard *self) {
  /* Clear previous legal moves */
  if (self->legal_move_targets) {
    g_list_free_full(self->legal_move_targets, g_free);
    self->legal_move_targets = NULL;
  }

  if (self->selected_file < 0 || !self->engine) return;

  /* Sync engine with current position */
  sync_engine_position(self);

  /* Get legal moves from selected square */
  gchar *from_sq = file_rank_to_algebraic(self->selected_file, self->selected_rank);
  self->legal_move_targets = chess_engine_get_legal_moves(self->engine, from_sq);
  g_free(from_sq);
}

static gboolean is_legal_move_target(GnostrChessBoard *self, gint file, gint rank) {
  if (!self->legal_move_targets) return FALSE;

  gchar *sq_name = file_rank_to_algebraic(file, rank);
  gboolean found = FALSE;

  for (GList *l = self->legal_move_targets; l != NULL; l = l->next) {
    if (g_strcmp0((gchar *)l->data, sq_name) == 0) {
      found = TRUE;
      break;
    }
  }

  g_free(sq_name);
  return found;
}

static void clear_selection(GnostrChessBoard *self) {
  if (self->selected_file >= 0) {
    self->selected_file = -1;
    self->selected_rank = -1;

    if (self->legal_move_targets) {
      g_list_free_full(self->legal_move_targets, g_free);
      self->legal_move_targets = NULL;
    }

    g_signal_emit(self, signals[SIGNAL_PIECE_DESELECTED], 0);
    gtk_widget_queue_draw(self->board_drawing);
  }
}

static gboolean is_promotion_move(GnostrChessBoard *self, gint from_file, gint from_rank,
                                   gint to_file, gint to_rank) {
  if (!self->game) return FALSE;

  const GnostrChessSquare *sq = gnostr_chess_get_piece_at(self->game, from_file, from_rank);
  if (sq->piece != GNOSTR_CHESS_PIECE_PAWN) return FALSE;

  /* White pawn reaching rank 8 (index 7) or black pawn reaching rank 1 (index 0) */
  if (sq->color == GNOSTR_CHESS_COLOR_WHITE && to_rank == 7) return TRUE;
  if (sq->color == GNOSTR_CHESS_COLOR_BLACK && to_rank == 0) return TRUE;

  return FALSE;
}

static void on_promotion_selected(GtkButton *button, gpointer user_data) {
  GnostrChessBoard *self = GNOSTR_CHESS_BOARD(user_data);

  const gchar *piece = g_object_get_data(G_OBJECT(button), "piece");
  if (!piece) return;

  gchar promotion = g_ascii_tolower(piece[0]);

  /* Dismiss popover */
  if (self->promotion_popover) {
    gtk_popover_popdown(GTK_POPOVER(self->promotion_popover));
  }

  /* Make the promotion move */
  gchar *from_sq = file_rank_to_algebraic(self->pending_promotion_from_file,
                                           self->pending_promotion_from_rank);
  gchar *to_sq = file_rank_to_algebraic(self->pending_promotion_to_file,
                                         self->pending_promotion_to_rank);

  sync_engine_position(self);

  if (chess_engine_make_move(self->engine, from_sq, to_sq, promotion)) {
    /* Update game state */
    gchar *uci = g_strdup_printf("%s%s%c", from_sq, to_sq, promotion);

    /* Apply move to our game state */
    gint from_idx = gnostr_chess_square_to_index(self->pending_promotion_from_file,
                                                   self->pending_promotion_from_rank);
    gint to_idx = gnostr_chess_square_to_index(self->pending_promotion_to_file,
                                                 self->pending_promotion_to_rank);

    /* Move the piece */
    GnostrChessSquare *from_square = &self->game->board[from_idx];
    GnostrChessSquare *to_square = &self->game->board[to_idx];

    to_square->color = from_square->color;

    /* Set promoted piece type */
    switch (promotion) {
      case 'q': to_square->piece = GNOSTR_CHESS_PIECE_QUEEN; break;
      case 'r': to_square->piece = GNOSTR_CHESS_PIECE_ROOK; break;
      case 'b': to_square->piece = GNOSTR_CHESS_PIECE_BISHOP; break;
      case 'n': to_square->piece = GNOSTR_CHESS_PIECE_KNIGHT; break;
      default: to_square->piece = GNOSTR_CHESS_PIECE_QUEEN; break;
    }

    from_square->piece = GNOSTR_CHESS_PIECE_NONE;
    from_square->color = GNOSTR_CHESS_COLOR_NONE;

    /* Update last move highlights */
    self->game->last_move_from = from_idx;
    self->game->last_move_to = to_idx;
    self->game->current_ply++;

    /* Emit move-made signal */
    gchar *san = g_strdup_printf("%s=%c", to_sq, g_ascii_toupper(promotion));
    g_signal_emit(self, signals[SIGNAL_MOVE_MADE], 0, san, uci);
    g_free(san);
    g_free(uci);

    /* Check for game over */
    sync_engine_position(self);
    if (chess_engine_is_checkmate(self->engine)) {
      GnostrChessResult result = (self->game->current_ply % 2 == 0) ?
        GNOSTR_CHESS_RESULT_BLACK_WINS : GNOSTR_CHESS_RESULT_WHITE_WINS;
      g_signal_emit(self, signals[SIGNAL_GAME_OVER], 0, (gint)result);
    } else if (chess_engine_is_stalemate(self->engine)) {
      g_signal_emit(self, signals[SIGNAL_GAME_OVER], 0, (gint)GNOSTR_CHESS_RESULT_DRAW);
    }
  }

  g_free(from_sq);
  g_free(to_sq);

  clear_selection(self);
  gtk_widget_queue_draw(self->board_drawing);
}

static void show_promotion_dialog(GnostrChessBoard *self,
                                   gint from_file, gint from_rank,
                                   gint to_file, gint to_rank) {
  /* Store pending promotion move */
  self->pending_promotion_from_file = from_file;
  self->pending_promotion_from_rank = from_rank;
  self->pending_promotion_to_file = to_file;
  self->pending_promotion_to_rank = to_rank;

  /* Create popover if needed */
  if (!self->promotion_popover) {
    self->promotion_popover = gtk_popover_new();
    gtk_widget_set_parent(self->promotion_popover, GTK_WIDGET(self));
  }

  /* Determine piece color for icons */
  const GnostrChessSquare *sq = gnostr_chess_get_piece_at(self->game, from_file, from_rank);
  gboolean is_white = (sq->color == GNOSTR_CHESS_COLOR_WHITE);

  /* Create button box */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);

  /* Piece buttons with Unicode chess symbols */
  const gchar *pieces[] = { "Q", "R", "B", "N" };
  const gchar *white_icons[] = {
    "\xe2\x99\x95",  /* White Queen */
    "\xe2\x99\x96",  /* White Rook */
    "\xe2\x99\x97",  /* White Bishop */
    "\xe2\x99\x98"   /* White Knight */
  };
  const gchar *black_icons[] = {
    "\xe2\x99\x9b",  /* Black Queen */
    "\xe2\x99\x9c",  /* Black Rook */
    "\xe2\x99\x9d",  /* Black Bishop */
    "\xe2\x99\x9e"   /* Black Knight */
  };

  for (int i = 0; i < 4; i++) {
    GtkWidget *btn = gtk_button_new_with_label(is_white ? white_icons[i] : black_icons[i]);
    gtk_widget_add_css_class(btn, "promotion-button");
    g_object_set_data(G_OBJECT(btn), "piece", (gpointer)pieces[i]);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_promotion_selected), self);
    gtk_box_append(GTK_BOX(box), btn);
  }

  gtk_popover_set_child(GTK_POPOVER(self->promotion_popover), box);

  /* Position popover at the promotion square */
  gint width = gtk_widget_get_width(self->board_drawing);
  gint height = gtk_widget_get_height(self->board_drawing);
  gint square_size = MIN(width, height) / 8;
  gint board_width = square_size * 8;
  gint board_height = square_size * 8;
  gint offset_x = (width - board_width) / 2;
  gint offset_y = (height - board_height) / 2;

  gint display_file = self->board_flipped ? (7 - to_file) : to_file;
  gint display_rank = self->board_flipped ? to_rank : (7 - to_rank);

  GdkRectangle rect = {
    .x = offset_x + display_file * square_size,
    .y = offset_y + display_rank * square_size,
    .width = square_size,
    .height = square_size
  };

  gtk_popover_set_pointing_to(GTK_POPOVER(self->promotion_popover), &rect);

  /* Emit promotion required signal */
  g_signal_emit(self, signals[SIGNAL_PROMOTION_REQUIRED], 0,
                gnostr_chess_square_to_index(from_file, from_rank),
                gnostr_chess_square_to_index(to_file, to_rank));

  gtk_popover_popup(GTK_POPOVER(self->promotion_popover));
}

static gboolean try_make_move(GnostrChessBoard *self, gint to_file, gint to_rank) {
  if (self->selected_file < 0 || !self->engine || !self->game) return FALSE;

  /* Check if this is a legal move */
  if (!is_legal_move_target(self, to_file, to_rank)) {
    g_signal_emit(self, signals[SIGNAL_ILLEGAL_MOVE_ATTEMPTED], 0);
    return FALSE;
  }

  /* Check for promotion */
  if (is_promotion_move(self, self->selected_file, self->selected_rank, to_file, to_rank)) {
    show_promotion_dialog(self, self->selected_file, self->selected_rank, to_file, to_rank);
    return TRUE;  /* Move will complete after promotion choice */
  }

  gchar *from_sq = file_rank_to_algebraic(self->selected_file, self->selected_rank);
  gchar *to_sq = file_rank_to_algebraic(to_file, to_rank);

  sync_engine_position(self);

  gboolean success = chess_engine_make_move(self->engine, from_sq, to_sq, 0);

  if (success) {
    /* Update game state */
    gchar *uci = g_strdup_printf("%s%s", from_sq, to_sq);

    gint from_idx = gnostr_chess_square_to_index(self->selected_file, self->selected_rank);
    gint to_idx = gnostr_chess_square_to_index(to_file, to_rank);

    /* Handle castling */
    const GnostrChessSquare *moving = gnostr_chess_get_piece_at(self->game,
      self->selected_file, self->selected_rank);

    if (moving->piece == GNOSTR_CHESS_PIECE_KING) {
      gint file_diff = to_file - self->selected_file;
      if (file_diff == 2) {
        /* Kingside castling - move rook */
        gint rook_from = gnostr_chess_square_to_index(7, self->selected_rank);
        gint rook_to = gnostr_chess_square_to_index(5, self->selected_rank);
        self->game->board[rook_to] = self->game->board[rook_from];
        self->game->board[rook_from].piece = GNOSTR_CHESS_PIECE_NONE;
        self->game->board[rook_from].color = GNOSTR_CHESS_COLOR_NONE;
      } else if (file_diff == -2) {
        /* Queenside castling - move rook */
        gint rook_from = gnostr_chess_square_to_index(0, self->selected_rank);
        gint rook_to = gnostr_chess_square_to_index(3, self->selected_rank);
        self->game->board[rook_to] = self->game->board[rook_from];
        self->game->board[rook_from].piece = GNOSTR_CHESS_PIECE_NONE;
        self->game->board[rook_from].color = GNOSTR_CHESS_COLOR_NONE;
      }
    }

    /* Handle en passant */
    if (moving->piece == GNOSTR_CHESS_PIECE_PAWN) {
      gint file_diff = to_file - self->selected_file;
      const GnostrChessSquare *target = gnostr_chess_get_piece_at(self->game, to_file, to_rank);
      if (file_diff != 0 && target->piece == GNOSTR_CHESS_PIECE_NONE) {
        /* En passant capture - remove the captured pawn */
        gint captured_idx = gnostr_chess_square_to_index(to_file, self->selected_rank);
        self->game->board[captured_idx].piece = GNOSTR_CHESS_PIECE_NONE;
        self->game->board[captured_idx].color = GNOSTR_CHESS_COLOR_NONE;
      }
    }

    /* Move the piece */
    self->game->board[to_idx] = self->game->board[from_idx];
    self->game->board[from_idx].piece = GNOSTR_CHESS_PIECE_NONE;
    self->game->board[from_idx].color = GNOSTR_CHESS_COLOR_NONE;

    /* Update last move highlights */
    self->game->last_move_from = from_idx;
    self->game->last_move_to = to_idx;
    self->game->current_ply++;

    /* Generate SAN for the move (simplified) */
    gchar piece_char = gnostr_chess_piece_char(moving->piece);
    gchar *san;
    if (moving->piece == GNOSTR_CHESS_PIECE_PAWN) {
      san = g_strdup(to_sq);
    } else if (moving->piece == GNOSTR_CHESS_PIECE_KING &&
               abs(to_file - self->selected_file) == 2) {
      san = g_strdup(to_file > self->selected_file ? "O-O" : "O-O-O");
    } else {
      san = g_strdup_printf("%c%s", piece_char, to_sq);
    }

    /* Emit move-made signal */
    g_signal_emit(self, signals[SIGNAL_MOVE_MADE], 0, san, uci);

    g_free(san);
    g_free(uci);

    /* Check for game over */
    sync_engine_position(self);
    if (chess_engine_is_checkmate(self->engine)) {
      GnostrChessResult result = (self->game->current_ply % 2 == 0) ?
        GNOSTR_CHESS_RESULT_BLACK_WINS : GNOSTR_CHESS_RESULT_WHITE_WINS;
      g_signal_emit(self, signals[SIGNAL_GAME_OVER], 0, (gint)result);
    } else if (chess_engine_is_stalemate(self->engine)) {
      g_signal_emit(self, signals[SIGNAL_GAME_OVER], 0, (gint)GNOSTR_CHESS_RESULT_DRAW);
    }
  } else {
    g_signal_emit(self, signals[SIGNAL_ILLEGAL_MOVE_ATTEMPTED], 0);
  }

  g_free(from_sq);
  g_free(to_sq);

  clear_selection(self);
  gtk_widget_queue_draw(self->board_drawing);

  return success;
}

/* ============================================================================
 * Click Handler
 * ============================================================================ */

static void on_board_pressed(GtkGestureClick *gesture, gint n_press,
                             double x, double y, gpointer user_data) {
  GnostrChessBoard *self = GNOSTR_CHESS_BOARD(user_data);
  (void)gesture;
  (void)n_press;

  if (!self->is_interactive || !self->game || !self->engine) return;

  gint file, rank;
  coords_to_square(self, x, y, &file, &rank);

  /* Get piece at clicked square */
  const GnostrChessSquare *sq = gnostr_chess_get_piece_at(self->game, file, rank);

  /* Determine whose turn it is */
  GnostrChessColor side_to_move = (self->game->current_ply % 2 == 0) ?
    GNOSTR_CHESS_COLOR_WHITE : GNOSTR_CHESS_COLOR_BLACK;

  /* Check if player can move this color */
  gboolean can_move_color = (self->player_color == GNOSTR_CHESS_COLOR_NONE ||
                             self->player_color == side_to_move);

  if (self->selected_file < 0) {
    /* No piece selected yet */
    if (sq->piece != GNOSTR_CHESS_PIECE_NONE &&
        sq->color == side_to_move &&
        can_move_color) {
      /* Select this piece */
      self->selected_file = file;
      self->selected_rank = rank;
      update_legal_moves(self);

      g_signal_emit(self, signals[SIGNAL_PIECE_SELECTED], 0, file, rank);
      gtk_widget_queue_draw(self->board_drawing);
    }
  } else {
    /* A piece is already selected */
    if (file == self->selected_file && rank == self->selected_rank) {
      /* Clicked same square - deselect */
      clear_selection(self);
    } else if (sq->piece != GNOSTR_CHESS_PIECE_NONE &&
               sq->color == side_to_move &&
               can_move_color) {
      /* Clicked another of our pieces - switch selection */
      self->selected_file = file;
      self->selected_rank = rank;
      update_legal_moves(self);

      g_signal_emit(self, signals[SIGNAL_PIECE_SELECTED], 0, file, rank);
      gtk_widget_queue_draw(self->board_drawing);
    } else {
      /* Attempt to move to this square */
      try_make_move(self, file, rank);
    }
  }
}

/* ============================================================================
 * Drawing
 * ============================================================================ */

static void draw_capture_indicator(cairo_t *cr, double x, double y, double size) {
  /* Draw corner triangles for capture indicator */
  double corner_size = size * 0.25;
  cairo_set_source_rgba(cr, 0, 0, 0, 0.15);

  /* Top-left corner */
  cairo_move_to(cr, x, y);
  cairo_line_to(cr, x + corner_size, y);
  cairo_line_to(cr, x, y + corner_size);
  cairo_close_path(cr);
  cairo_fill(cr);

  /* Top-right corner */
  cairo_move_to(cr, x + size, y);
  cairo_line_to(cr, x + size - corner_size, y);
  cairo_line_to(cr, x + size, y + corner_size);
  cairo_close_path(cr);
  cairo_fill(cr);

  /* Bottom-left corner */
  cairo_move_to(cr, x, y + size);
  cairo_line_to(cr, x + corner_size, y + size);
  cairo_line_to(cr, x, y + size - corner_size);
  cairo_close_path(cr);
  cairo_fill(cr);

  /* Bottom-right corner */
  cairo_move_to(cr, x + size, y + size);
  cairo_line_to(cr, x + size - corner_size, y + size);
  cairo_line_to(cr, x + size, y + size - corner_size);
  cairo_close_path(cr);
  cairo_fill(cr);
}

static void draw_board(GtkDrawingArea *area, cairo_t *cr,
                       int width, int height, gpointer user_data) {
  GnostrChessBoard *self = GNOSTR_CHESS_BOARD(user_data);
  (void)area;

  if (!self->game) return;

  gint square_size = MIN(width, height) / 8;
  gint board_width = square_size * 8;
  gint board_height = square_size * 8;
  gint offset_x = (width - board_width) / 2;
  gint offset_y = (height - board_height) / 2;

  /* Parse colors */
  double light_r, light_g, light_b;
  double dark_r, dark_g, dark_b;

  const gchar *light_color = self->light_square_color ? self->light_square_color : LIGHT_SQUARE_COLOR;
  const gchar *dark_color = self->dark_square_color ? self->dark_square_color : DARK_SQUARE_COLOR;

  parse_hex_color(light_color, &light_r, &light_g, &light_b);
  parse_hex_color(dark_color, &dark_r, &dark_g, &dark_b);

  /* Check if king is in check */
  gint check_file = -1, check_rank = -1;
  if (self->engine) {
    sync_engine_position(self);
    if (chess_engine_is_check(self->engine)) {
      /* Find the king of the side to move */
      GnostrChessColor side_to_move = (self->game->current_ply % 2 == 0) ?
        GNOSTR_CHESS_COLOR_WHITE : GNOSTR_CHESS_COLOR_BLACK;

      for (gint r = 0; r < 8; r++) {
        for (gint f = 0; f < 8; f++) {
          const GnostrChessSquare *sq = gnostr_chess_get_piece_at(self->game, f, r);
          if (sq->piece == GNOSTR_CHESS_PIECE_KING && sq->color == side_to_move) {
            check_file = f;
            check_rank = r;
            break;
          }
        }
        if (check_file >= 0) break;
      }
    }
  }

  /* Draw squares */
  for (gint rank = 0; rank < 8; rank++) {
    for (gint file = 0; file < 8; file++) {
      gint display_file = self->board_flipped ? (7 - file) : file;
      gint display_rank = self->board_flipped ? rank : (7 - rank);

      gint x = offset_x + display_file * square_size;
      gint y = offset_y + display_rank * square_size;

      /* Square base color */
      gboolean is_light = ((file + rank) % 2 == 0);
      if (is_light) {
        cairo_set_source_rgb(cr, light_r, light_g, light_b);
      } else {
        cairo_set_source_rgb(cr, dark_r, dark_g, dark_b);
      }
      cairo_rectangle(cr, x, y, square_size, square_size);
      cairo_fill(cr);

      gint index = gnostr_chess_square_to_index(file, rank);

      /* Last move highlight */
      if (self->game->last_move_from == index) {
        cairo_set_source_rgba(cr, 0.6, 0.78, 0, 0.5);
        cairo_rectangle(cr, x, y, square_size, square_size);
        cairo_fill(cr);
      } else if (self->game->last_move_to == index) {
        cairo_set_source_rgba(cr, 0.6, 0.78, 0, 0.7);
        cairo_rectangle(cr, x, y, square_size, square_size);
        cairo_fill(cr);
      }

      /* Check highlight */
      if (file == check_file && rank == check_rank) {
        cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.4);
        cairo_rectangle(cr, x, y, square_size, square_size);
        cairo_fill(cr);
      }

      /* Selected piece highlight */
      if (self->selected_file == file && self->selected_rank == rank) {
        cairo_set_source_rgba(cr, 0.08, 0.33, 0.12, 0.5);
        cairo_rectangle(cr, x, y, square_size, square_size);
        cairo_fill(cr);
      }

      /* Draw piece */
      const GnostrChessSquare *sq = gnostr_chess_get_piece_at(self->game, file, rank);
      if (sq && sq->piece != GNOSTR_CHESS_PIECE_NONE) {
        const gchar *piece_str = gnostr_chess_piece_unicode(sq->piece, sq->color);

        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, square_size * 0.75);

        cairo_text_extents_t extents;
        cairo_text_extents(cr, piece_str, &extents);

        double text_x = x + (square_size - extents.width) / 2 - extents.x_bearing;
        double text_y = y + (square_size - extents.height) / 2 - extents.y_bearing;

        /* Shadow */
        cairo_set_source_rgba(cr, 0, 0, 0, 0.3);
        cairo_move_to(cr, text_x + 1, text_y + 1);
        cairo_show_text(cr, piece_str);

        /* Piece */
        if (sq->color == GNOSTR_CHESS_COLOR_WHITE) {
          cairo_set_source_rgb(cr, 1, 1, 1);
        } else {
          cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        }
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, piece_str);
      }

      /* Legal move indicators */
      if (self->show_legal_moves && is_legal_move_target(self, file, rank)) {
        if (sq->piece == GNOSTR_CHESS_PIECE_NONE) {
          /* Empty square - draw dot */
          cairo_arc(cr, x + square_size / 2.0, y + square_size / 2.0,
                    square_size * LEGAL_MOVE_DOT_RADIUS, 0, 2 * G_PI);
          cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
          cairo_fill(cr);
        } else {
          /* Capture - draw corner triangles */
          draw_capture_indicator(cr, x, y, square_size);
        }
      }
    }
  }

  /* Draw coordinates if enabled */
  if (self->show_coordinates) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);

    for (gint i = 0; i < 8; i++) {
      /* File labels (a-h) */
      gint display_file = self->board_flipped ? (7 - i) : i;
      char file_char[2] = { (char)('a' + display_file), '\0' };
      cairo_move_to(cr, offset_x + i * square_size + square_size / 2 - 3, offset_y + board_height + 12);
      cairo_show_text(cr, file_char);

      /* Rank labels (1-8) */
      gint display_rank = self->board_flipped ? (i + 1) : (8 - i);
      char rank_char[2];
      g_snprintf(rank_char, sizeof(rank_char), "%d", display_rank);
      cairo_move_to(cr, offset_x - 12, offset_y + i * square_size + square_size / 2 + 4);
      cairo_show_text(cr, rank_char);
    }
  }
}

/* ============================================================================
 * Widget Lifecycle
 * ============================================================================ */

static void gnostr_chess_board_dispose(GObject *object) {
  GnostrChessBoard *self = GNOSTR_CHESS_BOARD(object);

  if (self->promotion_popover) {
    gtk_widget_unparent(self->promotion_popover);
    self->promotion_popover = NULL;
  }

  if (self->legal_move_targets) {
    g_list_free_full(self->legal_move_targets, g_free);
    self->legal_move_targets = NULL;
  }

  /* Unparent drawing area */
  if (self->board_drawing) {
    gtk_widget_unparent(self->board_drawing);
    self->board_drawing = NULL;
  }

  G_OBJECT_CLASS(gnostr_chess_board_parent_class)->dispose(object);
}

static void gnostr_chess_board_finalize(GObject *object) {
  GnostrChessBoard *self = GNOSTR_CHESS_BOARD(object);

  if (self->engine) {
    chess_engine_free(self->engine);
    self->engine = NULL;
  }

  gnostr_chess_game_free(self->game);
  g_free(self->light_square_color);
  g_free(self->dark_square_color);

  G_OBJECT_CLASS(gnostr_chess_board_parent_class)->finalize(object);
}

static void gnostr_chess_board_class_init(GnostrChessBoardClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = gnostr_chess_board_dispose;
  object_class->finalize = gnostr_chess_board_finalize;

  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);

  /* Signals */
  signals[SIGNAL_PIECE_SELECTED] = g_signal_new("piece-selected",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);

  signals[SIGNAL_PIECE_DESELECTED] = g_signal_new("piece-deselected",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_MOVE_MADE] = g_signal_new("move-made",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

  signals[SIGNAL_ILLEGAL_MOVE_ATTEMPTED] = g_signal_new("illegal-move-attempted",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);

  signals[SIGNAL_PROMOTION_REQUIRED] = g_signal_new("promotion-required",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);

  signals[SIGNAL_GAME_OVER] = g_signal_new("game-over",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT);
}

static void gnostr_chess_board_init(GnostrChessBoard *self) {
  /* Initialize state */
  self->board_size = DEFAULT_BOARD_SIZE;
  self->board_flipped = FALSE;
  self->show_coordinates = TRUE;
  self->show_legal_moves = TRUE;
  self->animate_moves = TRUE;
  self->is_interactive = FALSE;
  self->player_color = GNOSTR_CHESS_COLOR_NONE;
  self->selected_file = -1;
  self->selected_rank = -1;

  /* Create drawing area */
  self->board_drawing = gtk_drawing_area_new();
  gtk_widget_set_size_request(self->board_drawing, self->board_size, self->board_size);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->board_drawing),
                                  draw_board, self, NULL);
  gtk_widget_set_parent(self->board_drawing, GTK_WIDGET(self));

  /* Set up click gesture */
  self->click_gesture = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(self->click_gesture), GDK_BUTTON_PRIMARY);
  g_signal_connect(self->click_gesture, "pressed", G_CALLBACK(on_board_pressed), self);
  gtk_widget_add_controller(self->board_drawing, GTK_EVENT_CONTROLLER(self->click_gesture));

  /* Create chess engine */
  self->engine = chess_engine_new();

  /* Create default game with starting position */
  self->game = gnostr_chess_game_new();
  gnostr_chess_setup_initial_position(self->game->board);
  self->game->last_move_from = -1;
  self->game->last_move_to = -1;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

GnostrChessBoard *gnostr_chess_board_new(void) {
  return g_object_new(GNOSTR_TYPE_CHESS_BOARD, NULL);
}

void gnostr_chess_board_set_game(GnostrChessBoard *self, GnostrChessGame *game) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));

  /* Clear selection */
  clear_selection(self);

  /* Free old game if we own it */
  if (self->game) {
    gnostr_chess_game_free(self->game);
  }

  /* Copy the game (we take ownership) */
  if (game) {
    self->game = gnostr_chess_game_new();
    /* Copy board state */
    memcpy(self->game->board, game->board, sizeof(self->game->board));
    self->game->current_ply = game->current_ply;
    self->game->moves_count = game->moves_count;
    self->game->last_move_from = game->last_move_from;
    self->game->last_move_to = game->last_move_to;
  } else {
    self->game = gnostr_chess_game_new();
    gnostr_chess_setup_initial_position(self->game->board);
    self->game->last_move_from = -1;
    self->game->last_move_to = -1;
  }

  sync_engine_position(self);
  gtk_widget_queue_draw(self->board_drawing);
}

gboolean gnostr_chess_board_set_fen(GnostrChessBoard *self, const gchar *fen) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);
  g_return_val_if_fail(fen != NULL, FALSE);

  clear_selection(self);

  if (!self->engine) return FALSE;

  if (!chess_engine_set_fen(self->engine, fen)) {
    return FALSE;
  }

  /* Parse FEN into our game state */
  if (!self->game) {
    self->game = gnostr_chess_game_new();
  }

  /* Clear board */
  memset(self->game->board, 0, sizeof(self->game->board));

  /* Parse piece placement from FEN */
  gint file = 0, rank = 7;
  for (const gchar *p = fen; *p && *p != ' '; p++) {
    if (*p == '/') {
      file = 0;
      rank--;
    } else if (g_ascii_isdigit(*p)) {
      file += *p - '0';
    } else {
      gint idx = gnostr_chess_square_to_index(file, rank);
      GnostrChessSquare *sq = &self->game->board[idx];

      sq->color = g_ascii_isupper(*p) ? GNOSTR_CHESS_COLOR_WHITE : GNOSTR_CHESS_COLOR_BLACK;

      switch (g_ascii_tolower(*p)) {
        case 'p': sq->piece = GNOSTR_CHESS_PIECE_PAWN; break;
        case 'n': sq->piece = GNOSTR_CHESS_PIECE_KNIGHT; break;
        case 'b': sq->piece = GNOSTR_CHESS_PIECE_BISHOP; break;
        case 'r': sq->piece = GNOSTR_CHESS_PIECE_ROOK; break;
        case 'q': sq->piece = GNOSTR_CHESS_PIECE_QUEEN; break;
        case 'k': sq->piece = GNOSTR_CHESS_PIECE_KING; break;
        default: break;
      }
      file++;
    }
  }

  /* Parse side to move */
  const gchar *space = strchr(fen, ' ');
  if (space && *(space + 1) == 'b') {
    self->game->current_ply = 1;
  } else {
    self->game->current_ply = 0;
  }

  self->game->last_move_from = -1;
  self->game->last_move_to = -1;

  gtk_widget_queue_draw(self->board_drawing);
  return TRUE;
}

void gnostr_chess_board_reset(GnostrChessBoard *self) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));

  clear_selection(self);

  if (!self->game) {
    self->game = gnostr_chess_game_new();
  }

  gnostr_chess_setup_initial_position(self->game->board);
  self->game->current_ply = 0;
  self->game->moves_count = 0;
  self->game->last_move_from = -1;
  self->game->last_move_to = -1;

  if (self->engine) {
    chess_engine_reset(self->engine);
  }

  gtk_widget_queue_draw(self->board_drawing);
}

gchar *gnostr_chess_board_get_fen(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), NULL);

  if (!self->engine) return NULL;

  sync_engine_position(self);
  return chess_engine_get_fen(self->engine);
}

void gnostr_chess_board_set_interactive(GnostrChessBoard *self, gboolean interactive) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));
  self->is_interactive = interactive;

  if (!interactive) {
    clear_selection(self);
  }
}

gboolean gnostr_chess_board_is_interactive(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);
  return self->is_interactive;
}

void gnostr_chess_board_set_player_color(GnostrChessBoard *self, GnostrChessColor color) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));
  self->player_color = color;
}

GnostrChessColor gnostr_chess_board_get_player_color(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), GNOSTR_CHESS_COLOR_NONE);
  return self->player_color;
}

gboolean gnostr_chess_board_make_move(GnostrChessBoard *self,
                                       gint from_file, gint from_rank,
                                       gint to_file, gint to_rank,
                                       gchar promotion) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);
  g_return_val_if_fail(self->engine != NULL, FALSE);
  g_return_val_if_fail(self->game != NULL, FALSE);

  gchar *from_sq = file_rank_to_algebraic(from_file, from_rank);
  gchar *to_sq = file_rank_to_algebraic(to_file, to_rank);

  sync_engine_position(self);

  gboolean success = chess_engine_make_move(self->engine, from_sq, to_sq, promotion);

  if (success) {
    gint from_idx = gnostr_chess_square_to_index(from_file, from_rank);
    gint to_idx = gnostr_chess_square_to_index(to_file, to_rank);

    /* Move the piece */
    self->game->board[to_idx] = self->game->board[from_idx];
    self->game->board[from_idx].piece = GNOSTR_CHESS_PIECE_NONE;
    self->game->board[from_idx].color = GNOSTR_CHESS_COLOR_NONE;

    /* Handle promotion */
    if (promotion) {
      switch (promotion) {
        case 'q': self->game->board[to_idx].piece = GNOSTR_CHESS_PIECE_QUEEN; break;
        case 'r': self->game->board[to_idx].piece = GNOSTR_CHESS_PIECE_ROOK; break;
        case 'b': self->game->board[to_idx].piece = GNOSTR_CHESS_PIECE_BISHOP; break;
        case 'n': self->game->board[to_idx].piece = GNOSTR_CHESS_PIECE_KNIGHT; break;
        default: break;
      }
    }

    self->game->last_move_from = from_idx;
    self->game->last_move_to = to_idx;
    self->game->current_ply++;

    gchar *uci = promotion ?
      g_strdup_printf("%s%s%c", from_sq, to_sq, promotion) :
      g_strdup_printf("%s%s", from_sq, to_sq);

    g_signal_emit(self, signals[SIGNAL_MOVE_MADE], 0, uci, uci);
    g_free(uci);

    gtk_widget_queue_draw(self->board_drawing);
  }

  g_free(from_sq);
  g_free(to_sq);

  return success;
}

gboolean gnostr_chess_board_make_move_san(GnostrChessBoard *self, const gchar *san) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);
  g_return_val_if_fail(san != NULL, FALSE);
  g_return_val_if_fail(self->engine != NULL, FALSE);

  sync_engine_position(self);
  return chess_engine_make_move_san(self->engine, san);
}

gboolean gnostr_chess_board_make_move_uci(GnostrChessBoard *self, const gchar *uci) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);
  g_return_val_if_fail(uci != NULL, FALSE);
  g_return_val_if_fail(strlen(uci) >= 4, FALSE);

  gint from_file = uci[0] - 'a';
  gint from_rank = uci[1] - '1';
  gint to_file = uci[2] - 'a';
  gint to_rank = uci[3] - '1';
  gchar promotion = (strlen(uci) >= 5) ? uci[4] : 0;

  return gnostr_chess_board_make_move(self, from_file, from_rank, to_file, to_rank, promotion);
}

gboolean gnostr_chess_board_undo_move(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);

  if (!self->game || self->game->current_ply <= 0) return FALSE;

  /* For now, just reset and replay to previous position */
  /* A more sophisticated implementation would track move history */
  return FALSE;  /* TODO: Implement proper undo */
}

gboolean gnostr_chess_board_get_selected_square(GnostrChessBoard *self,
                                                  gint *out_file,
                                                  gint *out_rank) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);

  if (self->selected_file < 0) return FALSE;

  if (out_file) *out_file = self->selected_file;
  if (out_rank) *out_rank = self->selected_rank;

  return TRUE;
}

void gnostr_chess_board_clear_selection(GnostrChessBoard *self) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));
  clear_selection(self);
}

GnostrChessColor gnostr_chess_board_get_side_to_move(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), GNOSTR_CHESS_COLOR_WHITE);

  if (!self->game) return GNOSTR_CHESS_COLOR_WHITE;

  return (self->game->current_ply % 2 == 0) ?
    GNOSTR_CHESS_COLOR_WHITE : GNOSTR_CHESS_COLOR_BLACK;
}

gboolean gnostr_chess_board_is_check(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);

  if (!self->engine) return FALSE;

  sync_engine_position(self);
  return chess_engine_is_check(self->engine);
}

gboolean gnostr_chess_board_is_checkmate(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);

  if (!self->engine) return FALSE;

  sync_engine_position(self);
  return chess_engine_is_checkmate(self->engine);
}

gboolean gnostr_chess_board_is_stalemate(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);

  if (!self->engine) return FALSE;

  sync_engine_position(self);
  return chess_engine_is_stalemate(self->engine);
}

gboolean gnostr_chess_board_is_game_over(GnostrChessBoard *self) {
  return gnostr_chess_board_is_checkmate(self) || gnostr_chess_board_is_stalemate(self);
}

GnostrChessResult gnostr_chess_board_get_result(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), GNOSTR_CHESS_RESULT_UNKNOWN);

  if (gnostr_chess_board_is_checkmate(self)) {
    return (self->game->current_ply % 2 == 0) ?
      GNOSTR_CHESS_RESULT_BLACK_WINS : GNOSTR_CHESS_RESULT_WHITE_WINS;
  }

  if (gnostr_chess_board_is_stalemate(self)) {
    return GNOSTR_CHESS_RESULT_DRAW;
  }

  return GNOSTR_CHESS_RESULT_UNKNOWN;
}

void gnostr_chess_board_set_size(GnostrChessBoard *self, gint size) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));

  self->board_size = CLAMP(size, MIN_BOARD_SIZE, MAX_BOARD_SIZE);
  gtk_widget_set_size_request(self->board_drawing, self->board_size, self->board_size);
  gtk_widget_queue_draw(self->board_drawing);
}

gint gnostr_chess_board_get_size(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), DEFAULT_BOARD_SIZE);
  return self->board_size;
}

void gnostr_chess_board_set_flipped(GnostrChessBoard *self, gboolean flipped) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));

  self->board_flipped = flipped;
  gtk_widget_queue_draw(self->board_drawing);
}

gboolean gnostr_chess_board_is_flipped(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);
  return self->board_flipped;
}

void gnostr_chess_board_set_show_coordinates(GnostrChessBoard *self, gboolean show) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));

  self->show_coordinates = show;
  gtk_widget_queue_draw(self->board_drawing);
}

gboolean gnostr_chess_board_get_show_coordinates(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), TRUE);
  return self->show_coordinates;
}

void gnostr_chess_board_set_show_legal_moves(GnostrChessBoard *self, gboolean show) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));

  self->show_legal_moves = show;
  gtk_widget_queue_draw(self->board_drawing);
}

gboolean gnostr_chess_board_get_show_legal_moves(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), TRUE);
  return self->show_legal_moves;
}

void gnostr_chess_board_set_animate_moves(GnostrChessBoard *self, gboolean animate) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));
  self->animate_moves = animate;
}

gboolean gnostr_chess_board_get_animate_moves(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), TRUE);
  return self->animate_moves;
}

void gnostr_chess_board_set_square_colors(GnostrChessBoard *self,
                                           const gchar *light,
                                           const gchar *dark) {
  g_return_if_fail(GNOSTR_IS_CHESS_BOARD(self));

  g_free(self->light_square_color);
  g_free(self->dark_square_color);

  self->light_square_color = g_strdup(light);
  self->dark_square_color = g_strdup(dark);

  gtk_widget_queue_draw(self->board_drawing);
}

gboolean gnostr_chess_board_go_to_ply(GnostrChessBoard *self, gint ply) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), FALSE);

  if (!self->game) return FALSE;

  return gnostr_chess_game_set_position(self->game, ply);
}

gint gnostr_chess_board_get_current_ply(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), 0);

  if (!self->game) return 0;
  return self->game->current_ply;
}

gint gnostr_chess_board_get_total_plies(GnostrChessBoard *self) {
  g_return_val_if_fail(GNOSTR_IS_CHESS_BOARD(self), 0);

  if (!self->game) return 0;
  return (gint)self->game->moves_count;
}
