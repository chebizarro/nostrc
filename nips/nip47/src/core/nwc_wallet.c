#include "nostr/nip47/nwc_wallet.h"
#include <stdlib.h>
#include <string.h>

int nostr_nwc_wallet_session_init(NostrNwcWalletSession *s,
                                  const char *client_pub_hex,
                                  const char **wallet_supported, size_t wallet_n,
                                  const char **client_supported, size_t client_n) {
  if (!s || !client_pub_hex) return -1;
  memset(s, 0, sizeof(*s));
  NostrNwcEncryption enc = 0;
  if (nostr_nwc_select_encryption(client_supported, client_n, wallet_supported, wallet_n, &enc) != 0) {
    return -1;
  }
  s->client_pub_hex = strdup(client_pub_hex);
  if (!s->client_pub_hex) { memset(s, 0, sizeof(*s)); return -1; }
  s->enc = enc;
  return 0;
}

void nostr_nwc_wallet_session_clear(NostrNwcWalletSession *s) {
  if (!s) return;
  free(s->client_pub_hex);
  memset(s, 0, sizeof(*s));
}

int nostr_nwc_wallet_build_response(const NostrNwcWalletSession *s,
                                    const char *req_event_id,
                                    const NostrNwcResponseBody *body,
                                    char **out_event_json) {
  if (!s || !s->client_pub_hex) return -1;
  return nostr_nwc_response_build(s->client_pub_hex, req_event_id, s->enc, body, out_event_json);
}
