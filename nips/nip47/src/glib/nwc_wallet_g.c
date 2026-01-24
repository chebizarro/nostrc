/* GLib wrappers for NIP-47 wallet */

#include <glib.h>
#include <stdlib.h>
#include "nostr/nip47/nwc_wallet.h"
#include "nostr/nip47/nwc.h"
#include "nostr/nip47/nwc_envelope.h"

/* Error quark for NWC wallet operations */
#define NOSTR_NWC_WALLET_ERROR_QUARK g_quark_from_static_string("nostr-nwc-wallet-error")

/* Create a wallet session. Returns allocated pointer or NULL on error. */
gpointer nostr_nwc_wallet_session_init_g(const gchar *client_pub_hex,
                                         const gchar **wallet_supported, gsize wallet_n,
                                         const gchar **client_supported, gsize client_n,
                                         GError **error) {
  if (!client_pub_hex) {
    g_set_error(error, NOSTR_NWC_WALLET_ERROR_QUARK, 1, "client_pub_hex is NULL");
    return NULL;
  }
  NostrNwcWalletSession *s = g_new0(NostrNwcWalletSession, 1);
  if (nostr_nwc_wallet_session_init(s, client_pub_hex,
                                    (const char **)wallet_supported, (size_t)wallet_n,
                                    (const char **)client_supported, (size_t)client_n) != 0) {
    g_set_error(error, NOSTR_NWC_WALLET_ERROR_QUARK, 2, "encryption negotiation failed");
    g_free(s);
    return NULL;
  }
  return s;
}

void nostr_nwc_wallet_session_free_g(gpointer session) {
  if (!session) return;
  NostrNwcWalletSession *s = (NostrNwcWalletSession *)session;
  nostr_nwc_wallet_session_clear(s);
  g_free(s);
}

/* Build a successful response event */
gboolean nostr_nwc_wallet_build_response_g(gpointer session,
                                           const gchar *req_event_id,
                                           const gchar *result_type,
                                           const gchar *result_json,
                                           gchar **out_event_json,
                                           GError **error) {
  if (!session || !req_event_id || !result_type || !out_event_json) {
    g_set_error(error, NOSTR_NWC_WALLET_ERROR_QUARK, 3, "invalid arguments");
    return FALSE;
  }
  NostrNwcWalletSession *s = (NostrNwcWalletSession *)session;
  NostrNwcResponseBody body = {
    .result_type = (char*)result_type,
    .result_json = (char*)(result_json ? result_json : "{}"),
    .error_code = NULL,
    .error_message = NULL
  };
  char *json = NULL;
  if (nostr_nwc_wallet_build_response(s, req_event_id, &body, &json) != 0) {
    g_set_error(error, NOSTR_NWC_WALLET_ERROR_QUARK, 4, "build response failed");
    return FALSE;
  }
  *out_event_json = g_strdup(json);
  free(json);
  return TRUE;
}

/* Build an error response event */
gboolean nostr_nwc_wallet_build_error_response_g(gpointer session,
                                                  const gchar *req_event_id,
                                                  const gchar *error_code,
                                                  const gchar *error_message,
                                                  gchar **out_event_json,
                                                  GError **error) {
  if (!session || !req_event_id || !out_event_json) {
    g_set_error(error, NOSTR_NWC_WALLET_ERROR_QUARK, 5, "invalid arguments");
    return FALSE;
  }
  NostrNwcWalletSession *s = (NostrNwcWalletSession *)session;
  NostrNwcResponseBody body = {
    .result_type = NULL,
    .result_json = NULL,
    .error_code = (char*)(error_code ? error_code : "INTERNAL"),
    .error_message = (char*)(error_message ? error_message : "Unknown error")
  };
  char *json = NULL;
  if (nostr_nwc_wallet_build_response(s, req_event_id, &body, &json) != 0) {
    g_set_error(error, NOSTR_NWC_WALLET_ERROR_QUARK, 6, "build error response failed");
    return FALSE;
  }
  *out_event_json = g_strdup(json);
  free(json);
  return TRUE;
}

/* Encrypt plaintext using the session's negotiated encryption scheme */
gboolean nostr_nwc_wallet_encrypt_g(gpointer session,
                                    const gchar *wallet_sk_hex,
                                    const gchar *client_pub_hex,
                                    const gchar *plaintext,
                                    gchar **out_ciphertext,
                                    GError **error) {
  if (!session || !wallet_sk_hex || !client_pub_hex || !plaintext || !out_ciphertext) {
    g_set_error(error, NOSTR_NWC_WALLET_ERROR_QUARK, 7, "invalid arguments");
    return FALSE;
  }
  NostrNwcWalletSession *s = (NostrNwcWalletSession *)session;
  char *ciphertext = NULL;
  if (nostr_nwc_wallet_encrypt(s, wallet_sk_hex, client_pub_hex, plaintext, &ciphertext) != 0) {
    g_set_error(error, NOSTR_NWC_WALLET_ERROR_QUARK, 8, "encryption failed");
    return FALSE;
  }
  *out_ciphertext = g_strdup(ciphertext);
  free(ciphertext);
  return TRUE;
}

/* Decrypt ciphertext using the session's negotiated encryption scheme */
gboolean nostr_nwc_wallet_decrypt_g(gpointer session,
                                    const gchar *wallet_sk_hex,
                                    const gchar *client_pub_hex,
                                    const gchar *ciphertext,
                                    gchar **out_plaintext,
                                    GError **error) {
  if (!session || !wallet_sk_hex || !client_pub_hex || !ciphertext || !out_plaintext) {
    g_set_error(error, NOSTR_NWC_WALLET_ERROR_QUARK, 9, "invalid arguments");
    return FALSE;
  }
  NostrNwcWalletSession *s = (NostrNwcWalletSession *)session;
  char *plaintext = NULL;
  if (nostr_nwc_wallet_decrypt(s, wallet_sk_hex, client_pub_hex, ciphertext, &plaintext) != 0) {
    g_set_error(error, NOSTR_NWC_WALLET_ERROR_QUARK, 10, "decryption failed");
    return FALSE;
  }
  *out_plaintext = g_strdup(plaintext);
  free(plaintext);
  return TRUE;
}

/* Get the negotiated encryption type as a string */
const gchar *nostr_nwc_wallet_get_encryption_g(gpointer session) {
  if (!session) return NULL;
  NostrNwcWalletSession *s = (NostrNwcWalletSession *)session;
  return (s->enc == NOSTR_NWC_ENC_NIP44_V2) ? "nip44-v2" : "nip04";
}

/* Get the client public key hex from the session */
const gchar *nostr_nwc_wallet_get_client_pub_g(gpointer session) {
  if (!session) return NULL;
  NostrNwcWalletSession *s = (NostrNwcWalletSession *)session;
  return s->client_pub_hex;
}
