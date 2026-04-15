/*
 * marmot-gobject - GObject wrapper for libmarmot
 *
 * MarmotGobjectKeyPackage: GObject wrapper for MarmotKeyPackageInfo.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_GOBJECT_KEY_PACKAGE_H
#define MARMOT_GOBJECT_KEY_PACKAGE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define MARMOT_GOBJECT_TYPE_KEY_PACKAGE (marmot_gobject_key_package_get_type())
G_DECLARE_FINAL_TYPE(MarmotGobjectKeyPackage, marmot_gobject_key_package, MARMOT_GOBJECT, KEY_PACKAGE, GObject)

/**
 * MarmotGobjectKeyPackage:
 *
 * A GObject wrapper for a published MLS KeyPackage.
 *
 * ## Properties
 *
 * - #MarmotGobjectKeyPackage:ref - KeyPackageRef as hex string (32 bytes)
 * - #MarmotGobjectKeyPackage:owner-pubkey - Owner's Nostr pubkey as hex string
 * - #MarmotGobjectKeyPackage:relay-urls - Relay URLs as #GStrv
 * - #MarmotGobjectKeyPackage:created-at - Creation timestamp
 * - #MarmotGobjectKeyPackage:active - Whether this is the active key package
 *
 * Since: 1.0
 */

/**
 * marmot_gobject_key_package_new_from_data:
 * @ref_hex: KeyPackageRef as hex string (64 chars)
 * @owner_pubkey_hex: owner's Nostr pubkey as hex string (64 chars)
 * @relay_urls: (array zero-terminated=1) (nullable): relay URLs
 * @created_at: creation timestamp
 * @active: whether this key package is active
 *
 * Creates a new MarmotGobjectKeyPackage from individual fields.
 *
 * Returns: (transfer full): a new #MarmotGobjectKeyPackage
 */
MarmotGobjectKeyPackage *marmot_gobject_key_package_new_from_data(const gchar *ref_hex,
                                                                    const gchar *owner_pubkey_hex,
                                                                    const gchar * const *relay_urls,
                                                                    gint64 created_at,
                                                                    gboolean active);

/* ── Property accessors ──────────────────────────────────────────── */

/**
 * marmot_gobject_key_package_get_ref:
 * @self: a #MarmotGobjectKeyPackage
 *
 * Returns: (transfer none): the KeyPackageRef as a hex string
 */
const gchar *marmot_gobject_key_package_get_ref(MarmotGobjectKeyPackage *self);

/**
 * marmot_gobject_key_package_get_owner_pubkey:
 * @self: a #MarmotGobjectKeyPackage
 *
 * Returns: (transfer none): the owner's Nostr pubkey as a hex string
 */
const gchar *marmot_gobject_key_package_get_owner_pubkey(MarmotGobjectKeyPackage *self);

/**
 * marmot_gobject_key_package_get_relay_urls:
 * @self: a #MarmotGobjectKeyPackage
 *
 * Returns: (transfer none) (array zero-terminated=1) (nullable): relay URLs
 */
const gchar * const *marmot_gobject_key_package_get_relay_urls(MarmotGobjectKeyPackage *self);

/**
 * marmot_gobject_key_package_get_created_at:
 * @self: a #MarmotGobjectKeyPackage
 *
 * Returns: the creation timestamp
 */
gint64 marmot_gobject_key_package_get_created_at(MarmotGobjectKeyPackage *self);

/**
 * marmot_gobject_key_package_is_active:
 * @self: a #MarmotGobjectKeyPackage
 *
 * Returns: whether this is the active key package for the owner
 */
gboolean marmot_gobject_key_package_is_active(MarmotGobjectKeyPackage *self);

G_END_DECLS

#endif /* MARMOT_GOBJECT_KEY_PACKAGE_H */
