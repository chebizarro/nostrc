/* Portable identity-core sanity check for the bucket B5 broker-test seed
 * (nostrc-zcll.6). accounts.pubkey_hex is UNIQUE in the schema, so seeding
 * two enrolled accounts requires two distinct pubkeys. A regression that
 * reused the same PUBKEY across n_local and n_both was caught here first —
 * this test reproduces the two-account seed shape without pulling in the
 * broker (Linux-only) or the vault (still portable), so it runs on macOS
 * and any host with sqlite + openssl. Label matches the other identity
 * tests: "nostr-homed;portable;identity".
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "../nh_test.h"
#include "nostr_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Two well-known xonly pubkeys (secp256k1 SK=1 and a nonzero distinct value).
 * We do not derive them here — this file only depends on identity_core to
 * stay portable and off the libnostr link path. The values do not need to
 * be valid secp256k1 pubkeys for the identity schema, which stores them as
 * plain hex text; any two 64-char lowercase hex strings differing at any
 * byte will exercise the UNIQUE constraint the same way. */
static const char *PUBKEY_A =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *PUBKEY_B =
    "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";

static nh_identity_ownership_result available(void *c, const char *n,
                                              uint32_t u, uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

static void set_config(nh_identity_config *config, const char *dir) {
  nh_identity_config_defaults(config);
  snprintf(config->authority_path, sizeof config->authority_path,
           "%s/authority.db", dir);
  snprintf(config->projection_path, sizeof config->projection_path,
           "%s/nss.db", dir);
  snprintf(config->home_root, sizeof config->home_root, "%s/home", dir);
}

/* Enroll+install `username` with `pubkey_hex`. `home_inode`, `op_id` and the
 * username must be unique per call to keep the store's UNIQUE constraints
 * happy. Returns the loaded account struct via *out. */
static void enroll(nh_identity_store *store, const char *username,
                   const char *pubkey_hex, const char *op_id,
                   uint64_t home_inode, nh_identity_account *out) {
  nh_identity_enroll_request req = {0};
  req.username = username;
  req.pubkey_hex = pubkey_hex;
  req.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  NH_CHECK(nh_identity_operation_begin_enroll(store, op_id, &req, &state) ==
           NH_IDENTITY_OK);
  nh_identity_home_evidence staged = {11, home_inode};
  nh_identity_home_evidence installed = {11, home_inode + 100};
  NH_CHECK(nh_identity_operation_advance_home(
               store, op_id, NH_IDENTITY_PHASE_RESERVED,
               NH_IDENTITY_PHASE_STAGED, &staged, &state) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_advance_home(
               store, op_id, NH_IDENTITY_PHASE_STAGED,
               NH_IDENTITY_PHASE_INSTALLED, &installed, &state) ==
           NH_IDENTITY_OK);
  NH_CHECK(nh_identity_store_lookup_by_name(store, username, out) ==
           NH_IDENTITY_OK);
}

/* Confirm the UNIQUE constraint bites when two accounts share a pubkey — the
 * failure mode the maintainer reported for the B5 seed regression. */
static void test_reused_pubkey_rejected(void) {
  char dir[] = "/tmp/nh-seed-reuse-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  static nh_identity_config config;
  set_config(&config, dir);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = NH_IDENTITY_STORE_CREATE;
  nh_identity_store *store = NULL;
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);

  nh_identity_account a1;
  enroll(store, "n_dup1", PUBKEY_A, "00000000-0000-4000-8000-0000000000a1",
         901, &a1);

  /* Second enroll with the SAME pubkey must fail with an identity_store rc
   * other than OK — this is the regression the B5 test seed used to hit. */
  nh_identity_enroll_request dup = {0};
  dup.username = "n_dup2";
  dup.pubkey_hex = PUBKEY_A;
  dup.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state ds;
  nh_identity_rc rc = nh_identity_operation_begin_enroll(
      store, "00000000-0000-4000-8000-0000000000a2", &dup, &ds);
  NH_CHECK(rc != NH_IDENTITY_OK);

  nh_identity_store_close(store);

  /* Cleanup */
  char path[1024];
  const char *files[] = {"authority.db", "authority.db-wal",
                         "authority.db-shm", "authority.lock", "nss.db"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i]);
    unlink(path);
  }
  rmdir(dir);
  printf("reused-pubkey rejected: rc=%s\n", nh_identity_rc_name(rc));
}

/* Confirm the two-account seed shape the B5 broker test relies on works
 * end-to-end when the pubkeys are distinct. */
static void test_two_accounts_seed_ok(void) {
  char dir[] = "/tmp/nh-seed-ok-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  static nh_identity_config config;
  set_config(&config, dir);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = NH_IDENTITY_STORE_CREATE;
  nh_identity_store *store = NULL;
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);

  nh_identity_account a_local;
  enroll(store, "n_local", PUBKEY_A, "00000000-0000-4000-8000-000000000001",
         101, &a_local);

  nh_identity_account a_both;
  enroll(store, "n_both", PUBKEY_B, "00000000-0000-4000-8000-000000000010",
         201, &a_both);

  /* Each account must have its OWN account_id, pubkey and key_generation. */
  NH_CHECK(strcmp(a_local.account_id, a_both.account_id) != 0);
  NH_CHECK(strcmp(a_local.pubkey_hex, a_both.pubkey_hex) != 0);
  NH_CHECK(!strcmp(a_local.pubkey_hex, PUBKEY_A));
  NH_CHECK(!strcmp(a_both.pubkey_hex, PUBKEY_B));
  NH_CHECK(a_local.uid != a_both.uid);

  nh_identity_store_close(store);

  char path[1024];
  const char *files[] = {"authority.db", "authority.db-wal",
                         "authority.db-shm", "authority.lock", "nss.db"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i]);
    unlink(path);
  }
  rmdir(dir);
  printf("two-account seed OK: %s vs %s\n", a_local.pubkey_hex,
         a_both.pubkey_hex);
}

int main(void) {
  test_two_accounts_seed_ok();
  test_reused_pubkey_rejected();
  printf("RESULT: PASS\n");
  return 0;
}
