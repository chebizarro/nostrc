/* SPDX-License-Identifier: GPL-3.0-or-later
 * mls-groups-plugin.h - MLS Group Messaging Plugin
 *
 * Implements the Marmot protocol (MIP-00 through MIP-04) for secure
 * group messaging over Nostr using MLS (RFC 9420).
 *
 * Fully interoperable with Whitenoise and MDK-compatible clients.
 *
 * Event kinds handled:
 *   - 443  (MLS Key Package, MIP-00)
 *   - 444  (MLS Welcome, MIP-02, via NIP-59 gift wrap)
 *   - 445  (MLS Group Message, MIP-03)
 *   - 1059 (NIP-59 Gift Wrap, for welcome delivery)
 *   - 10051 (Key Package Relay List, MIP-00)
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef MLS_GROUPS_PLUGIN_H
#define MLS_GROUPS_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define MLS_TYPE_GROUPS_PLUGIN (mls_groups_plugin_get_type())
G_DECLARE_FINAL_TYPE(MlsGroupsPlugin, mls_groups_plugin, MLS, GROUPS_PLUGIN, GObject)

G_END_DECLS

#endif /* MLS_GROUPS_PLUGIN_H */
