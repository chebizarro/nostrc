/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-chess-games-browser.h - Chess Games Browser Widget
 *
 * Displays a list of chess games from Nostr relays (NIP-64).
 * Allows users to browse and select games for viewing.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GNOSTR_CHESS_GAMES_BROWSER_H
#define GNOSTR_CHESS_GAMES_BROWSER_H

#include <gtk/gtk.h>
#include "../util/nip64_chess.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CHESS_GAMES_BROWSER (gnostr_chess_games_browser_get_type())

G_DECLARE_FINAL_TYPE(GnostrChessGamesBrowser, gnostr_chess_games_browser,
                     GNOSTR, CHESS_GAMES_BROWSER, GtkWidget)

/**
 * gnostr_chess_games_browser_new:
 *
 * Create a new chess games browser widget.
 *
 * Returns: (transfer full): A new #GnostrChessGamesBrowser
 */
GnostrChessGamesBrowser *gnostr_chess_games_browser_new(void);

/**
 * gnostr_chess_games_browser_set_games:
 * @self: The browser
 * @games: (transfer none): Hash table of event_id -> GnostrChessGame*
 *
 * Set the games to display. The browser will show cards for each game.
 */
void gnostr_chess_games_browser_set_games(GnostrChessGamesBrowser *self,
                                           GHashTable *games);

/**
 * gnostr_chess_games_browser_refresh:
 * @self: The browser
 *
 * Refresh the games list from the current games hash table.
 */
void gnostr_chess_games_browser_refresh(GnostrChessGamesBrowser *self);

/**
 * gnostr_chess_games_browser_set_loading:
 * @self: The browser
 * @loading: Whether games are being loaded
 *
 * Show or hide the loading indicator.
 */
void gnostr_chess_games_browser_set_loading(GnostrChessGamesBrowser *self,
                                             gboolean loading);

G_END_DECLS

#endif /* GNOSTR_CHESS_GAMES_BROWSER_H */
