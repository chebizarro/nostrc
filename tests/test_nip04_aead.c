#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nostr/nip04.h"
#include "keys.h"

static void expect(int cond, const char *msg) {
  if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
}

int main(void) {
  const char *sender_sk = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; // 0xaa * 32
  const char *receiver_sk = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"; // 0xbb * 32
  char *receiver_pk = nostr_key_get_public_sec1_compressed(receiver_sk);
  char *sender_pk   = nostr_key_get_public_sec1_compressed(sender_sk);
  expect(receiver_pk && strlen(receiver_pk)==66, "derive receiver_pk");
  expect(sender_pk && strlen(sender_pk)==66, "derive sender_pk");

  // Encrypt v2
  char *enc = NULL; char *err = NULL;
  int rc = nostr_nip04_encrypt("hello", receiver_pk, sender_sk, &enc, &err);
  expect(rc == 0 && enc && strncmp(enc, "v=2:", 4) == 0, "encrypt v2 ok");
  if (err) free(err);

  // Decrypt v2
  char *pt = NULL; err = NULL;
  rc = nostr_nip04_decrypt(enc, sender_pk, receiver_sk, &pt, &err);
  expect(rc == 0 && pt && strcmp(pt, "hello") == 0, "decrypt v2 ok");
  free(pt);

  // Tamper last byte (affects tag section
  size_t elen = strlen(enc);
  enc[elen-1] = (enc[elen-1] == 'A') ? 'B' : 'A';
  pt = NULL; err = NULL;
  rc = nostr_nip04_decrypt(enc, sender_pk, receiver_sk, &pt, &err);
  expect(rc != 0 && pt == NULL && err && strcmp(err, "decrypt failed") == 0, "tamper fails with unified msg");
  if (err) free(err);

  free(enc);
  free(receiver_pk);
  free(sender_pk);
  fprintf(stdout, "OK\n");
  return 0;
}
