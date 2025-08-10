#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include "nostr/nip44/nip44.h"

static void hex_to_bytes(const char *hex, unsigned char *out, size_t outlen) {
  for (size_t i = 0; i < outlen; i++) {
    unsigned int byte;
    sscanf(hex + 2*i, "%2x", &byte);
    out[i] = (unsigned char)byte;
  }
}

static void test_case(const unsigned char *msg, size_t msg_len) {
  /* Use deterministic keys: sk=1 and pk_xonly = x(G) */
  unsigned char sender_sk[32] = {0}; sender_sk[31] = 1; /* 0x...01 */
  const char *g_x_hex = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  unsigned char receiver_pk_xonly[32];
  hex_to_bytes(g_x_hex, receiver_pk_xonly, 32);

  char *b64 = NULL;
  int rc = nostr_nip44_encrypt_v2(sender_sk, receiver_pk_xonly, msg, msg_len, &b64);
  assert(rc == 0 && b64);

  /* Decrypt with inverse keys: receiver_sk=1 and sender_pk_xonly=Gx */
  unsigned char receiver_sk[32] = {0}; receiver_sk[31] = 1;
  unsigned char sender_pk_xonly[32];
  memcpy(sender_pk_xonly, receiver_pk_xonly, 32);

  unsigned char *plain = NULL; size_t plain_len = 0;
  rc = nostr_nip44_decrypt_v2(receiver_sk, sender_pk_xonly, b64, &plain, &plain_len);
  assert(rc == 0);
  assert(plain_len == msg_len);
  assert(memcmp(plain, msg, msg_len) == 0);

  free(plain);

  /* Tamper MAC: flip last base64 char; expect failure */
  size_t blen = strlen(b64);
  char saved = b64[blen - 1];
  b64[blen - 1] = (saved == 'A') ? 'B' : 'A';
  rc = nostr_nip44_decrypt_v2(receiver_sk, sender_pk_xonly, b64, &plain, &plain_len);
  assert(rc != 0);
  free(b64);
}

int main(void) {
  srand((unsigned)time(NULL));

  /* Edge sizes (len=0 invalid per spec/vectors) */

  const unsigned char one[] = {0x42};
  test_case(one, sizeof(one));

  unsigned char m31[31]; for (int i=0;i<31;i++) m31[i]=(unsigned char)i; test_case(m31, sizeof(m31));
  unsigned char m32[32]; for (int i=0;i<32;i++) m32[i]=(unsigned char)(i+1); test_case(m32, sizeof(m32));
  unsigned char m33[33]; for (int i=0;i<33;i++) m33[i]=(unsigned char)(i+2); test_case(m33, sizeof(m33));

  /* Random message */
  unsigned char rnd[123]; for (int i=0;i<123;i++) rnd[i]=(unsigned char)(rand() & 0xFF);
  test_case(rnd, sizeof(rnd));

  /* Maximum allowed length 65535 */
  unsigned char *maxmsg = (unsigned char*)malloc(65535);
  assert(maxmsg);
  for (size_t i=0;i<65535;i++) maxmsg[i] = (unsigned char)(i & 0xFF);
  test_case(maxmsg, 65535);
  free(maxmsg);

  printf("test_nip44_roundtrip: OK\n");
  return 0;
}
