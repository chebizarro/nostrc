/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip77-negentropy-plugin.h - NIP-77 Negentropy Sync Plugin
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP77_NEGENTROPY_PLUGIN_H
#define NIP77_NEGENTROPY_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP77_TYPE_NEGENTROPY_PLUGIN (nip77_negentropy_plugin_get_type())
G_DECLARE_FINAL_TYPE(Nip77NegentropyPlugin, nip77_negentropy_plugin, NIP77, NEGENTROPY_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP77_NEGENTROPY_PLUGIN_H */
