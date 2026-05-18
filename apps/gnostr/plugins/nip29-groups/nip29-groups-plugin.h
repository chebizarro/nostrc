/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip29-groups-plugin.h - NIP-29 relay groups plugin
 */

#ifndef NIP29_GROUPS_PLUGIN_H
#define NIP29_GROUPS_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP29_TYPE_GROUPS_PLUGIN (nip29_groups_plugin_get_type())
G_DECLARE_FINAL_TYPE(Nip29GroupsPlugin, nip29_groups_plugin, NIP29, GROUPS_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP29_GROUPS_PLUGIN_H */
