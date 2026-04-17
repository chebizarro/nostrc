/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-chess-game-item.c - GObject wrapper for GnostrChessGame
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gnostr-chess-game-item.h"

struct _GnostrChessGameItem {
    GObject parent_instance;
    GnostrChessGame *game; /* non-owning reference */
};

G_DEFINE_FINAL_TYPE(GnostrChessGameItem, gnostr_chess_game_item, G_TYPE_OBJECT)

static void
gnostr_chess_game_item_class_init(GnostrChessGameItemClass *klass)
{
    (void)klass;
}

static void
gnostr_chess_game_item_init(GnostrChessGameItem *self)
{
    self->game = NULL;
}

GnostrChessGameItem *
gnostr_chess_game_item_new(GnostrChessGame *game)
{
    GnostrChessGameItem *self = g_object_new(GNOSTR_TYPE_CHESS_GAME_ITEM, NULL);
    self->game = game;
    return self;
}

GnostrChessGame *
gnostr_chess_game_item_get_game(GnostrChessGameItem *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_GAME_ITEM(self), NULL);
    return self->game;
}

const char *
gnostr_chess_game_item_get_event_id(GnostrChessGameItem *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_GAME_ITEM(self), NULL);
    return self->game ? self->game->event_id : NULL;
}

gint64
gnostr_chess_game_item_get_created_at(GnostrChessGameItem *self)
{
    g_return_val_if_fail(GNOSTR_IS_CHESS_GAME_ITEM(self), 0);
    return self->game ? self->game->created_at : 0;
}
