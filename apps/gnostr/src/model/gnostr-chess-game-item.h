/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-chess-game-item.h - GObject wrapper for GnostrChessGame
 *
 * Lightweight wrapper that holds a non-owning reference to a
 * GnostrChessGame struct, suitable for use in GListStore.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GNOSTR_CHESS_GAME_ITEM_H
#define GNOSTR_CHESS_GAME_ITEM_H

#include <glib-object.h>
#include "../util/nip64_chess.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHESS_GAME_ITEM (gnostr_chess_game_item_get_type())

G_DECLARE_FINAL_TYPE(GnostrChessGameItem, gnostr_chess_game_item,
                     GNOSTR, CHESS_GAME_ITEM, GObject)

/**
 * gnostr_chess_game_item_new:
 * @game: (transfer none): The chess game (not owned).
 *
 * Creates a new item wrapping the given game pointer.
 * The caller must ensure the game outlives this item.
 *
 * Returns: (transfer full): A new #GnostrChessGameItem.
 */
GnostrChessGameItem *gnostr_chess_game_item_new(GnostrChessGame *game);

/**
 * gnostr_chess_game_item_get_game:
 * @self: The item.
 *
 * Returns: (transfer none): The wrapped game pointer.
 */
GnostrChessGame *gnostr_chess_game_item_get_game(GnostrChessGameItem *self);

/**
 * gnostr_chess_game_item_get_event_id:
 * @self: The item.
 *
 * Returns: (transfer none) (nullable): The event ID string.
 */
const char *gnostr_chess_game_item_get_event_id(GnostrChessGameItem *self);

/**
 * gnostr_chess_game_item_get_created_at:
 * @self: The item.
 *
 * Returns: The event creation timestamp.
 */
gint64 gnostr_chess_game_item_get_created_at(GnostrChessGameItem *self);

G_END_DECLS

#endif /* GNOSTR_CHESS_GAME_ITEM_H */
