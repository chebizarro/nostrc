# GnostrChessBoard Interactive Widget Design

**Issue**: nostrc-9ghs
**Author**: Brett (GTK4 UI Developer)
**Status**: BLOCKED - Awaiting chess engine port
**Date**: 2026-02-06

## Overview

This document describes the design for `GnostrChessBoard`, an interactive GTK4 widget that extends the current `GnostrChessCard` functionality to support live chess play. The widget will allow users to make moves by clicking on squares, with full legal move validation once the chess engine is integrated.

## Current State Analysis

### gnostr-chess-card.c (Existing)

The current implementation provides:

1. **Board Drawing** (lines 256-357):
   - Cairo-based rendering in `draw_board()` function
   - 8x8 grid with light/dark square colors (`#f0d9b5` / `#b58863`)
   - Piece rendering using Unicode chess symbols via `gnostr_chess_piece_unicode()`
   - Last move highlighting with semi-transparent green overlay
   - File/rank labels (a-h, 1-8)
   - Board flipping support via `board_flipped` flag

2. **Navigation Controls** (lines 537-577):
   - First/prev/next/last buttons
   - Autoplay with configurable interval
   - Flip board button

3. **Widget Signals** (lines 648-670):
   - `open-profile`, `open-game`, `share-game`
   - `copy-pgn`, `zap-requested`, `bookmark-toggled`

### nip64_chess.h/c (Game State)

The chess library provides:

1. **Data Structures**:
   - `GnostrChessGame`: Full game state with board array, moves, metadata
   - `GnostrChessSquare`: Piece and color per square
   - `GnostrChessMove`: SAN notation, from/to squares, flags

2. **Board Utilities**:
   - `gnostr_chess_square_to_index()`: Convert file/rank to 0-63 index
   - `gnostr_chess_index_to_file/rank()`: Convert back
   - `gnostr_chess_get_piece_at()`: Query current position

3. **Navigation**:
   - `gnostr_chess_game_set_position()`: Jump to any ply
   - `gnostr_chess_game_next/prev()`: Step through moves

**Missing**: Legal move generation - this requires the chess engine port.

---

## Interactive Board Design

### 1. Click Handling Architecture

```c
/* GtkGestureClick setup in gnostr_chess_board_init() */
GtkGesture *click = gtk_gesture_click_new();
gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);  /* All buttons */
g_signal_connect(click, "pressed", G_CALLBACK(on_board_pressed), self);
g_signal_connect(click, "released", G_CALLBACK(on_board_released), self);
gtk_widget_add_controller(self->board_drawing, GTK_EVENT_CONTROLLER(click));
```

**Click-to-Move Flow**:
```
User clicks square A
  |
  v
on_board_pressed():
  - Convert (x, y) to (file, rank) using square_size
  - Account for board_flipped orientation
  - If no piece selected:
      - Check if square has our piece (based on side_to_move)
      - If yes: set selected_square, highlight legal moves
  - If piece already selected:
      - If clicking same square: deselect
      - If clicking legal move target: execute move
      - If clicking another of our pieces: switch selection
  |
  v
gtk_widget_queue_draw() -> redraw with highlights
```

**Coordinate Conversion**:
```c
static void coords_to_square(GnostrChessBoard *self,
                             double x, double y,
                             gint *out_file, gint *out_rank) {
    gint square_size = self->board_size / 8;
    gint offset_x = (widget_width - self->board_size) / 2;
    gint offset_y = (widget_height - self->board_size) / 2;

    gint file = (x - offset_x) / square_size;
    gint rank = 7 - ((y - offset_y) / square_size);

    if (self->board_flipped) {
        file = 7 - file;
        rank = 7 - rank;
    }

    *out_file = CLAMP(file, 0, 7);
    *out_rank = CLAMP(rank, 0, 7);
}
```

### 2. Visual Feedback - Highlighting

**Selected Piece Highlight**:
```c
#define SELECTED_SQUARE_COLOR "rgba(20, 85, 30, 0.5)"  /* Dark green */
```

**Legal Move Indicators**:
```c
/* For empty squares - small centered dot */
#define LEGAL_MOVE_DOT_COLOR "rgba(0, 0, 0, 0.15)"
#define LEGAL_MOVE_DOT_RADIUS 0.15  /* Fraction of square size */

/* For capture squares - corner triangles or ring */
#define LEGAL_CAPTURE_COLOR "rgba(0, 0, 0, 0.15)"
```

**Drawing Implementation** (extend `draw_board()`):
```c
/* After drawing piece, check for highlights */
if (self->selected_file == file && self->selected_rank == rank) {
    cairo_set_source_rgba(cr, 0.08, 0.33, 0.12, 0.5);
    cairo_rectangle(cr, x, y, square_size, square_size);
    cairo_fill(cr);
}

/* Legal move dots */
if (is_legal_move_target(self, file, rank)) {
    gint sq_idx = gnostr_chess_square_to_index(file, rank);
    const GnostrChessSquare *sq = &self->game->board[sq_idx];

    if (sq->piece == GNOSTR_CHESS_PIECE_NONE) {
        /* Empty square - draw dot */
        cairo_arc(cr, x + square_size/2, y + square_size/2,
                  square_size * 0.15, 0, 2 * G_PI);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
        cairo_fill(cr);
    } else {
        /* Capture - draw corner triangles */
        draw_capture_indicator(cr, x, y, square_size);
    }
}
```

### 3. Promotion Dialog

When a pawn reaches the 8th (or 1st for black) rank:

```c
static void show_promotion_dialog(GnostrChessBoard *self,
                                   gint from_file, gint from_rank,
                                   gint to_file, gint to_rank) {
    GtkWidget *dialog = gtk_popover_new();
    gtk_widget_set_parent(dialog, GTK_WIDGET(self));

    /* Position above/below the promotion square */
    GdkRectangle rect = get_square_rect(self, to_file, to_rank);
    gtk_popover_set_pointing_to(GTK_POPOVER(dialog), &rect);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    /* Add piece buttons: Q, R, B, N */
    const gchar *pieces[] = { "Q", "R", "B", "N" };
    const gchar *icons[] = {
        "\xe2\x99\x95", "\xe2\x99\x96",
        "\xe2\x99\x97", "\xe2\x99\x98"
    };

    for (int i = 0; i < 4; i++) {
        GtkWidget *btn = gtk_button_new_with_label(icons[i]);
        g_object_set_data(G_OBJECT(btn), "piece", (gpointer)pieces[i]);
        g_signal_connect(btn, "clicked",
                        G_CALLBACK(on_promotion_selected), self);
        gtk_box_append(GTK_BOX(box), btn);
    }

    gtk_popover_set_child(GTK_POPOVER(dialog), box);

    /* Store pending move */
    self->pending_promotion_from = gnostr_chess_square_to_index(from_file, from_rank);
    self->pending_promotion_to = gnostr_chess_square_to_index(to_file, to_rank);

    gtk_popover_popup(GTK_POPOVER(dialog));
}
```

### 4. Signal Design

```c
enum {
    /* Existing signals from ChessCard */
    SIGNAL_OPEN_PROFILE,
    SIGNAL_OPEN_GAME,
    SIGNAL_SHARE_GAME,
    SIGNAL_COPY_PGN,
    SIGNAL_ZAP_REQUESTED,
    SIGNAL_BOOKMARK_TOGGLED,

    /* New interactive signals */
    SIGNAL_PIECE_SELECTED,      /* (gint file, gint rank, GnostrChessPiece piece) */
    SIGNAL_PIECE_DESELECTED,    /* () */
    SIGNAL_MOVE_MADE,           /* (const gchar *san, const gchar *uci) */
    SIGNAL_MOVE_INVALID,        /* (const gchar *attempted_san) */
    SIGNAL_PROMOTION_REQUIRED,  /* (gint from_sq, gint to_sq) */
    SIGNAL_GAME_OVER,           /* (GnostrChessResult result) */

    N_SIGNALS
};

/* Signal definitions */
signals[SIGNAL_PIECE_SELECTED] = g_signal_new("piece-selected",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);

signals[SIGNAL_MOVE_MADE] = g_signal_new("move-made",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
```

### 5. Chess Engine Integration Points

The chess engine (once ported) will provide:

```c
/* Expected chess_engine.h API */

/* Legal move generation */
typedef struct {
    gint from;          /* 0-63 square index */
    gint to;            /* 0-63 square index */
    gchar promotion;    /* 'q', 'r', 'b', 'n' or '\0' */
    gboolean is_capture;
    gboolean is_castle;
    gboolean is_en_passant;
} ChessMove;

gboolean chess_engine_init(void);
void chess_engine_cleanup(void);

/* Set position from FEN or board array */
void chess_engine_set_position(const gchar *fen);
void chess_engine_set_position_from_board(const GnostrChessSquare board[64],
                                          GnostrChessColor side_to_move);

/* Get legal moves */
ChessMove *chess_engine_get_legal_moves(gsize *out_count);
gboolean chess_engine_is_legal_move(gint from, gint to, gchar promotion);

/* Make a move (returns SAN notation or NULL if illegal) */
gchar *chess_engine_make_move(gint from, gint to, gchar promotion);

/* Game state queries */
gboolean chess_engine_is_check(void);
gboolean chess_engine_is_checkmate(void);
gboolean chess_engine_is_stalemate(void);
gboolean chess_engine_is_insufficient_material(void);
```

**Integration in GnostrChessBoard**:
```c
static void update_legal_moves(GnostrChessBoard *self) {
    /* Clear previous legal moves */
    if (self->legal_moves) {
        g_free(self->legal_moves);
        self->legal_moves = NULL;
        self->legal_moves_count = 0;
    }

    if (self->selected_file < 0) return;

    /* Set engine position from current game state */
    gchar *fen = gnostr_chess_game_to_fen(self->game);
    chess_engine_set_position(fen);
    g_free(fen);

    /* Get all legal moves */
    gsize all_count;
    ChessMove *all_moves = chess_engine_get_legal_moves(&all_count);

    /* Filter to moves from selected square */
    gint from_sq = gnostr_chess_square_to_index(self->selected_file,
                                                 self->selected_rank);
    GPtrArray *filtered = g_ptr_array_new();

    for (gsize i = 0; i < all_count; i++) {
        if (all_moves[i].from == from_sq) {
            ChessMove *m = g_new(ChessMove, 1);
            *m = all_moves[i];
            g_ptr_array_add(filtered, m);
        }
    }

    self->legal_moves = (ChessMove **)g_ptr_array_free(filtered, FALSE);
    self->legal_moves_count = filtered->len;

    g_free(all_moves);
}
```

---

## State Machine

```
                    IDLE
                      |
                      | click on own piece
                      v
             PIECE_SELECTED
              /    |    \
    click     click      click on
    same      legal      other own
    square    target     piece
      |         |           |
      v         v           |
    IDLE   MOVE_MADE -------+
              |
       is_promotion?
         /       \
       no        yes
        |         |
        v         v
      IDLE   AWAITING_PROMOTION
                  |
            piece selected
                  |
                  v
             MOVE_MADE
                  |
                  v
                IDLE
```

---

## GnostrChessBoard Widget Structure

```c
struct _GnostrChessBoard {
    GtkWidget parent_instance;

    /* Drawing */
    GtkWidget *board_drawing;
    gint board_size;
    gboolean board_flipped;

    /* Game state */
    GnostrChessGame *game;
    GnostrChessColor side_to_move;
    gboolean is_interactive;   /* FALSE for replay mode */

    /* Selection state */
    gint selected_file;        /* -1 if none */
    gint selected_rank;

    /* Legal moves (from engine) */
    ChessMove **legal_moves;
    gsize legal_moves_count;

    /* Pending promotion */
    gint pending_promotion_from;
    gint pending_promotion_to;
    GtkWidget *promotion_popover;

    /* Drag state (future enhancement) */
    gboolean is_dragging;
    gint drag_start_file;
    gint drag_start_rank;
    double drag_x;
    double drag_y;

    /* Appearance */
    gchar *light_square_color;
    gchar *dark_square_color;
    gchar *selected_color;
    gchar *last_move_color;
    gboolean show_coordinates;
    gboolean animate_moves;
};
```

---

## Implementation Priority

1. **Phase 1 (Pre-Engine)** - This document + header scaffold
   - Design document (this file)
   - `gnostr-chess-board.h` header with API

2. **Phase 2 (Engine Integration)**
   - Port chess engine to C
   - Implement `chess_engine_get_legal_moves()`
   - Wire up to GnostrChessBoard

3. **Phase 3 (Interactive Play)**
   - Click handling and selection
   - Legal move highlighting
   - Move execution

4. **Phase 4 (Polish)**
   - Promotion dialog
   - Drag-and-drop support
   - Move animation
   - Sound effects

---

## Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `gnostr-chess-board.h` | CREATE | Widget header with public API |
| `gnostr-chess-board.c` | CREATE (Phase 2) | Full implementation |
| `chess_engine.h` | CREATE (Blocked) | Engine API header |
| `chess_engine.c` | CREATE (Blocked) | Engine implementation |
| `nip64_chess.h` | MODIFY | Add `gnostr_chess_game_to_fen()` |
| `CMakeLists.txt` | MODIFY | Add new source files |

---

## Testing Plan

1. **Unit Tests**:
   - Coordinate conversion (click -> square)
   - Legal move filtering
   - Promotion detection

2. **Integration Tests**:
   - Full game playthrough
   - Edge cases: castling, en passant, promotion
   - Undo/redo support

3. **Visual Tests**:
   - Highlight colors visible on both square colors
   - Promotion dialog positioning
   - Board flipping with selection preserved

---

## Notes

- The current `gnostr-chess-card.c` is 1177 lines. Consider whether to:
  - Extract `GnostrChessBoard` as a separate widget embedded in `GnostrChessCard`
  - OR extend `GnostrChessCard` directly (more coupling but less duplication)

- Recommendation: Create `GnostrChessBoard` as a standalone widget, then use it in `GnostrChessCard`. This allows reuse in other contexts (e.g., standalone analysis board).
