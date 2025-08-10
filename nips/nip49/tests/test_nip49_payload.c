#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "nostr/nip49/nip49.h"

int main(void) {
  NostrNip49Payload p; memset(&p, 0, sizeof p);
  p.version = 0x02; p.log_n = 16; p.ad = 0x01;
  for (int i=0;i<16;i++) p.salt[i] = (uint8_t)i;
  for (int i=0;i<24;i++) p.nonce[i] = (uint8_t)(0xA0+i);
  for (int i=0;i<48;i++) p.ciphertext[i] = (uint8_t)(0xF0+i);

  uint8_t buf[91];
  assert(nostr_nip49_payload_serialize(&p, buf) == 0);
  assert(buf[0] == 0x02);
  assert(buf[1] == 16);
  assert(buf[42] == 0x01);

  NostrNip49Payload q; memset(&q, 0, sizeof q);
  assert(nostr_nip49_payload_deserialize(buf, &q) == 0);
  assert(q.version == 0x02);
  assert(q.log_n == 16);
  assert(q.ad == 0x01);
  assert(memcmp(q.salt, p.salt, 16) == 0);
  assert(memcmp(q.nonce, p.nonce, 24) == 0);
  assert(memcmp(q.ciphertext, p.ciphertext, 48) == 0);
  printf("ok\n");
  return 0;
}
