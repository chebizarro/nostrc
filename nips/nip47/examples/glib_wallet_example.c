#include <glib.h>
#include <stdio.h>
#include "nostr/nip47/nwc_wallet_g.h"

int main(void) {
  GError *err = NULL;
  const gchar *client_pub = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
  const gchar *client_supported[] = {"nip44-v2", "nip04"};
  const gchar *wallet_supported[] = {"nip04", "nip44-v2"};

  gpointer s = nostr_nwc_wallet_session_init_g(client_pub,
                                               wallet_supported, 2,
                                               client_supported, 2,
                                               &err);
  if (!s) {
    g_printerr("init failed: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    return 1;
  }

  const gchar *req_event_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  gchar *event_json = NULL;
  if (!nostr_nwc_wallet_build_response_g(s, req_event_id, "get_balance", "{\"balance\":42}", &event_json, &err)) {
    g_printerr("build failed: %s\n", err ? err->message : "unknown");
    if (err) g_error_free(err);
    nostr_nwc_wallet_session_free_g(s);
    return 1;
  }

  g_print("glib response event: %s\n", event_json);
  g_free(event_json);
  nostr_nwc_wallet_session_free_g(s);
  return 0;
}
