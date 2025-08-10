#include <glib.h>
#include <string.h>
#include <openssl/crypto.h>
#include "nostr/nip44/nip44.h"

/* GLib-friendly wrappers */

gboolean nostr_nip44_encrypt_v2_g(const guint8 sender_sk[32],
                                  const guint8 receiver_pk_xonly[32],
                                  GBytes *plaintext_utf8,
                                  gchar **out_base64,
                                  GError **error) {
  if (!sender_sk || !receiver_pk_xonly || !plaintext_utf8 || !out_base64) {
    if (error) *error = g_error_new_literal(g_quark_from_static_string("nip44"), 1, "invalid args");
    return FALSE;
  }
  gsize len = 0;
  const guint8 *data = g_bytes_get_data(plaintext_utf8, &len);
  char *b64 = NULL;
  if (nostr_nip44_encrypt_v2(sender_sk, receiver_pk_xonly, data, (size_t)len, &b64) != 0) {
    if (error) *error = g_error_new_literal(g_quark_from_static_string("nip44"), 2, "encrypt failed");
    return FALSE;
  }
  *out_base64 = b64;
  return TRUE;
}


gboolean nostr_nip44_decrypt_v2_g(const guint8 receiver_sk[32],
                                  const guint8 sender_pk_xonly[32],
                                  const gchar *base64_payload,
                                  GBytes **out_plaintext_utf8,
                                  GError **error) {
  if (!receiver_sk || !sender_pk_xonly || !base64_payload || !out_plaintext_utf8) {
    if (error) *error = g_error_new_literal(g_quark_from_static_string("nip44"), 1, "invalid args");
    return FALSE;
  }
  uint8_t *plain = NULL; size_t plain_len = 0;
  if (nostr_nip44_decrypt_v2(receiver_sk, sender_pk_xonly, base64_payload, &plain, &plain_len) != 0) {
    if (error) *error = g_error_new_literal(g_quark_from_static_string("nip44"), 3, "decrypt failed");
    return FALSE;
  }
  *out_plaintext_utf8 = g_bytes_new_take(plain, plain_len);
  return TRUE;
}
