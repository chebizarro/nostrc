#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip47/nwc_wallet.h"
#include "nostr/nip47/nwc_client.h"
#include "keys.h"

int main(void) {
  // Generate demo keypairs
  char *wallet_sk = nostr_key_generate_private();
  char *wallet_pk = nostr_key_get_public(wallet_sk);
  char *client_sk = nostr_key_generate_private();
  char *client_pub = nostr_key_get_public(client_sk);
  if(!wallet_sk||!wallet_pk||!client_sk||!client_pub){ fprintf(stderr, "keygen failed\n"); return 1; }
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

  // Demonstrate encrypt/decrypt helpers (wallet <-> client)
  NostrNwcClientSession cs = { .wallet_pub_hex = wallet_pk, .enc = s.enc };
  const char *msg = "hello from wallet";
  char *cipher = NULL, *plain = NULL;
  if (nostr_nwc_wallet_encrypt(&s, wallet_sk, client_pub, msg, &cipher) == 0) {
    printf("wallet->client cipher: %s\n", cipher);
    if (nostr_nwc_client_decrypt(&cs, client_sk, wallet_pk, cipher, &plain) == 0) {
      printf("wallet->client plain: %s\n", plain);
      free(plain);
    }
    free(cipher);
  }
  cipher = NULL; plain = NULL;
  if (nostr_nwc_client_encrypt(&cs, client_sk, wallet_pk, msg, &cipher) == 0) {
    printf("client->wallet cipher: %s\n", cipher);
    if (nostr_nwc_wallet_decrypt(&s, wallet_sk, client_pub, cipher, &plain) == 0) {
      printf("client->wallet plain: %s\n", plain);
      free(plain);
    }
    free(cipher);
  }

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
  free(wallet_sk); free(wallet_pk); free(client_sk); free(client_pub);
  return 0;
}
