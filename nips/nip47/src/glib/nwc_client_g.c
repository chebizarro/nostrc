/* GLib wrappers for NIP-47 client */
#include <glib.h>
#include "nostr/nip47/nwc_client.h"

/* Create a client session. Returns allocated pointer or NULL on error. */
gpointer nostr_nwc_client_session_init_g(const gchar *wallet_pub_hex,
                                         const gchar **client_supported, gsize client_n,
                                         const gchar **wallet_supported, gsize wallet_n,
                                         GError **error) {
  if (!wallet_pub_hex) {
    g_set_error(error, g_quark_from_static_string("nwc"), 1, "wallet_pub_hex is NULL");
    return NULL;
  }
  NostrNwcClientSession *s = g_new0(NostrNwcClientSession, 1);
  if (nostr_nwc_client_session_init(s, wallet_pub_hex,
                                    (const char **)client_supported, (size_t)client_n,
                                    (const char **)wallet_supported, (size_t)wallet_n) != 0) {
    g_set_error(error, g_quark_from_static_string("nwc"), 2, "negotiation failed");
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
    g_set_error(error, g_quark_from_static_string("nwc"), 3, "invalid arguments");
    return FALSE;
  }
  NostrNwcClientSession *s = (NostrNwcClientSession *)session;
  NostrNwcRequestBody body = { .method = (char*)method, .params_json = (char*)(params_json ? params_json : "{}") };
  char *json = NULL;
  if (nostr_nwc_client_build_request(s, &body, &json) != 0) {
    g_set_error(error, g_quark_from_static_string("nwc"), 4, "build request failed");
    return FALSE;
  }
  *out_event_json = g_strdup(json);
  free(json);
  return TRUE;
}
