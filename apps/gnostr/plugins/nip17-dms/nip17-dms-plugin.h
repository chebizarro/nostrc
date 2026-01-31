/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip17-dms-plugin.h - NIP-17 Private DMs Plugin Header
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP17_DMS_PLUGIN_H
#define NIP17_DMS_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP17_TYPE_DMS_PLUGIN (nip17_dms_plugin_get_type())
G_DECLARE_FINAL_TYPE(Nip17DmsPlugin, nip17_dms_plugin, NIP17, DMS_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP17_DMS_PLUGIN_H */
