/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip55-androidsigner-plugin.h - NIP-55 Android Signer Plugin
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP55_ANDROIDSIGNER_PLUGIN_H
#define NIP55_ANDROIDSIGNER_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP55_TYPE_ANDROIDSIGNER_PLUGIN (nip55_androidsigner_plugin_get_type())
G_DECLARE_FINAL_TYPE(Nip55AndroidsignerPlugin, nip55_androidsigner_plugin, NIP55, ANDROIDSIGNER_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP55_ANDROIDSIGNER_PLUGIN_H */
