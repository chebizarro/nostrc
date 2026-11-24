/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_TEST_CONNECT_ADMISSION_H
#define SIGNET_TEST_CONNECT_ADMISSION_H

#include "../src/key_store_private.h"
#include "signet/store.h"
#include "test_check.h"
#include <glib.h>

/* These fixtures test pairing state, independently of NIP-46 policy. */
static inline bool test_allow_connect(const char *agent_id, const char *pubkey,
                                      void *data) {
  (void)agent_id;
  (void)pubkey;
  (void)data;
  return true;
}

static inline int test_admit_client(SignetKeyStore *ks, const char *secret,
                                    const char *client, int64_t now,
                                    char **agent) {
  return signet_key_store_admit_connect_client(ks, secret, client, now,
                                               test_allow_connect, NULL, agent);
}

static inline int test_admit_store_client(SignetStore *store, const char *secret,
                                          const char *client, int64_t now,
                                          char **agent) {
  return signet_store_admit_connect_client(store, secret, client, now,
                                            test_allow_connect, NULL, agent);
}

static inline void test_pair_store_client(SignetStore *store, const char *id,
                                          const char *secret, const char *client,
                                          int64_t now) {
  char *agent = NULL;
  CHECK(test_admit_store_client(store, secret, client, now, &agent) == 0);
  CHECK(g_strcmp0(agent, id) == 0);
  g_free(agent);
}

/* Use the same binding pin passed to custody by the NIP-46 handler. */
static inline int test_crypt_bound_client(SignetKeyStore *ks, const char *id,
                                          const char *client, const char *method,
                                          const char *peer, const char *input,
                                          char **result) {
  g_autofree char *agent = NULL;
  g_autofree char *hash = NULL;
  CHECK(signet_store_lookup_client_binding(signet_key_store_get_store(ks),
                                             client, 0, &agent, &hash) == 0);
  CHECK(g_strcmp0(agent, id) == 0);
  return signet_key_store_crypt_bound(ks, id, client, hash, 0, method,
                                       peer, input, result);
}

#endif
