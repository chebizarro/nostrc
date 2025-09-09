#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nostr/nip04.h"
#include "keys.h"

static void expect(int cond, const char *msg) {
  if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
}

int main(void) {
  const char *sender_sk = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *receiver_sk = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  char *receiver_pk = nostr_key_get_public_sec1_compressed(receiver_sk);
  expect(receiver_pk && strlen(receiver_pk)==66, "derive receiver_pk");

  // Force legacy AES-CBC encrypt
  setenv("NIP04_LEGACY_CBC", "1", 1);
  char *enc = NULL; char *err = NULL;
  int rc = nostr_nip04_encrypt("hello", receiver_pk, sender_sk, &enc, &err);
  expect(rc == 0 && enc && strstr(enc, "?iv=") != NULL, "legacy encrypt ?iv=");
  if (err) free(err);
  free(enc);

  // Default AEAD encrypt
  unsetenv("NIP04_LEGACY_CBC");
  enc = NULL; err = NULL;
  rc = nostr_nip04_encrypt("hello", receiver_pk, sender_sk, &enc, &err);
  expect(rc == 0 && enc && strncmp(enc, "v=2:", 4) == 0, "aead encrypt v2");
  if (err) free(err);
  free(enc);

  free(receiver_pk);
  fprintf(stdout, "OK\n");
  return 0;
}
