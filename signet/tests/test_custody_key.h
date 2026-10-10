/* Test-only adapter: copy a key inside a custody transaction to exercise
 * historical crypto vectors. Production transports must never do this. */
#ifndef SIGNET_TEST_CUSTODY_KEY_H
#define SIGNET_TEST_CUSTODY_KEY_H
#include "signet/key_store.h"
#include <sodium.h>
#include <string.h>

static int test_copy_key_in_custody(const uint8_t key[32], void *data) {
  SignetLoadedKey *out = data;
  out->secret_key = sodium_malloc(32);
  if (!out->secret_key) return -1;
  memcpy(out->secret_key, key, 32);
  out->secret_key_len = 32;
  return 0;
}

static bool test_load_key_via_custody(SignetKeyStore *ks, const char *agent_id,
                                      SignetLoadedKey *out) {
  memset(out, 0, sizeof(*out));
  return signet_key_store_with_signing_key(ks, agent_id, NULL,
                                          test_copy_key_in_custody, out) == 0;
}
#endif
