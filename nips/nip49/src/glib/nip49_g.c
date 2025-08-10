#include <glib.h>
#include "nostr/nip49/nip49.h"

static int glib_nfkc_cb(const char *in_utf8, char **out_nfkc) {
  if (!in_utf8 || !out_nfkc) return -1;
  gchar *norm = g_utf8_normalize(in_utf8, -1, G_NORMALIZE_ALL_COMPOSE);
  if (!norm) return -1;
  *out_nfkc = (char*)norm; // caller must g_free
  return 0;
}

static void ensure_norm_cb_set(void) {
  static gsize once = 0;
  if (g_once_init_enter(&once)) {
    nostr_nip49_set_normalize_cb(glib_nfkc_cb);
    g_once_init_leave(&once, 1);
  }
}

gboolean nostr_nip49_encrypt_g(const guint8 privkey32[32],
                               guint8 security_byte,
                               const gchar *password_utf8,
                               guint8 log_n,
                               gchar **out_ncryptsec,
                               GError **error) {
  ensure_norm_cb_set();
  if (!privkey32 || !password_utf8 || !out_ncryptsec) {
    g_set_error(error, g_quark_from_static_string("nostr-nip49"), 1, "invalid arguments");
    return FALSE;
  }
  char *ncrypt = NULL;
  int rc = nostr_nip49_encrypt(privkey32, (NostrNip49SecurityByte)security_byte, password_utf8, log_n, &ncrypt);
  if (rc != 0) {
    g_set_error(error, g_quark_from_static_string("nostr-nip49"), rc, "encrypt failed");
    return FALSE;
  }
  *out_ncryptsec = ncrypt; // core returns malloc; acceptable to free with free(); callers may g_free if same allocator
  return TRUE;
}

gboolean nostr_nip49_decrypt_g(const gchar *ncryptsec_bech32,
                               const gchar *password_utf8,
                               guint8 out_privkey32[32],
                               guint8 *out_security_byte,
                               guint8 *out_log_n,
                               GError **error) {
  ensure_norm_cb_set();
  if (!ncryptsec_bech32 || !password_utf8 || !out_privkey32) {
    g_set_error(error, g_quark_from_static_string("nostr-nip49"), 1, "invalid arguments");
    return FALSE;
  }
  NostrNip49SecurityByte sec = 0; uint8_t ln = 0;
  int rc = nostr_nip49_decrypt(ncryptsec_bech32, password_utf8, out_privkey32,
                               out_security_byte ? &sec : NULL,
                               out_log_n ? &ln : NULL);
  if (rc != 0) {
    g_set_error(error, g_quark_from_static_string("nostr-nip49"), rc, "decrypt failed");
    return FALSE;
  }
  if (out_security_byte) *out_security_byte = (guint8)sec;
  if (out_log_n) *out_log_n = (guint8)ln;
  return TRUE;
}
