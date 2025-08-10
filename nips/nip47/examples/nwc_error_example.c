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

  const char *req_event_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  NostrNwcResponseBody resp = {0};
  /* Simulate an error per NIP-47: error_code + error_message */
  resp.error_code = (char*)"INSUFFICIENT_BALANCE";
  resp.error_message = (char*)"Balance too low";

  char *event_json = NULL;
  if (nostr_nwc_wallet_build_response(&s, req_event_id, &resp, &event_json) != 0) {
    fprintf(stderr, "build error response failed\n");
    nostr_nwc_wallet_session_clear(&s);
    return 1;
  }

  printf("error response event: %s\n", event_json);

  free(event_json);
  nostr_nwc_wallet_session_clear(&s);
  return 0;
}
