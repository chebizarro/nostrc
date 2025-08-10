/* GLib wrapper stubs for NIP-47 wallet - TODO: implement GObject API */

#include <glib.h>
#include "nostr/nip47/nwc_wallet.h"

gpointer nostr_nwc_wallet_session_init_g(const gchar *client_pub_hex,
                                         const gchar **wallet_supported, gsize wallet_n,
                                         const gchar **client_supported, gsize client_n,
                                         GError **error) {
  if (!client_pub_hex) {
    g_set_error(error, g_quark_from_static_string("nwc"), 1, "client_pub_hex is NULL");
    return NULL;
  }
  NostrNwcWalletSession *s = g_new0(NostrNwcWalletSession, 1);
  if (nostr_nwc_wallet_session_init(s, client_pub_hex,
                                    (const char **)wallet_supported, (size_t)wallet_n,
                                    (const char **)client_supported, (size_t)client_n) != 0) {
    g_set_error(error, g_quark_from_static_string("nwc"), 2, "negotiation failed");
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

gboolean nostr_nwc_wallet_build_response_g(gpointer session,
                                           const gchar *req_event_id,
                                           const gchar *result_type,
                                           const gchar *result_json,
                                           gchar **out_event_json,
                                           GError **error) {
  if (!session || !req_event_id || !result_type || !out_event_json) {
    g_set_error(error, g_quark_from_static_string("nwc"), 3, "invalid arguments");
    return FALSE;
  }
  NostrNwcWalletSession *s = (NostrNwcWalletSession *)session;
  NostrNwcResponseBody body = { .result_type = (char*)result_type, .result_json = (char*)(result_json ? result_json : "{}") };
  char *json = NULL;
  if (nostr_nwc_wallet_build_response(s, req_event_id, &body, &json) != 0) {
    g_set_error(error, g_quark_from_static_string("nwc"), 4, "build response failed");
    return FALSE;
  }
  *out_event_json = g_strdup(json);
  free(json);
  return TRUE;
}
