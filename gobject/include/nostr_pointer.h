#ifndef GNOSTR_POINTER_H
#define GNOSTR_POINTER_H

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * GNostrPointer:
 *
 * A GObject wrapper for NIP-19 pointers (nprofile, nevent, naddr, nrelay).
 * G-prefixed to avoid collision with core libnostr NostrPointer.
 *
 * Wraps the core NostrPointer tagged union and provides bech32 parse/encode.
 */
#define GNOSTR_TYPE_POINTER (gnostr_pointer_get_type())
G_DECLARE_FINAL_TYPE(GNostrPointer, gnostr_pointer, GNOSTR, POINTER, GObject)

/**
 * gnostr_pointer_new_from_bech32:
 * @bech32: a NIP-19 bech32 string (npub1..., note1..., nprofile1..., etc.)
 * @error: (nullable): return location for a #GError
 *
 * Parses a NIP-19 bech32 string into a GNostrPointer.
 *
 * Returns: (transfer full) (nullable): a new #GNostrPointer, or %NULL on error
 */
GNostrPointer *gnostr_pointer_new_from_bech32(const gchar *bech32, GError **error);

/**
 * gnostr_pointer_to_bech32:
 * @self: a #GNostrPointer
 * @error: (nullable): return location for a #GError
 *
 * Encodes the pointer back to a NIP-19 bech32 string.
 *
 * Returns: (transfer full) (nullable): bech32 string, or %NULL on error
 */
gchar *gnostr_pointer_to_bech32(GNostrPointer *self, GError **error);

/**
 * gnostr_pointer_get_kind_name:
 * @self: a #GNostrPointer
 *
 * Gets the pointer kind as a string ("nprofile", "nevent", "naddr", "nrelay", or "none").
 *
 * Returns: (transfer none): kind string
 */
const gchar *gnostr_pointer_get_kind_name(GNostrPointer *self);

G_END_DECLS

#endif /* GNOSTR_POINTER_H */
