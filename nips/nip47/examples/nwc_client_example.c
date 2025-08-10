#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip47/nwc_client.h"
#include "nostr/nip47/nwc_wallet.h"
#include "keys.h"

int main(void) {
  // Generate demo keypairs
  char *client_sk = nostr_key_generate_private();
  char *client_pk = nostr_key_get_public(client_sk);
  char *wallet_sk = nostr_key_generate_private();
  char *wallet_pub = nostr_key_get_public(wallet_sk);
  if(!client_sk||!client_pk||!wallet_sk||!wallet_pub){ fprintf(stderr, "keygen failed\n"); return 1; }
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

  // Demonstrate encrypt/decrypt helpers (client <-> wallet)
  NostrNwcWalletSession ws = { .client_pub_hex = client_pk, .enc = s.enc };
  const char *msg = "hello from client";
  char *cipher = NULL, *plain = NULL;
  if (nostr_nwc_client_encrypt(&s, client_sk, wallet_pub, msg, &cipher) == 0) {
    printf("client->wallet cipher: %s\n", cipher);
    if (nostr_nwc_wallet_decrypt(&ws, wallet_sk, client_pk, cipher, &plain) == 0) {
      printf("client->wallet plain: %s\n", plain);
      free(plain);
    }
    free(cipher);
  }
  cipher = NULL; plain = NULL;
  if (nostr_nwc_wallet_encrypt(&ws, wallet_sk, client_pk, msg, &cipher) == 0) {
    printf("wallet->client cipher: %s\n", cipher);
    if (nostr_nwc_client_decrypt(&s, client_sk, wallet_pub, cipher, &plain) == 0) {
      printf("wallet->client plain: %s\n", plain);
      free(plain);
    }
    free(cipher);
  }

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
  free(client_sk); free(client_pk); free(wallet_sk); free(wallet_pub);
  return 0;
}
