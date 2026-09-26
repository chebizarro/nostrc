/* gnostr-secret — the one libsecret schema for Nostr key material.
 *
 * nostrc-bml6: every private key the desktop stack keeps in the Secret
 * Service lives under GNOSTR_SECRET_SCHEMA_NAME, written by the signer
 * daemon (nips/nip55l, org.nostr.Signer). The name is the daemon's historic
 * "org.gnostr.Signer/identity", so items it stored before this library owned
 * the schema remain valid without migration. The attribute set is the union
 * of the daemon's identity attributes and the former Seahorse helper's
 * org.gnostr.Key attributes; see org.gnostr.secret.schema.txt.
 *
 * Legacy schemas are exported only so the daemon's one-shot migration can
 * find and retire their items.
 *
 * This header requires libsecret; only build it where libsecret-1 exists.
 */
#ifndef GNOME_SEAHORSE_SECRET_STORE_H
#define GNOME_SEAHORSE_SECRET_STORE_H

#include <glib.h>
#include <libsecret/secret.h>

G_BEGIN_DECLS

#define GNOSTR_SECRET_SCHEMA_NAME                   "org.gnostr.Signer/identity"
#define GNOSTR_SECRET_LEGACY_SIGNER_KEY_SCHEMA_NAME "org.gnostr.Signer/key"
#define GNOSTR_SECRET_LEGACY_HELPER_SCHEMA_NAME     "org.gnostr.Key"
#define GNOSTR_SECRET_MIGRATION_SCHEMA_NAME         "org.gnostr.Signer/migration"

#define GNOSTR_SECRET_CURVE           "secp256k1"
#define GNOSTR_SECRET_ORIGIN_SOFTWARE "software"
#define GNOSTR_SECRET_ORIGIN_HARDWARE "hardware"

/* Unified schema. Attributes (all strings):
 *   key_id          selector the daemon looks items up by (defaults to npub)
 *   npub            bech32 public key
 *   label           friendly name (was org.gnostr.Key "uid")
 *   hardware        "true" iff origin == "hardware"
 *   owner_uid       Unix uid linked to the identity (optional)
 *   owner_username  Unix username linked to the identity (optional)
 *   curve           always "secp256k1"
 *   origin          "software" | "hardware"
 *   hardware_slot   token-specific locator (optional, hardware only)
 *   fingerprint     first 8 hex chars of the pubkey (optional)
 *   created_at      ISO-8601 timestamp (optional) */
extern const SecretSchema gnostr_secret_schema;

/* Legacy: apps/gnostr-signer/src/secret-storage.c (removed)
 * {application,label,npub,key_type,created_at}. Secret is 64-hex. */
extern const SecretSchema gnostr_secret_legacy_signer_key_schema;

/* Legacy: this library's former org.gnostr.Key
 * {type,npub,uid,curve,origin,hardware_slot}. Secret is hex/nsec, or a
 * hardware reference when origin == "hardware". */
extern const SecretSchema gnostr_secret_legacy_helper_schema;

/* Per-keyring marker recording that a migration pass completed. {name} */
extern const SecretSchema gnostr_secret_migration_schema;

/* Description of one identity item. All pointers are borrowed. npub is
 * required; everything else is optional (NULL or "" = unset). key_id
 * defaults to npub and origin to "software". */
typedef struct {
  const gchar *key_id;
  const gchar *npub;
  const gchar *label;
  const gchar *owner_uid;
  const gchar *owner_username;
  const gchar *origin;
  const gchar *hardware_slot;
  const gchar *fingerprint;
  const gchar *created_at;
} GnostrSecretIdentity;

/* Build the Seahorse-visible item label documented in
 * org.gnostr.secret.schema.txt: "Nostr key: <uid> (<npub prefix>…)", or
 * "Nostr key: <npub prefix>…" when uid is NULL/empty. The full npub is kept
 * in the item attributes; the label abbreviates it so Seahorse's list stays
 * readable. Returns NULL when npub is NULL/empty. Free with g_free(). */
gchar *gnostr_secret_store_build_label(const gchar *uid, const gchar *npub);

/* Attribute table (static keys, g_free'd values) for id under
 * gnostr_secret_schema. Only set attributes are included; key_id, npub,
 * curve, origin and hardware are always present. NULL if npub is unset. */
GHashTable *gnostr_secret_identity_to_attributes(const GnostrSecretIdentity *id);

/* Legacy org.gnostr.Signer/key "key_type" → unified "origin". */
const gchar *gnostr_secret_origin_from_key_type(const gchar *key_type);

typedef enum {
  GNOSTR_SECRET_LEGACY_SIGNER_KEY, /* org.gnostr.Signer/key */
  GNOSTR_SECRET_LEGACY_HELPER_KEY  /* org.gnostr.Key */
} GnostrSecretLegacyKind;

/* Map a legacy item's attributes onto an identity. Pointers in *out borrow
 * from legacy_attrs. key_id/npub are the legacy npub hint (may be NULL); the
 * caller is expected to overwrite both from the npub derived from the secret
 * (the daemon uses the label as key_id when it differs from the npub).
 * Returns FALSE with *why_not set (static string) when the item must not be
 * migrated — currently: hardware references, which carry no private key the
 * signer daemon could use. */
gboolean gnostr_secret_legacy_to_identity(GnostrSecretLegacyKind kind,
                                          GHashTable *legacy_attrs,
                                          GnostrSecretIdentity *out,
                                          const gchar **why_not);

/* Store secret under gnostr_secret_schema in the default collection, then
 * delete any other item for the same {key_id, npub} whose attribute set
 * differs (e.g. one written before a schema attribute was added), so an
 * identity is always exactly one item. */
gboolean gnostr_secret_store_save(const GnostrSecretIdentity *id,
                                  const gchar *secret,
                                  GError **error);

/* Convenience: software key with an optional friendly uid. */
gboolean gnostr_secret_store_save_software_key(const gchar *npub,
                                                const gchar *uid,
                                                const gchar *secret, /* hex or nsec */
                                                GError **error);

/* All identity items: "npub|label" → attribute table (transfer full). */
GHashTable *gnostr_secret_store_find_all(GError **error);

/* Delete identity items matching npub and/or label (the former helper's
 * "uid"). At least one must be non-empty. Returns TRUE if every matched item
 * was deleted. */
gboolean gnostr_secret_store_delete_by_identity(const gchar *npub,
                                                const gchar *label,
                                                GError **error);

G_END_DECLS

#endif /* GNOME_SEAHORSE_SECRET_STORE_H */
