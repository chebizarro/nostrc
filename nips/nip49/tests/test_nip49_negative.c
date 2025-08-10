#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nostr/nip49/nip49.h"

static void fill_key(uint8_t sk[32]) {
  for (int i=0;i<32;i++) sk[i] = (uint8_t)(i);
}

static char *dupstr(const char *s) {
  size_t n = strlen(s);
  char *d = (char*)malloc(n+1);
  memcpy(d, s, n+1);
  return d;
}

int main(void) {
  // wrong password should fail to decrypt
  uint8_t sk[32]; fill_key(sk);
  char *enc = NULL;
  int rc = nostr_nip49_encrypt(sk, NOSTR_NIP49_SECURITY_SECURE, "pw", 16, &enc);
  assert(rc == 0 && enc);
  uint8_t out_sk[32];
  rc = nostr_nip49_decrypt(enc, "wrong", out_sk, NULL, NULL);
  assert(rc != 0);

  // tamper HRP should fail bech32 decode or HRP check
  char *bad = dupstr(enc);
  // change prefix ncryptsec -> ncryptsed
  for (int i=0;i<9 && bad[i]; i++) {
    if (bad[i] == 'c') { bad[i] = 'd'; break; }
  }
  rc = nostr_nip49_decrypt(bad, "pw", out_sk, NULL, NULL);
  assert(rc != 0);
  free(bad);

  // payload version mismatch should fail deserialize
  NostrNip49Payload p; memset(&p, 0, sizeof p);
  p.version = 0x01; p.log_n = 16; p.ad = 1;
  uint8_t buf[91];
  rc = nostr_nip49_payload_serialize(&p, buf);
  assert(rc == 0);
  NostrNip49Payload q; memset(&q, 0, sizeof q);
  rc = nostr_nip49_payload_deserialize(buf, &q);
  assert(rc != 0);

  free(enc);
  printf("nip49 negative: ok\n");
  return 0;
}
