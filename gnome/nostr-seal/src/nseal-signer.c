/* nseal-signer.c — org.nostr.Signer client lane (see nseal-signer.h).
 * SPDX-License-Identifier: MIT */

#include "nseal-signer.h"
#include "nseal-private.h"

#include <gio/gio.h>
#include <string.h>
#include <openssl/crypto.h>

#define SIGNER_BUS   "org.nostr.Signer"
#define SIGNER_PATH  "/org/nostr/signer"
#define SIGNER_IFACE "org.nostr.Signer"
/* Long enough for a human to answer the signer's approval prompt. */
#define APPROVAL_TIMEOUT_MS (180 * 1000)

struct _NsealSigner {
  GDBusConnection *bus;
  char *identity;
};

void nseal_signer_free(NsealSigner *s) {
  if (!s) return;
  g_clear_object(&s->bus);
  g_free(s->identity);
  g_free(s);
}

NsealSigner *nseal_signer_new(const char *identity, GError **error) {
  GError *e = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  if (!bus) {
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_SIGNER, "no session bus: %s", e->message);
    g_error_free(e);
    return NULL;
  }
  NsealSigner *s = g_new0(NsealSigner, 1);
  s->bus = bus;
  s->identity = g_strdup(identity ? identity : "");
  return s;
}

static void signer_error(GError **error, GError *e, const char *what) {
  g_autofree gchar *remote = g_dbus_error_get_remote_error(e);
  if (remote && g_str_equal(remote, "org.nostr.Signer.Error.ApprovalDenied"))
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_SIGNER, "%s: the signer denied the request", what);
  else if (remote && g_str_equal(remote, "org.nostr.Signer.Error.NoKeyConfigured"))
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_SIGNER, "%s: the signer has no key for this identity", what);
  else if (g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
           g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER))
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_SIGNER,
                "%s: no Nostr signer is running (org.nostr.Signer)", what);
  else {
    g_dbus_error_strip_remote_error(e);
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_SIGNER, "%s: %s", what, e->message);
  }
}

gboolean nseal_signer_public_key(NsealSigner *s, uint8_t out[32], GError **error) {
  if (s->identity && *s->identity)
    return nseal_parse_pubkey(s->identity, out, error);
  GError *e = NULL;
  GVariant *r = g_dbus_connection_call_sync(s->bus, SIGNER_BUS, SIGNER_PATH, SIGNER_IFACE,
                                            "GetPublicKey", NULL, G_VARIANT_TYPE("(s)"),
                                            G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &e);
  if (!r) { signer_error(error, e, "GetPublicKey"); g_error_free(e); return FALSE; }
  const char *npub = NULL;
  g_variant_get(r, "(&s)", &npub);
  gboolean ok = nseal_parse_pubkey(npub, out, error);
  g_variant_unref(r);
  return ok;
}

static void to_hex(const uint8_t *in, gsize n, char *out) {
  static const char HX[] = "0123456789abcdef";
  for (gsize i = 0; i < n; i++) { out[2*i] = HX[in[i] >> 4]; out[2*i+1] = HX[in[i] & 15]; }
  out[2*n] = '\0';
}

gboolean nseal_signer_unwrap(gpointer signer, const uint8_t eph[32], const char *payload,
                             uint8_t out_file_key[32], GError **error) {
  NsealSigner *s = signer;
  char eph_hex[65];
  to_hex(eph, 32, eph_hex);

  GError *e = NULL;
  GVariant *r = g_dbus_connection_call_sync(s->bus, SIGNER_BUS, SIGNER_PATH, SIGNER_IFACE,
                                            "NIP44DeriveConversationKey",
                                            g_variant_new("(sss)", eph_hex, s->identity, NSEAL_SIGNER_APP_ID),
                                            G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
                                            APPROVAL_TIMEOUT_MS, NULL, &e);
  if (r) {
    const char *hex = NULL;
    g_variant_get(r, "(&s)", &hex);
    uint8_t ck[32];
    gboolean ok = strlen(hex) == 64;
    for (int i = 0; i < 32 && ok; i++) {
      int hi = g_ascii_xdigit_value(hex[2*i]), lo = g_ascii_xdigit_value(hex[2*i+1]);
      if (hi < 0 || lo < 0) ok = FALSE; else ck[i] = (uint8_t)((hi << 4) | lo);
    }
    g_variant_unref(r);
    if (!ok) {
      g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_SIGNER, "signer returned a malformed conversation key");
      return FALSE;
    }
    ok = nseal_unwrap_with_convkey(ck, payload, out_file_key, error);
    OPENSSL_cleanse(ck, sizeof ck);
    return ok;
  }
  if (!g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
    signer_error(error, e, "NIP44DeriveConversationKey");
    g_error_free(e);
    return FALSE;
  }
  g_clear_error(&e);

  /* Pre-0.3.0 signer: have it open the standard NIP-44 payload itself. */
  r = g_dbus_connection_call_sync(s->bus, SIGNER_BUS, SIGNER_PATH, SIGNER_IFACE,
                                  "NIP44DecryptB64",
                                  g_variant_new("(sss)", payload, eph_hex, s->identity),
                                  G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE,
                                  APPROVAL_TIMEOUT_MS, NULL, &e);
  if (!r) { signer_error(error, e, "NIP44DecryptB64"); g_error_free(e); return FALSE; }
  const char *b64 = NULL;
  g_variant_get(r, "(&s)", &b64);
  gsize n = 0;
  guchar *key = g_base64_decode(b64, &n);
  g_variant_unref(r);
  gboolean ok = key && n == NSEAL_FILE_KEY_LEN;
  if (ok) memcpy(out_file_key, key, 32);
  else g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_UNWRAP, "key stanza did not open with this identity");
  if (key) { OPENSSL_cleanse(key, n); g_free(key); }
  return ok;
}
