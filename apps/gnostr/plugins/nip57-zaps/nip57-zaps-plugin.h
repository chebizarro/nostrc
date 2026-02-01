/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip57-zaps-plugin.h - NIP-57 Lightning Zaps Plugin
 *
 * Implements NIP-57 (Lightning Zaps) for sending and receiving zaps.
 * Handles event kinds 9734 (zap request) and 9735 (zap receipt).
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP57_ZAPS_PLUGIN_H
#define NIP57_ZAPS_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP57_TYPE_ZAPS_PLUGIN (nip57_zaps_plugin_get_type())

G_DECLARE_FINAL_TYPE(Nip57ZapsPlugin, nip57_zaps_plugin, NIP57, ZAPS_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP57_ZAPS_PLUGIN_H */
