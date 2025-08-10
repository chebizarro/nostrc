#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nostr/nip44/nip44.h"

static void hex_to_bytes(const char *hex, unsigned char *out, size_t outlen) {
  for (size_t i = 0; i < outlen; i++) {
    unsigned int byte; sscanf(hex + 2*i, "%2x", &byte); out[i] = (unsigned char)byte;
  }
}

int main(void) {
  unsigned char sk[32] = {0}; sk[31] = 1;
  const char *g_x_hex = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  unsigned char pkx[32]; hex_to_bytes(g_x_hex, pkx, 32);
  unsigned char conv[32]; memset(conv, 0, sizeof conv);
  int rc = nostr_nip44_convkey(sk, pkx, conv);
  assert(rc == 0);
  /* Ensure convkey is not all-zero */
  int allzero = 1; for (int i=0;i<32;i++) if (conv[i]) { allzero = 0; break; }
  assert(!allzero);
  printf("test_nip44_convkey: OK\n");
  return 0;
}
