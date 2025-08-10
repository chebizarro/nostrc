#include "nostr/nip47/nwc_client.h"
#include <stdlib.h>
#include <string.h>

int nostr_nwc_client_session_init(NostrNwcClientSession *s,
                                  const char *wallet_pub_hex,
                                  const char **client_supported, size_t client_n,
                                  const char **wallet_supported, size_t wallet_n) {
  if (!s || !wallet_pub_hex) return -1;
  memset(s, 0, sizeof(*s));
  NostrNwcEncryption enc = 0;
  if (nostr_nwc_select_encryption(client_supported, client_n, wallet_supported, wallet_n, &enc) != 0) {
    return -1;
  }
  s->wallet_pub_hex = strdup(wallet_pub_hex);
  if (!s->wallet_pub_hex) { memset(s, 0, sizeof(*s)); return -1; }
  s->enc = enc;
  return 0;
}

void nostr_nwc_client_session_clear(NostrNwcClientSession *s) {
  if (!s) return;
  free(s->wallet_pub_hex);
  memset(s, 0, sizeof(*s));
}

int nostr_nwc_client_build_request(const NostrNwcClientSession *s,
                                   const NostrNwcRequestBody *body,
                                   char **out_event_json) {
  if (!s || !s->wallet_pub_hex) return -1;
  return nostr_nwc_request_build(s->wallet_pub_hex, s->enc, body, out_event_json);
}
