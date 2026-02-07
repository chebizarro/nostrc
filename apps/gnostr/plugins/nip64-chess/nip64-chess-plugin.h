/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip64-chess-plugin.h - NIP-64 Chess Plugin
 *
 * Implements NIP-64 (Chess Games) for playing and publishing chess games.
 * Handles event kind 64 (chess games in PGN format).
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP64_CHESS_PLUGIN_H
#define NIP64_CHESS_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP64_TYPE_CHESS_PLUGIN (nip64_chess_plugin_get_type())

G_DECLARE_FINAL_TYPE(Nip64ChessPlugin, nip64_chess_plugin, NIP64, CHESS_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP64_CHESS_PLUGIN_H */
