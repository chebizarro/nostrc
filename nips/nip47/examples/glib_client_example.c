#include <glib.h>
#include <stdio.h>
#include "nostr/nip47/nwc_client_g.h"

int main(void) {
  GError *err = NULL;
  const gchar *wallet_pub = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const gchar *client_supported[] = {"nip44-v2", "nip04"};
  const gchar *wallet_supported[] = {"nip04", "nip44-v2"};

  gpointer s = nostr_nwc_client_session_init_g(wallet_pub,
                                               client_supported, 2,
                                               wallet_supported, 2,
                                               &err);
  if (!s) {
    g_printerr("init failed: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    return 1;
  }

  gchar *event_json = NULL;
  if (!nostr_nwc_client_build_request_g(s, "get_balance", "{\"unit\":\"msat\"}", &event_json, &err)) {
    g_printerr("build failed: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    nostr_nwc_client_session_free_g(s);
    return 1;
  }

  g_print("glib request event: %s\n", event_json);
  g_free(event_json);
  nostr_nwc_client_session_free_g(s);
  return 0;
}
