/* GLib wrappers for NIP-47 client */
#include <glib.h>
#include <stdlib.h>
#include "nostr/nip47/nwc_client.h"
#include "nostr/nip47/nwc.h"
#include "nostr/nip47/nwc_envelope.h"
#include "nostr/nip47/nwc_info.h"

/* Error quark for NWC operations */
#define NOSTR_NWC_ERROR_QUARK g_quark_from_static_string("nostr-nwc-error")

/* Create a client session. Returns allocated pointer or NULL on error. */
gpointer nostr_nwc_client_session_init_g(const gchar *wallet_pub_hex,
                                         const gchar **client_supported, gsize client_n,
                                         const gchar **wallet_supported, gsize wallet_n,
                                         GError **error) {
  if (!wallet_pub_hex) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 1, "wallet_pub_hex is NULL");
    return NULL;
  }
  NostrNwcClientSession *s = g_new0(NostrNwcClientSession, 1);
  if (nostr_nwc_client_session_init(s, wallet_pub_hex,
                                    (const char **)client_supported, (size_t)client_n,
                                    (const char **)wallet_supported, (size_t)wallet_n) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 2, "encryption negotiation failed");
    g_free(s);
    return NULL;
  }
  return s;
}

void nostr_nwc_client_session_free_g(gpointer session) {
  if (!session) return;
  NostrNwcClientSession *s = (NostrNwcClientSession *)session;
  nostr_nwc_client_session_clear(s);
  g_free(s);
}

gboolean nostr_nwc_client_build_request_g(gpointer session,
                                          const gchar *method,
                                          const gchar *params_json,
                                          gchar **out_event_json,
                                          GError **error) {
  if (!session || !method || !out_event_json) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 3, "invalid arguments");
    return FALSE;
  }
  NostrNwcClientSession *s = (NostrNwcClientSession *)session;
  NostrNwcRequestBody body = { .method = (char*)method, .params_json = (char*)(params_json ? params_json : "{}") };
  char *json = NULL;
  if (nostr_nwc_client_build_request(s, &body, &json) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 4, "build request failed");
    return FALSE;
  }
  *out_event_json = g_strdup(json);
  free(json);
  return TRUE;
}

/* Encrypt plaintext using the session's negotiated encryption scheme */
gboolean nostr_nwc_client_encrypt_g(gpointer session,
                                    const gchar *client_sk_hex,
                                    const gchar *wallet_pub_hex,
                                    const gchar *plaintext,
                                    gchar **out_ciphertext,
                                    GError **error) {
  if (!session || !client_sk_hex || !wallet_pub_hex || !plaintext || !out_ciphertext) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 5, "invalid arguments");
    return FALSE;
  }
  NostrNwcClientSession *s = (NostrNwcClientSession *)session;
  char *ciphertext = NULL;
  if (nostr_nwc_client_encrypt(s, client_sk_hex, wallet_pub_hex, plaintext, &ciphertext) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 6, "encryption failed");
    return FALSE;
  }
  *out_ciphertext = g_strdup(ciphertext);
  free(ciphertext);
  return TRUE;
}

/* Decrypt ciphertext using the session's negotiated encryption scheme */
gboolean nostr_nwc_client_decrypt_g(gpointer session,
                                    const gchar *client_sk_hex,
                                    const gchar *wallet_pub_hex,
                                    const gchar *ciphertext,
                                    gchar **out_plaintext,
                                    GError **error) {
  if (!session || !client_sk_hex || !wallet_pub_hex || !ciphertext || !out_plaintext) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 7, "invalid arguments");
    return FALSE;
  }
  NostrNwcClientSession *s = (NostrNwcClientSession *)session;
  char *plaintext = NULL;
  if (nostr_nwc_client_decrypt(s, client_sk_hex, wallet_pub_hex, ciphertext, &plaintext) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 8, "decryption failed");
    return FALSE;
  }
  *out_plaintext = g_strdup(plaintext);
  free(plaintext);
  return TRUE;
}

/* Get the negotiated encryption type as a string */
const gchar *nostr_nwc_client_get_encryption_g(gpointer session) {
  if (!session) return NULL;
  NostrNwcClientSession *s = (NostrNwcClientSession *)session;
  return (s->enc == NOSTR_NWC_ENC_NIP44_V2) ? "nip44-v2" : "nip04";
}

/* Parse NWC URI into components */
gboolean nostr_nwc_uri_parse_g(const gchar *uri,
                               gchar **out_wallet_pubkey_hex,
                               gchar ***out_relays,
                               gchar **out_secret_hex,
                               gchar **out_lud16,
                               GError **error) {
  if (!uri) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 9, "uri is NULL");
    return FALSE;
  }
  NostrNwcConnection conn = {0};
  if (nostr_nwc_uri_parse(uri, &conn) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 10, "failed to parse NWC URI");
    return FALSE;
  }
  if (out_wallet_pubkey_hex) *out_wallet_pubkey_hex = g_strdup(conn.wallet_pubkey_hex);
  if (out_secret_hex) *out_secret_hex = g_strdup(conn.secret_hex);
  if (out_lud16) *out_lud16 = conn.lud16 ? g_strdup(conn.lud16) : NULL;
  if (out_relays && conn.relays) {
    gsize count = 0;
    for (gsize i = 0; conn.relays[i]; i++) count++;
    gchar **relays = g_new0(gchar *, count + 1);
    for (gsize i = 0; i < count; i++) {
      relays[i] = g_strdup(conn.relays[i]);
    }
    relays[count] = NULL;
    *out_relays = relays;
  } else if (out_relays) {
    *out_relays = NULL;
  }
  nostr_nwc_connection_clear(&conn);
  return TRUE;
}

/* Build NWC URI from components */
gboolean nostr_nwc_uri_build_g(const gchar *wallet_pubkey_hex,
                               const gchar *const *relays,
                               const gchar *secret_hex,
                               const gchar *lud16,
                               gchar **out_uri,
                               GError **error) {
  if (!wallet_pubkey_hex || !secret_hex || !out_uri) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 11, "invalid arguments");
    return FALSE;
  }
  NostrNwcConnection conn = {
    .wallet_pubkey_hex = (char *)wallet_pubkey_hex,
    .relays = (char **)relays,
    .secret_hex = (char *)secret_hex,
    .lud16 = (char *)lud16
  };
  char *uri = NULL;
  if (nostr_nwc_uri_build(&conn, &uri) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 12, "failed to build NWC URI");
    return FALSE;
  }
  *out_uri = g_strdup(uri);
  free(uri);
  return TRUE;
}

/* Build NIP-47 Info event JSON */
gboolean nostr_nwc_info_build_g(const gchar *pubkey,
                                gint64 created_at,
                                const gchar *const *methods,
                                gsize methods_count,
                                const gchar *const *encryptions,
                                gsize enc_count,
                                gboolean notifications,
                                gchar **out_event_json,
                                GError **error) {
  if (!methods || methods_count == 0 || !out_event_json) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 13, "invalid arguments");
    return FALSE;
  }
  char *json = NULL;
  if (nostr_nwc_info_build(pubkey, (long long)created_at,
                           (const char **)methods, (size_t)methods_count,
                           (const char **)encryptions, (size_t)enc_count,
                           notifications ? 1 : 0, &json) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 14, "failed to build Info event");
    return FALSE;
  }
  *out_event_json = g_strdup(json);
  free(json);
  return TRUE;
}

/* Parse NIP-47 Info event JSON */
gboolean nostr_nwc_info_parse_g(const gchar *event_json,
                                gchar ***out_methods,
                                gsize *out_methods_count,
                                gchar ***out_encryptions,
                                gsize *out_enc_count,
                                gboolean *out_notifications,
                                GError **error) {
  if (!event_json) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 15, "event_json is NULL");
    return FALSE;
  }
  char **methods = NULL;
  size_t methods_n = 0;
  char **encryptions = NULL;
  size_t enc_n = 0;
  int notifications = 0;
  if (nostr_nwc_info_parse(event_json, &methods, &methods_n, &encryptions, &enc_n, &notifications) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 16, "failed to parse Info event");
    return FALSE;
  }
  /* Convert to GLib-owned strings */
  if (out_methods && methods) {
    gchar **glib_methods = g_new0(gchar *, methods_n + 1);
    for (gsize i = 0; i < methods_n; i++) {
      glib_methods[i] = g_strdup(methods[i]);
      free(methods[i]);
    }
    glib_methods[methods_n] = NULL;
    free(methods);
    *out_methods = glib_methods;
  } else if (methods) {
    for (gsize i = 0; i < methods_n; i++) free(methods[i]);
    free(methods);
  }
  if (out_methods_count) *out_methods_count = methods_n;

  if (out_encryptions && encryptions) {
    gchar **glib_encs = g_new0(gchar *, enc_n + 1);
    for (gsize i = 0; i < enc_n; i++) {
      glib_encs[i] = g_strdup(encryptions[i]);
      free(encryptions[i]);
    }
    glib_encs[enc_n] = NULL;
    free(encryptions);
    *out_encryptions = glib_encs;
  } else if (encryptions) {
    for (gsize i = 0; i < enc_n; i++) free(encryptions[i]);
    free(encryptions);
  }
  if (out_enc_count) *out_enc_count = enc_n;
  if (out_notifications) *out_notifications = notifications ? TRUE : FALSE;
  return TRUE;
}

/* Parse NWC request event JSON */
gboolean nostr_nwc_request_parse_g(const gchar *event_json,
                                   gchar **out_wallet_pub_hex,
                                   gchar **out_encryption,
                                   gchar **out_method,
                                   gchar **out_params_json,
                                   GError **error) {
  if (!event_json) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 17, "event_json is NULL");
    return FALSE;
  }
  char *wallet_pub = NULL;
  NostrNwcEncryption enc = NOSTR_NWC_ENC_NIP44_V2;
  NostrNwcRequestBody body = {0};
  if (nostr_nwc_request_parse(event_json, &wallet_pub, &enc, &body) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 18, "failed to parse request event");
    return FALSE;
  }
  if (out_wallet_pub_hex) {
    *out_wallet_pub_hex = wallet_pub ? g_strdup(wallet_pub) : NULL;
  }
  if (wallet_pub) free(wallet_pub);
  if (out_encryption) {
    *out_encryption = g_strdup(enc == NOSTR_NWC_ENC_NIP44_V2 ? "nip44-v2" : "nip04");
  }
  if (out_method) {
    *out_method = body.method ? g_strdup(body.method) : NULL;
  }
  if (out_params_json) {
    *out_params_json = body.params_json ? g_strdup(body.params_json) : NULL;
  }
  nostr_nwc_request_body_clear(&body);
  return TRUE;
}

/* Parse NWC response event JSON */
gboolean nostr_nwc_response_parse_g(const gchar *event_json,
                                    gchar **out_client_pub_hex,
                                    gchar **out_req_event_id,
                                    gchar **out_encryption,
                                    gchar **out_result_type,
                                    gchar **out_result_json,
                                    gchar **out_error_code,
                                    gchar **out_error_message,
                                    GError **error) {
  if (!event_json) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 19, "event_json is NULL");
    return FALSE;
  }
  char *client_pub = NULL;
  char *req_id = NULL;
  NostrNwcEncryption enc = NOSTR_NWC_ENC_NIP44_V2;
  NostrNwcResponseBody body = {0};
  if (nostr_nwc_response_parse(event_json, &client_pub, &req_id, &enc, &body) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 20, "failed to parse response event");
    return FALSE;
  }
  if (out_client_pub_hex) {
    *out_client_pub_hex = client_pub ? g_strdup(client_pub) : NULL;
  }
  if (client_pub) free(client_pub);
  if (out_req_event_id) {
    *out_req_event_id = req_id ? g_strdup(req_id) : NULL;
  }
  if (req_id) free(req_id);
  if (out_encryption) {
    *out_encryption = g_strdup(enc == NOSTR_NWC_ENC_NIP44_V2 ? "nip44-v2" : "nip04");
  }
  if (out_result_type) {
    *out_result_type = body.result_type ? g_strdup(body.result_type) : NULL;
  }
  if (out_result_json) {
    *out_result_json = body.result_json ? g_strdup(body.result_json) : NULL;
  }
  if (out_error_code) {
    *out_error_code = body.error_code ? g_strdup(body.error_code) : NULL;
  }
  if (out_error_message) {
    *out_error_message = body.error_message ? g_strdup(body.error_message) : NULL;
  }
  nostr_nwc_response_body_clear(&body);
  return TRUE;
}

/* Helper to select encryption based on capabilities */
gboolean nostr_nwc_select_encryption_g(const gchar *const *client_supported, gsize client_n,
                                       const gchar *const *wallet_supported, gsize wallet_n,
                                       gchar **out_encryption,
                                       GError **error) {
  if (!out_encryption) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 21, "out_encryption is NULL");
    return FALSE;
  }
  NostrNwcEncryption enc = NOSTR_NWC_ENC_NIP44_V2;
  if (nostr_nwc_select_encryption((const char **)client_supported, (size_t)client_n,
                                   (const char **)wallet_supported, (size_t)wallet_n, &enc) != 0) {
    g_set_error(error, NOSTR_NWC_ERROR_QUARK, 22, "no common encryption method found");
    return FALSE;
  }
  *out_encryption = g_strdup(enc == NOSTR_NWC_ENC_NIP44_V2 ? "nip44-v2" : "nip04");
  return TRUE;
}
