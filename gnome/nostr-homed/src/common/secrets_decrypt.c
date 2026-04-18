/* secrets_decrypt.c - Decrypt secrets envelope via org.nostr.Signer
 *
 * SPDX-License-Identifier: MIT
 *
 * Calls NIP44Decrypt on the real signer D-Bus interface. The secrets
 * envelope (kind 30079) is self-encrypted: the peer_pubkey is the
 * user's own pubkey. We fetch the pubkey from the signer automatically.
 *
 * SECURITY: There is intentionally NO fallback/passthrough. If
 * decryption fails, the caller MUST treat it as a hard error and
 * NOT write ciphertext to tmpfs.
 */

#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include "nostr_secrets.h"
#include "nostr_dbus.h"

/**
 * Fetch the signer's public key as npub bech32 string.
 * Caller must g_free the returned string.
 */
static char *
get_signer_npub(void)
{
  const char *busname = nh_signer_bus_name();
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) {
    if (err) g_error_free(err);
    return NULL;
  }

  GVariant *ret = g_dbus_connection_call_sync(
    bus, busname,
    "/org/nostr/signer", "org.nostr.Signer", "GetPublicKey",
    NULL, G_VARIANT_TYPE("(s)"),
    G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);

  char *npub = NULL;
  if (ret) {
    const char *val = NULL;
    g_variant_get(ret, "(&s)", &val);
    if (val && val[0] != '\0')
      npub = g_strdup(val);
    g_variant_unref(ret);
  }
  if (err) g_error_free(err);
  g_object_unref(bus);
  return npub;
}

/**
 * nh_secrets_decrypt_via_signer:
 * @ciphertext: NIP-44 encrypted secrets envelope content
 * @plaintext_out: (out): receives newly-allocated plaintext on success
 *
 * Fetches the user's own npub from the signer, then calls NIP44Decrypt
 * with the npub as both peer_pubkey and current_user. The signer
 * resolves npub to hex internally for the key derivation.
 *
 * Returns 0 on success, -1 on failure. On failure, *plaintext_out is NULL.
 */
int nh_secrets_decrypt_via_signer(const char *ciphertext, char **plaintext_out)
{
  if (!plaintext_out) return -1;
  *plaintext_out = NULL;
  if (!ciphertext) return -1;

  /* Get own identity for self-decrypt */
  char *npub = get_signer_npub();
  if (!npub) {
    g_warning("secrets_decrypt: cannot get signer pubkey");
    return -1;
  }

  const char *busname = nh_signer_bus_name();
  GError *err = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (!bus) {
    if (err) {
      g_warning("secrets_decrypt: D-Bus unavailable: %s", err->message);
      g_error_free(err);
    }
    g_free(npub);
    return -1;
  }

  /*
   * Real signer interface: NIP44Decrypt(ciphertext, peer_pubkey, current_user)
   * For self-encrypted secrets, pass our npub as both peer and identity.
   * The signer accepts npub bech32 in the peer_pubkey field and converts
   * internally, and uses current_user as the identity selector.
   */
  GVariant *ret = g_dbus_connection_call_sync(
    bus, busname,
    "/org/nostr/signer", "org.nostr.Signer", "NIP44Decrypt",
    g_variant_new("(sss)", ciphertext, npub, npub),
    G_VARIANT_TYPE("(s)"),
    G_DBUS_CALL_FLAGS_NONE, 5000 /* 5s timeout */, NULL, &err);

  g_free(npub);

  if (!ret) {
    g_warning("secrets_decrypt: NIP44Decrypt failed: %s",
              err ? err->message : "unknown error");
    if (err) g_error_free(err);
    g_object_unref(bus);
    return -1;
  }

  const char *pt = NULL;
  g_variant_get(ret, "(&s)", &pt);
  if (pt && pt[0] != '\0')
    *plaintext_out = strdup(pt);
  g_variant_unref(ret);
  g_object_unref(bus);

  if (!*plaintext_out) {
    g_warning("secrets_decrypt: NIP44Decrypt returned empty plaintext");
    return -1;
  }

  return 0;
}
