#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip47/nwc_client.h"

int main(void) {
  const char *wallet_pub = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const char *client_supported[] = {"nip44-v2", "nip04"};
  const char *wallet_supported[] = {"nip04", "nip44-v2"};

  NostrNwcClientSession s = {0};
  if (nostr_nwc_client_session_init(&s, wallet_pub,
                                    client_supported, 2,
                                    wallet_supported, 2) != 0) {
    fprintf(stderr, "failed to init client session\n");
    return 1;
  }

  printf("negotiated enc: %s\n", s.enc == NOSTR_NWC_ENC_NIP44_V2 ? "nip44-v2" : "nip04");

  NostrNwcRequestBody req = { .method = (char*)"get_balance", .params_json = (char*)"{\"unit\":\"sat\"}" };
  char *event_json = NULL;
  if (nostr_nwc_client_build_request(&s, &req, &event_json) != 0) {
    fprintf(stderr, "build request failed\n");
    nostr_nwc_client_session_clear(&s);
    return 1;
  }

  printf("request event: %s\n", event_json);

  free(event_json);
  nostr_nwc_client_session_clear(&s);
  return 0;
}
