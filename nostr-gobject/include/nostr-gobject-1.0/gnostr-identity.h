/**
 * GNostr Identity Management
 *
 * High-level identity management combining:
 * - Read-only identity metadata from the signer's key store (via the app
 *   bridge; npub and label only, never secrets)
 * - GSettings for preferences
 *
 * Clients never hold private keys (nostrc-e5nz): keys are created,
 * imported and removed in the signer (org.nostr.Signer / NIP-46), and all
 * signing goes through it. This API therefore has no import, export or
 * delete of key material.
 *
 * The schema_id must be set via gnostr_identity_init() before
 * calling any GSettings-dependent functions. The library has no
 * opinion about schema names - apps provide their own.
 */

#ifndef GNOSTR_IDENTITY_H
#define GNOSTR_IDENTITY_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * GNostrIdentity:
 * @npub: The bech32-encoded public key (npub1...)
 * @label: Human-readable label (e.g., NIP-05 name)
 * @signer_holds_key: Whether the local signer's key store (org.nostr.Signer,
 *   e.g. Grotto) holds this identity. The client itself never does.
 * @signer_type: Hint only — "nip55l" when @signer_holds_key, otherwise
 *   "external" (NIP-46 or unknown). Do not route signing by it; the app's
 *   signer service knows the method actually in use.
 *
 * Represents a user identity in the app.
 */
typedef struct {
  char *npub;
  char *label;
  gboolean signer_holds_key;
  char *signer_type;
} GNostrIdentity;

/**
 * gnostr_identity_init:
 * @schema_id: GSettings schema ID for identity settings (e.g., "org.gnostr.Client")
 *
 * Initialize the identity module with the GSettings schema to use.
 * Must be called before gnostr_identity_get_current() or
 * gnostr_identity_set_current().
 */
void gnostr_identity_init(const char *schema_id);

/**
 * gnostr_identity_free:
 * @identity: A #GNostrIdentity
 *
 * Free an identity structure.
 */
void gnostr_identity_free(GNostrIdentity *identity);

/**
 * gnostr_identity_copy:
 * @identity: A #GNostrIdentity
 *
 * Copy an identity structure.
 *
 * Returns: (transfer full): A copy of the identity.
 */
GNostrIdentity *gnostr_identity_copy(const GNostrIdentity *identity);

/**
 * gnostr_identity_get_current:
 *
 * Get the currently active identity from GSettings.
 * Requires gnostr_identity_init() to have been called.
 *
 * Returns: (transfer full) (nullable): The current identity, or %NULL if not logged in.
 */
GNostrIdentity *gnostr_identity_get_current(void);

/**
 * gnostr_identity_set_current:
 * @npub: The npub to set as current (or %NULL to log out)
 *
 * Set the currently active identity.
 * Requires gnostr_identity_init() to have been called.
 */
void gnostr_identity_set_current(const char *npub);

/**
 * gnostr_identity_list_stored:
 * @error: (out) (optional): Return location for error
 *
 * List the identities the local signer's key store holds (metadata only).
 *
 * Returns: (transfer full) (element-type GNostrIdentity): List of identities.
 */
GList *gnostr_identity_list_stored(GError **error);

/**
 * gnostr_identity_signer_holds_key:
 * @npub: The public key to check
 *
 * Returns: %TRUE if the local signer's key store holds @npub.
 */
gboolean gnostr_identity_signer_holds_key(const char *npub);

/**
 * gnostr_identity_secure_storage_available:
 *
 * Returns: %TRUE if the platform key store can be queried for identity
 *   metadata.
 */
gboolean gnostr_identity_secure_storage_available(void);

G_END_DECLS

#endif /* GNOSTR_IDENTITY_H */
