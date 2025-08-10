#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nostr/nip49/nip49.h"

static void fill_key(uint8_t sk[32], uint8_t base) {
  for (int i=0;i<32;i++) sk[i] = (uint8_t)(base + i);
}

int main(void) {
  const char *pw = "testpw"; // ASCII => allowed in core without GLib
  uint8_t log_ns[] = { 16, 18, 20 };
  NostrNip49SecurityByte secs[] = {
    NOSTR_NIP49_SECURITY_INSECURE,
    NOSTR_NIP49_SECURITY_SECURE,
    NOSTR_NIP49_SECURITY_UNKNOWN
  };

  for (size_t i=0;i<sizeof(log_ns);i++) {
    for (size_t j=0;j<sizeof(secs)/sizeof(secs[0]);j++) {
      uint8_t sk[32]; fill_key(sk, (uint8_t)(0x10 + i*3 + j));
      char *enc = NULL;
      int rc = nostr_nip49_encrypt(sk, secs[j], pw, log_ns[i], &enc);
      assert(rc == 0 && enc);

      uint8_t out_sk[32]; NostrNip49SecurityByte out_sec = 0; uint8_t out_ln = 0;
      rc = nostr_nip49_decrypt(enc, pw, out_sk, &out_sec, &out_ln);
      assert(rc == 0);
      assert(memcmp(sk, out_sk, 32) == 0);
      assert(out_sec == secs[j]);
      assert(out_ln == log_ns[i]);
      free(enc);
    }
  }
  printf("nip49 roundtrip: ok\n");
  return 0;
}
