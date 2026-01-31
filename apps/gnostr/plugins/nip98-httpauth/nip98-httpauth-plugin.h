/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip98-httpauth-plugin.h - NIP-98 HTTP Auth Plugin
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP98_HTTPAUTH_PLUGIN_H
#define NIP98_HTTPAUTH_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP98_TYPE_HTTPAUTH_PLUGIN (nip98_httpauth_plugin_get_type())
G_DECLARE_FINAL_TYPE(Nip98HttpAuthPlugin, nip98_httpauth_plugin, NIP98, HTTPAUTH_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP98_HTTPAUTH_PLUGIN_H */
