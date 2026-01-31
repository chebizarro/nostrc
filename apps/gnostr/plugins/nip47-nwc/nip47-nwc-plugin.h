/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip47-nwc-plugin.h - NIP-47 Nostr Wallet Connect Plugin
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP47_NWC_PLUGIN_H
#define NIP47_NWC_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP47_TYPE_NWC_PLUGIN (nip47_nwc_plugin_get_type())
G_DECLARE_FINAL_TYPE(Nip47NwcPlugin, nip47_nwc_plugin, NIP47, NWC_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP47_NWC_PLUGIN_H */
