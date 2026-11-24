/* SPDX-License-Identifier: MIT */
#include "signet/policy_store.h"
#include "signet/revocation.h"
#include "signet/store.h"
#include "signet/store_secrets.h"
#include "test_deny_list_fixture.h"
#include "test_check.h"

#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
  int64_t now = (int64_t)time(NULL);
  if (argc == 5 && strcmp(argv[1], "set-policy") == 0) {
    /* Same writer as management SET_POLICY, in a separate process so the
     * running daemon has to reload the durable policy. */
    SignetPolicyStore *policy = signet_policy_store_file_new(argv[2]);
    CHECK(policy);
    char *error = NULL;
    CHECK(signet_policy_store_set_identity_json(policy, argv[3], argv[4], now, &error) == 0);
    CHECK(!error);
    signet_policy_store_free(policy);
    return 0;
  }

  CHECK(argc == 4);
  CHECK(sodium_init() >= 0);
  SignetStoreConfig cfg = { .db_path = argv[1], .master_key = getenv("SIGNET_DB_KEY") };
  SignetStore *store = signet_store_open(&cfg);
  CHECK(store);
  const char *agent = argv[2];
  const char *pubkey = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
  const uint8_t sk[32] = { [31] = 1 };
  CHECK(signet_store_put_agent_ex(store, agent, sk, sizeof(sk), "fixture-connect",
                                  pubkey, "provisioned", now) == 0);
  SignetSecretMetadata meta = {0};
  CHECK(signet_store_create_secret(store, agent, SIGNET_SECRET_API_TOKEN,
                                   "fixture-token", (const uint8_t *)"token", 5,
                                   NULL, 0, "created", NULL, now, &meta) == SIGNET_SECRET_OK);
  puts(meta.id);
  signet_secret_metadata_clear(&meta);
  if (strcmp(argv[3], "deny") == 0) {
    signet_deny_list_free(test_set_denied(store, NULL, pubkey, agent, now, true));
  }
  signet_store_close(store);
  return 0;
}
