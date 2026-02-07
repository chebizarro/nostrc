#ifndef GNOSTR_NIP19_H
#define GNOSTR_NIP19_H

#include <glib-object.h>
#include "nostr-error.h"

G_BEGIN_DECLS

/**
 * GNostrBech32Type:
 * @GNOSTR_BECH32_UNKNOWN: Unknown or invalid bech32 type
 * @GNOSTR_BECH32_NPUB: Public key (npub1...)
 * @GNOSTR_BECH32_NSEC: Secret key (nsec1...)
 * @GNOSTR_BECH32_NOTE: Event ID (note1...)
 * @GNOSTR_BECH32_NPROFILE: Profile pointer with relays (nprofile1...)
 * @GNOSTR_BECH32_NEVENT: Event pointer with metadata (nevent1...)
 * @GNOSTR_BECH32_NADDR: Addressable entity pointer (naddr1...)
 * @GNOSTR_BECH32_NRELAY: Relay pointer (nrelay1...)
 *
 * NIP-19 bech32 entity types. Mirrors the C-level NostrBech32Type
 * but registered as a GObject enum for property/signal use.
 */
typedef enum {
  GNOSTR_BECH32_UNKNOWN = 0,
  GNOSTR_BECH32_NPUB,
  GNOSTR_BECH32_NSEC,
  GNOSTR_BECH32_NOTE,
  GNOSTR_BECH32_NPROFILE,
  GNOSTR_BECH32_NEVENT,
  GNOSTR_BECH32_NADDR,
  GNOSTR_BECH32_NRELAY
} GNostrBech32Type;

GType gnostr_bech32_type_get_type(void) G_GNUC_CONST;
#define GNOSTR_TYPE_BECH32_TYPE (gnostr_bech32_type_get_type())

/**
 * GNostrNip19:
 *
 * A GObject wrapper for NIP-19 bech32 encoding and decoding.
 *
 * Supports all NIP-19 entity types: npub, nsec, note, nprofile,
 * nevent, naddr, and nrelay. Constructed by decoding a bech32 string
 * or by encoding from components.
 *
 * ## Properties
 *
 * - #GNostrNip19:entity-type - The #GNostrBech32Type of this entity
 * - #GNostrNip19:bech32 - The bech32-encoded string
 * - #GNostrNip19:pubkey - Public key hex (npub, nprofile, naddr)
 * - #GNostrNip19:event-id - Event ID hex (note, nevent)
 * - #GNostrNip19:author - Author pubkey hex (nevent, naddr)
 * - #GNostrNip19:kind - Event kind (nevent, naddr); -1 if unset
 * - #GNostrNip19:identifier - d-tag identifier (naddr)
 * - #GNostrNip19:relays - Relay URLs as #GStrv (nprofile, nevent, naddr, nrelay)
 *
 * Since: 0.1
 */
#define GNOSTR_TYPE_NIP19 (gnostr_nip19_get_type())
G_DECLARE_FINAL_TYPE(GNostrNip19, gnostr_nip19, GNOSTR, NIP19, GObject)

/* ── Decode constructor ──────────────────────────────────────────── */

/**
 * gnostr_nip19_decode:
 * @bech32: a NIP-19 bech32 string (npub1..., nsec1..., note1..., etc.)
 * @error: (nullable): return location for a #GError
 *
 * Decodes any NIP-19 bech32 string into a #GNostrNip19 object.
 * Use gnostr_nip19_get_entity_type() to determine which fields are valid.
 *
 * Returns: (transfer full) (nullable): a new #GNostrNip19, or %NULL on error
 */
GNostrNip19 *gnostr_nip19_decode(const gchar *bech32, GError **error);

/* ── Encode constructors ─────────────────────────────────────────── */

/**
 * gnostr_nip19_encode_npub:
 * @pubkey_hex: 64-character hex-encoded public key
 * @error: (nullable): return location for a #GError
 *
 * Encodes a public key as an npub bech32 string.
 *
 * Returns: (transfer full) (nullable): a new #GNostrNip19, or %NULL on error
 */
GNostrNip19 *gnostr_nip19_encode_npub(const gchar *pubkey_hex, GError **error);

/**
 * gnostr_nip19_encode_nsec:
 * @seckey_hex: 64-character hex-encoded secret key
 * @error: (nullable): return location for a #GError
 *
 * Encodes a secret key as an nsec bech32 string.
 *
 * Returns: (transfer full) (nullable): a new #GNostrNip19, or %NULL on error
 */
GNostrNip19 *gnostr_nip19_encode_nsec(const gchar *seckey_hex, GError **error);

/**
 * gnostr_nip19_encode_note:
 * @event_id_hex: 64-character hex-encoded event ID
 * @error: (nullable): return location for a #GError
 *
 * Encodes an event ID as a note bech32 string.
 *
 * Returns: (transfer full) (nullable): a new #GNostrNip19, or %NULL on error
 */
GNostrNip19 *gnostr_nip19_encode_note(const gchar *event_id_hex, GError **error);

/**
 * gnostr_nip19_encode_nprofile:
 * @pubkey_hex: 64-character hex-encoded public key
 * @relays: (nullable) (array zero-terminated=1): relay URLs, or %NULL
 * @error: (nullable): return location for a #GError
 *
 * Encodes a profile pointer as an nprofile bech32 string.
 *
 * Returns: (transfer full) (nullable): a new #GNostrNip19, or %NULL on error
 */
GNostrNip19 *gnostr_nip19_encode_nprofile(const gchar *pubkey_hex,
                                           const gchar *const *relays,
                                           GError **error);

/**
 * gnostr_nip19_encode_nevent:
 * @event_id_hex: 64-character hex-encoded event ID
 * @relays: (nullable) (array zero-terminated=1): relay URLs, or %NULL
 * @author_hex: (nullable): 64-character hex-encoded author pubkey, or %NULL
 * @kind: event kind, or -1 to omit
 * @error: (nullable): return location for a #GError
 *
 * Encodes an event pointer as an nevent bech32 string.
 *
 * Returns: (transfer full) (nullable): a new #GNostrNip19, or %NULL on error
 */
GNostrNip19 *gnostr_nip19_encode_nevent(const gchar *event_id_hex,
                                         const gchar *const *relays,
                                         const gchar *author_hex,
                                         gint kind,
                                         GError **error);

/**
 * gnostr_nip19_encode_naddr:
 * @identifier: the d-tag identifier string
 * @author_hex: 64-character hex-encoded author pubkey
 * @kind: event kind (required, must be >= 0)
 * @relays: (nullable) (array zero-terminated=1): relay URLs, or %NULL
 * @error: (nullable): return location for a #GError
 *
 * Encodes an addressable entity as an naddr bech32 string.
 *
 * Returns: (transfer full) (nullable): a new #GNostrNip19, or %NULL on error
 */
GNostrNip19 *gnostr_nip19_encode_naddr(const gchar *identifier,
                                        const gchar *author_hex,
                                        gint kind,
                                        const gchar *const *relays,
                                        GError **error);

/**
 * gnostr_nip19_encode_nrelay:
 * @relays: (array zero-terminated=1): one or more relay URLs
 * @error: (nullable): return location for a #GError
 *
 * Encodes relay URL(s) as an nrelay bech32 string.
 *
 * Returns: (transfer full) (nullable): a new #GNostrNip19, or %NULL on error
 */
GNostrNip19 *gnostr_nip19_encode_nrelay(const gchar *const *relays,
                                         GError **error);

/* ── Inspection ──────────────────────────────────────────────────── */

/**
 * gnostr_nip19_inspect:
 * @bech32: a candidate NIP-19 bech32 string
 *
 * Quickly determines the type of a NIP-19 bech32 string without
 * performing a full decode.
 *
 * Returns: the #GNostrBech32Type, or %GNOSTR_BECH32_UNKNOWN on error
 */
GNostrBech32Type gnostr_nip19_inspect(const gchar *bech32);

/* ── Property accessors ──────────────────────────────────────────── */

/**
 * gnostr_nip19_get_entity_type:
 * @self: a #GNostrNip19
 *
 * Returns: the #GNostrBech32Type of this entity
 */
GNostrBech32Type gnostr_nip19_get_entity_type(GNostrNip19 *self);

/**
 * gnostr_nip19_get_bech32:
 * @self: a #GNostrNip19
 *
 * Returns: (transfer none): the bech32-encoded string
 */
const gchar *gnostr_nip19_get_bech32(GNostrNip19 *self);

/**
 * gnostr_nip19_get_pubkey:
 * @self: a #GNostrNip19
 *
 * Gets the public key hex string. Valid for npub, nprofile, and naddr types.
 *
 * Returns: (transfer none) (nullable): public key hex, or %NULL if not applicable
 */
const gchar *gnostr_nip19_get_pubkey(GNostrNip19 *self);

/**
 * gnostr_nip19_get_event_id:
 * @self: a #GNostrNip19
 *
 * Gets the event ID hex string. Valid for note and nevent types.
 *
 * Returns: (transfer none) (nullable): event ID hex, or %NULL if not applicable
 */
const gchar *gnostr_nip19_get_event_id(GNostrNip19 *self);

/**
 * gnostr_nip19_get_author:
 * @self: a #GNostrNip19
 *
 * Gets the author pubkey hex string. Valid for nevent and naddr types.
 *
 * Returns: (transfer none) (nullable): author hex, or %NULL if not applicable
 */
const gchar *gnostr_nip19_get_author(GNostrNip19 *self);

/**
 * gnostr_nip19_get_kind:
 * @self: a #GNostrNip19
 *
 * Gets the event kind. Valid for nevent and naddr types.
 *
 * Returns: the event kind, or -1 if not applicable/unset
 */
gint gnostr_nip19_get_kind(GNostrNip19 *self);

/**
 * gnostr_nip19_get_identifier:
 * @self: a #GNostrNip19
 *
 * Gets the d-tag identifier. Valid only for naddr type.
 *
 * Returns: (transfer none) (nullable): identifier string, or %NULL if not applicable
 */
const gchar *gnostr_nip19_get_identifier(GNostrNip19 *self);

/**
 * gnostr_nip19_get_relays:
 * @self: a #GNostrNip19
 *
 * Gets the relay URLs. Valid for nprofile, nevent, naddr, and nrelay types.
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): relay URLs, or %NULL
 */
const gchar *const *gnostr_nip19_get_relays(GNostrNip19 *self);

G_END_DECLS

#endif /* GNOSTR_NIP19_H */
