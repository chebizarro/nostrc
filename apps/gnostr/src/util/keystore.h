/**
 * Identity metadata shim (nostrc-e5nz)
 *
 * GNostr never holds private keys. Signing and encryption go through a
 * signer: org.nostr.Signer over D-Bus (Grotto / the nip55l daemon)
 * or a NIP-46 remote signer (see ipc/gnostr-signer-service.h). Keys are
 * created, imported, backed up and removed in Grotto.
 *
 * This module only reads *metadata* — npub and label, never secrets — and
 * never writes or deletes anything:
 *
 * - Linux (HAVE_LIBSECRET): the signer's org.gnostr.Signer/identity items
 *   (schema owned by gnome/seahorse, gnostr-secret), plus the retired client
 *   keystore org.gnostr.NostrKey, only to tell whether keys an older GNostr
 *   stored still wait for the signer daemon's startup migration. Searches
 *   neither unlock collections nor load secrets.
 * - macOS (HAVE_MACOS_KEYCHAIN): the signer daemon's Keychain items
 *   (service "Gnostr Identity Key") and the retired client items (service
 *   "org.gnostr.Client"), attributes only.
 * - Elsewhere: nothing is available.
 */

#ifndef GNOSTR_KEYSTORE_H
#define GNOSTR_KEYSTORE_H

#include <glib.h>
#include <gio/gio.h>
/* nostrc-ecrx: GnostrKeyInfo + GnostrKeystoreError + GNOSTR_KEYSTORE_ERROR
 * are defined by the nostr-gobject bridge header so both the app-side
 * shim (this file) and the library's identity consumer
 * (nostr-gobject/src/gnostr-identity.c) see the same layout. */
#include <nostr-gobject-1.0/gnostr-app-bridge.h>

G_BEGIN_DECLS

/**
 * gnostr_keystore_available:
 *
 * Returns: %TRUE if the platform key store can be queried for identity
 *   metadata (Secret Service reachable / Keychain present).
 */
gboolean gnostr_keystore_available(void);

/**
 * gnostr_keystore_list_keys:
 * @error: (nullable): return location for a #GError
 *
 * Identities the signer holds, one entry per npub. Metadata only.
 *
 * Returns: (transfer full) (element-type GnostrKeyInfo): list; free with
 *   g_list_free_full(list, (GDestroyNotify)gnostr_key_info_free). %NULL
 *   when there are none or on error.
 */
GList *gnostr_keystore_list_keys(GError **error);

/**
 * gnostr_keystore_has_key:
 * @npub: bech32 public key
 *
 * Returns: %TRUE if the signer's key store holds an identity for @npub.
 */
gboolean gnostr_keystore_has_key(const char *npub);

/**
 * gnostr_keystore_list_legacy_keys:
 * @error: (nullable): return location for a #GError
 *
 * Keys a GNostr release before nostrc-e5nz stored in the client's own
 * keystore and that are still there (not yet imported into the signer).
 * Only npubs are returned; @label is %NULL.
 *
 * Returns: (transfer full) (element-type GnostrKeyInfo): list, or %NULL.
 */
GList *gnostr_keystore_list_legacy_keys(GError **error);

/**
 * gnostr_keystore_legacy_migrates_automatically:
 *
 * Returns: %TRUE if the signer daemon imports legacy client keys by itself
 *   when it starts (Linux: org.gnostr.NostrKey is on its migration list;
 *   macOS: the "org.gnostr.Client" Keychain items, nostrc-de9h);
 *   %FALSE if the user must import them in Grotto by hand.
 */
gboolean gnostr_keystore_legacy_migrates_automatically(void);

/**
 * gnostr_key_info_free:
 * @info: (nullable): a #GnostrKeyInfo
 */
void gnostr_key_info_free(GnostrKeyInfo *info);

/**
 * gnostr_key_info_copy:
 * @info: (nullable): a #GnostrKeyInfo
 *
 * Returns: (transfer full) (nullable): a deep copy
 */
GnostrKeyInfo *gnostr_key_info_copy(const GnostrKeyInfo *info);

/* Error domain quark. GNOSTR_KEYSTORE_ERROR macro and the enum come from
 * the nostr-gobject bridge header. */
GQuark gnostr_keystore_error_quark(void);

G_END_DECLS

#endif /* GNOSTR_KEYSTORE_H */
