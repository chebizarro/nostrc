#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip47/nwc_wallet.h"

int main(void) {
  const char *client_pub = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
  const char *client_supported[] = {"nip44-v2", "nip04"};
  const char *wallet_supported[] = {"nip04", "nip44-v2"};

  NostrNwcWalletSession s = {0};
  if (nostr_nwc_wallet_session_init(&s, client_pub,
                                    wallet_supported, 2,
                                    client_supported, 2) != 0) {
    fprintf(stderr, "failed to init wallet session\n");
    return 1;
  }

  printf("negotiated enc: %s\n", s.enc == NOSTR_NWC_ENC_NIP44_V2 ? "nip44-v2" : "nip04");

  const char *req_event_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  NostrNwcResponseBody resp = { .result_type = (char*)"get_balance", .result_json = (char*)"{\"balance\":123}" };
  char *event_json = NULL;
  if (nostr_nwc_wallet_build_response(&s, req_event_id, &resp, &event_json) != 0) {
    fprintf(stderr, "build response failed\n");
    nostr_nwc_wallet_session_clear(&s);
    return 1;
  }

  printf("response event: %s\n", event_json);

  free(event_json);
  nostr_nwc_wallet_session_clear(&s);
  return 0;
}
