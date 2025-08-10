#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip47/nwc.h"
#include "nostr/nip47/nwc_client.h"
#include "nostr/nip47/nwc_wallet.h"
#include "keys.h"

static void expect(int cond, const char *msg){ if(!cond){ fprintf(stderr, "FAIL: %s\n", msg); exit(1);} }

static void free2(char *a, char *b){ if(a) free(a); if(b) free(b); }

static void test_roundtrip(NostrNwcEncryption enc){
  // Generate client and wallet keys
  char *client_sk = nostr_key_generate_private();
  char *wallet_sk = nostr_key_generate_private();
  expect(client_sk && wallet_sk, "keygen");
  char *client_pk = nostr_key_get_public(client_sk);
  char *wallet_pk = nostr_key_get_public(wallet_sk);
  expect(client_pk && wallet_pk, "pub derive");

  // Sessions (init to mirror production)
  NostrNwcClientSession cs = {0};
  NostrNwcWalletSession ws = {0};
  const char *client_supported[] = {"nip44-v2", "nip04"};
  const char *wallet_supported[] = {"nip04", "nip44-v2"};
  int rc = nostr_nwc_client_session_init(&cs, wallet_pk,
                                         client_supported, 2,
                                         wallet_supported, 2);
  expect(rc==0, "client session init");
  // Force desired enc for this test call
  cs.enc = enc;

  rc = nostr_nwc_wallet_session_init(&ws, client_pk,
                                     wallet_supported, 2,
                                     client_supported, 2);
  expect(rc==0, "wallet session init");
  ws.enc = enc;

  const char *msg = "hello nip47";

  // client -> wallet
  fprintf(stdout, "enc=%d client->wallet pklen=%zu sklen=%zu\n", (int)enc, strlen(wallet_pk), strlen(client_sk));
  char *c2w = NULL; rc = nostr_nwc_client_encrypt(&cs, client_sk, wallet_pk, msg, &c2w);
  expect(rc==0 && c2w, "client encrypt");
  char *plain = NULL; rc = nostr_nwc_wallet_decrypt(&ws, wallet_sk, client_pk, c2w, &plain);
  expect(rc==0 && plain, "wallet decrypt");
  expect(strcmp(plain, msg)==0, "roundtrip client->wallet");
  free(plain); free(c2w); plain = NULL; c2w = NULL;

  // wallet -> client
  char *w2c = NULL; rc = nostr_nwc_wallet_encrypt(&ws, wallet_sk, client_pk, msg, &w2c);
  expect(rc==0 && w2c, "wallet encrypt");
  rc = nostr_nwc_client_decrypt(&cs, client_sk, wallet_pk, w2c, &plain);
  expect(rc==0 && plain, "client decrypt");
  expect(strcmp(plain, msg)==0, "roundtrip wallet->client");
  free(plain); free(w2c);

  // compressed SEC1 input accepted for NIP-44 path
  if (enc == NOSTR_NWC_ENC_NIP44_V2){
    size_t xlen = strlen(wallet_pk);
    char *sec1 = (char*)malloc(xlen + 2 + 1);
    expect(sec1 != NULL, "sec1 alloc");
    sec1[0]='0'; sec1[1]='2'; memcpy(sec1+2, wallet_pk, xlen+1);
    rc = nostr_nwc_client_encrypt(&cs, client_sk, sec1, msg, &c2w);
    expect(rc==0 && c2w, "client encrypt SEC1");
    free(c2w); free(sec1);
  }

  free2(client_sk, wallet_sk);
  free2(client_pk, wallet_pk);
  nostr_nwc_client_session_clear(&cs);
  nostr_nwc_wallet_session_clear(&ws);
}

int main(void){
  test_roundtrip(NOSTR_NWC_ENC_NIP44_V2);
  test_roundtrip(NOSTR_NWC_ENC_NIP04);
  fprintf(stdout, "test_nwc_crypto: OK\n");
  return 0;
}
