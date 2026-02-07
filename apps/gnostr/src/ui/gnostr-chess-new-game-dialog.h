/**
 * GnostrChessNewGameDialog - New Chess Game Configuration Dialog
 *
 * A dialog for configuring and starting a new chess game.
 * Allows selection of player color and AI difficulty level.
 */

#ifndef GNOSTR_CHESS_NEW_GAME_DIALOG_H
#define GNOSTR_CHESS_NEW_GAME_DIALOG_H

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHESS_NEW_GAME_DIALOG (gnostr_chess_new_game_dialog_get_type())

G_DECLARE_FINAL_TYPE(GnostrChessNewGameDialog, gnostr_chess_new_game_dialog, GNOSTR, CHESS_NEW_GAME_DIALOG, AdwDialog)

/**
 * GnostrChessPlayerColor:
 * @GNOSTR_CHESS_COLOR_WHITE: Play as white
 * @GNOSTR_CHESS_COLOR_BLACK: Play as black
 * @GNOSTR_CHESS_COLOR_RANDOM: Randomly assign color
 *
 * Player color selection options.
 */
typedef enum {
    GNOSTR_CHESS_NEW_GAME_COLOR_WHITE = 0,
    GNOSTR_CHESS_NEW_GAME_COLOR_BLACK,
    GNOSTR_CHESS_NEW_GAME_COLOR_RANDOM
} GnostrChessNewGameColor;

/**
 * GnostrChessAIDifficulty:
 * AI difficulty levels mapped to search depth.
 */
typedef enum {
    GNOSTR_CHESS_AI_BEGINNER = 2,
    GNOSTR_CHESS_AI_INTERMEDIATE = 4,
    GNOSTR_CHESS_AI_ADVANCED = 6,
    GNOSTR_CHESS_AI_EXPERT = 8
} GnostrChessAIDifficulty;

/**
 * gnostr_chess_new_game_dialog_new:
 *
 * Creates a new chess game configuration dialog.
 *
 * Returns: (transfer full): A new #GnostrChessNewGameDialog
 */
GnostrChessNewGameDialog *gnostr_chess_new_game_dialog_new(void);

/**
 * gnostr_chess_new_game_dialog_present:
 * @self: The dialog
 * @parent: (nullable): Parent widget
 *
 * Presents the dialog to the user.
 */
void gnostr_chess_new_game_dialog_present(GnostrChessNewGameDialog *self, GtkWidget *parent);

/**
 * gnostr_chess_new_game_dialog_get_player_color:
 * @self: The dialog
 *
 * Gets the selected player color.
 *
 * Returns: The selected #GnostrChessNewGameColor
 */
GnostrChessNewGameColor gnostr_chess_new_game_dialog_get_player_color(GnostrChessNewGameDialog *self);

/**
 * gnostr_chess_new_game_dialog_get_ai_depth:
 * @self: The dialog
 *
 * Gets the selected AI search depth based on difficulty.
 *
 * Returns: AI search depth (2, 4, 6, or 8)
 */
gint gnostr_chess_new_game_dialog_get_ai_depth(GnostrChessNewGameDialog *self);

/**
 * gnostr_chess_new_game_dialog_get_ai_difficulty_label:
 * @depth: AI search depth
 *
 * Gets a human-readable label for the AI difficulty.
 *
 * Returns: (transfer none): Difficulty label string
 */
const gchar *gnostr_chess_new_game_dialog_get_ai_difficulty_label(gint depth);

/**
 * Signals:
 * - "game-started": Emitted when user clicks Start Game
 *   Callback signature: void callback(GnostrChessNewGameDialog *dialog,
 *                                      gint player_color,
 *                                      gint ai_depth,
 *                                      gpointer user_data)
 */

G_END_DECLS

#endif /* GNOSTR_CHESS_NEW_GAME_DIALOG_H */
