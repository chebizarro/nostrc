/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip99-marketplace-plugin.h - NIP-99 Marketplace Plugin
 *
 * Implements NIP-99 (Classified Listings) for browsing and publishing
 * marketplace listings on Nostr.
 *
 * Event kinds handled:
 * - 30402: Classified Listing
 * - 30403: Draft Classified Listing
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP99_MARKETPLACE_PLUGIN_H
#define NIP99_MARKETPLACE_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP99_TYPE_MARKETPLACE_PLUGIN (nip99_marketplace_plugin_get_type())

G_DECLARE_FINAL_TYPE(Nip99MarketplacePlugin, nip99_marketplace_plugin,
                     NIP99, MARKETPLACE_PLUGIN, GObject)

/**
 * nip99_marketplace_plugin_get_listings:
 * @self: The marketplace plugin
 *
 * Get the hash table of cached classified listings from relays.
 *
 * Returns: (transfer none): Hash table mapping event_id -> GnostrClassified*
 */
GHashTable *nip99_marketplace_plugin_get_listings(Nip99MarketplacePlugin *self);

/**
 * nip99_marketplace_plugin_request_listings:
 * @self: The marketplace plugin
 *
 * Request fresh classified listings from configured relays.
 */
void nip99_marketplace_plugin_request_listings(Nip99MarketplacePlugin *self);

/**
 * nip99_marketplace_plugin_get_listing_count:
 * @self: The marketplace plugin
 *
 * Returns: Number of cached listings.
 */
guint nip99_marketplace_plugin_get_listing_count(Nip99MarketplacePlugin *self);

G_END_DECLS

#endif /* NIP99_MARKETPLACE_PLUGIN_H */
