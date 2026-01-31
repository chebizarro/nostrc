/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip46-connect-plugin.h - NIP-46 Nostr Connect Plugin
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP46_CONNECT_PLUGIN_H
#define NIP46_CONNECT_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP46_TYPE_CONNECT_PLUGIN (nip46_connect_plugin_get_type())
G_DECLARE_FINAL_TYPE(Nip46ConnectPlugin, nip46_connect_plugin, NIP46, CONNECT_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP46_CONNECT_PLUGIN_H */
