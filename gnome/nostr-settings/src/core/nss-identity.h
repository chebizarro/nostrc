/* nss-identity.h — Identity page model: the signer's active identity and
 * relays (org.nostr.Signer) and the identities stored in the unified
 * keyring schema (gnome/seahorse/secret_store.h, read-only: attributes
 * only, never secrets, never unlocking).
 * SPDX-License-Identifier: MIT
 */
#ifndef NSS_IDENTITY_H
#define NSS_IDENTITY_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  gchar   *npub;
  gchar   *label;     /* may be NULL */
  gchar   *origin;    /* "software" | "hardware" | NULL */
  gboolean active;    /* matches the signer's GetPublicKey */
} NssIdentity;

void       nss_identity_free(NssIdentity *id);

/* GetPublicKey → npub, or NULL with @error (no signer / locked). */
gchar     *nss_signer_get_npub(GDBusConnection *bus, GError **error);
/* GetRelays → relay URLs (JSON array of strings, or of {"url": …}
 * objects). NotFound → empty array, no error. */
gchar    **nss_signer_get_relays(GDBusConnection *bus, GError **error);
/* Parse the GetRelays JSON (exposed for tests). */
gchar    **nss_signer_parse_relays_json(const gchar *json, GError **error);

/* Keyring identities (gnostr_secret_store_find_all), deduplicated by npub,
 * sorted by label then npub; @active_npub marks the active one. Returns an
 * empty array when built without libsecret. */
GPtrArray *nss_keyring_identities(const gchar *active_npub, GError **error);
gboolean   nss_keyring_available(void);

G_END_DECLS

#endif /* NSS_IDENTITY_H */
