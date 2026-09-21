/* Linux-only: BEGIN_LOGIN reports the account's available providers, and the
 * new client login API drives SELECT_PROVIDER + SUBMIT_UNLOCK for the caller's
 * chosen provider. Also exercises the pure PAM-side helpers — provider-choice
 * parser and the retry-budget constant — that back the interactive UX. Covers
 * bucket B5, beads nostrc-zcll.6.
 *
 * Two accounts are seeded:
 *   n_local  — only NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY enabled.
 *   n_both   — both local and NH_IDENTITY_PROVIDER_NIP46_BUNKER enabled.
 *
 * The broker is served in a fork'd child over a SEQPACKET socketpair; the
 * client side calls nh_auth_client_begin_login and nh_auth_client_login_with
 * against it. Root peers pass the auth ACL; non-root peers are denied — this
 * matches the existing broker tests (test_broker_login, test_broker_login_nip46).
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "../nh_test.h"
#include "auth_broker.h"
#include "auth_client.h"
#include "auth_vault.h"
#include "nostr-keys.h"
#include "nostr_auth_protocol.h"
#include "nostr_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* Two accounts, two distinct keypairs — the accounts table has
 * `pubkey_hex TEXT UNIQUE`, so both users cannot share a key. Keypair A
 * (secp256k1 SK=1) is the classic well-known key; keypair B (SK=2) is
 * derived at runtime via nostr_key_get_public so we don't hard-code a
 * dependent constant. The NIP-46 client transport key is SK=3 — distinct
 * from either account key so a bug swapping the two would show up. */
static const char *PUBKEY_A =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *SK_B_HEX =
    "0000000000000000000000000000000000000000000000000000000000000002";
static const char *PASSPHRASE = "correct horse battery staple";
static const char *NIP46_CLIENT_SK =
    "0000000000000000000000000000000000000000000000000000000000000003";
static const char *ALICE_PK_SEC1 =
    "0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798";

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

static nh_identity_store *open_store(const char *dir, uint32_t flags) {
  static nh_identity_config config; /* must outlive the store */
  set_config(&config, dir);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = flags;
  nh_identity_store *store = NULL;
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);
  return store;
}

static void hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
  for (size_t i = 0; i < out_len; i++) {
    unsigned v;
    sscanf(hex + i * 2, "%2x", &v);
    out[i] = (uint8_t)v;
  }
}

/* Seals a real local encrypted vault whose plaintext key is `sk_bytes` (the
 * account's own secp256k1 private key), bound to (provider_id, account_id,
 * pubkey_hex, key_generation) from the account this vault belongs to. The
 * binding uses the account's OWN pubkey/generation, not a shared constant.
 * Then swaps it into the staged provider's secret_blob. Mirrors the seeding
 * path from test_broker_login.c. */
static void seal_and_reseal_local(nh_identity_store *store,
                                  const nh_identity_account *account,
                                  const char *provider_id,
                                  const char *op_reseal,
                                  const uint8_t sk_bytes[32]) {
  nh_auth_vault_binding binding = {provider_id, account->account_id,
                                   account->pubkey_hex,
                                   account->key_generation};
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  NH_CHECK(nh_auth_vault_seal(sk_bytes, (const uint8_t *)PASSPHRASE,
                              strlen(PASSPHRASE), &binding, &blob,
                              &blob_len) == NH_AUTH_VAULT_OK);
  NH_CHECK(nh_identity_provider_reseal(store, op_reseal, provider_id, blob,
                                       blob_len) == NH_IDENTITY_OK);
  free(blob);
}

/* Enrolls `username` with `pubkey_hex` and installs its home. Returns the
 * loaded account struct via *account_out. `op_enroll`, `pubkey_hex`, the
 * username, and the home_evidence values must all be unique across seed
 * calls in the same store — the identity store enforces UNIQUE on all of
 * them. */
static void enroll_active(nh_identity_store *store, const char *username,
                          const char *pubkey_hex, const char *op_enroll,
                          uint64_t home_inode,
                          nh_identity_account *account_out) {
  nh_identity_enroll_request enroll = {0};
  enroll.username = username;
  enroll.pubkey_hex = pubkey_hex;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  NH_CHECK(nh_identity_operation_begin_enroll(store, op_enroll, &enroll,
                                              &state) == NH_IDENTITY_OK);
  nh_identity_home_evidence staged = {11, home_inode};
  nh_identity_home_evidence installed = {11, home_inode + 100};
  NH_CHECK(nh_identity_operation_advance_home(
               store, op_enroll, NH_IDENTITY_PHASE_RESERVED,
               NH_IDENTITY_PHASE_STAGED, &staged, &state) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_advance_home(
               store, op_enroll, NH_IDENTITY_PHASE_STAGED,
               NH_IDENTITY_PHASE_INSTALLED, &installed, &state) ==
           NH_IDENTITY_OK);
  NH_CHECK(nh_identity_store_lookup_by_name(store, username, account_out) ==
           NH_IDENTITY_OK);
}

/* Seed n_local (only local, keypair A) and n_both (local + nip46, keypair B)
 * in a fresh store. Distinct keypairs are mandatory: accounts.pubkey_hex is
 * UNIQUE in the schema, so reusing PUBKEY_A across both users would abort
 * the second enroll. Per-account operation ids, home_inode values, usernames
 * and pubkeys are all held distinct. Every vault binding and attestation is
 * bound to its OWN account's pubkey and key_generation. */
static void seed(const char *dir) {
  /* Derive keypair B (pubkey for SK=2) at runtime so the constant follows
   * whichever curve/derivation libnostr uses. */
  char *pubkey_b = nostr_key_get_public(SK_B_HEX);
  NH_CHECK(pubkey_b && strlen(pubkey_b) == 64);

  nh_identity_store *store = open_store(dir, NH_IDENTITY_STORE_CREATE);
  nh_identity_operation_state state;
  const uint8_t placeholder[] = {1, 2, 3, 4};

  uint8_t sk_a[32] = {0};
  sk_a[31] = 1; /* secp256k1 SK 1 -> PUBKEY_A */
  uint8_t sk_b[32];
  hex_to_bytes(SK_B_HEX, sk_b, sizeof sk_b);

  /* --- n_local: keypair A, single local provider ----------------------- */
  nh_identity_account acct_local;
  enroll_active(store, "n_local", PUBKEY_A,
                "00000000-0000-4000-8000-000000000001", 101, &acct_local);
  char pid_local[NH_IDENTITY_UUID_CAP];
  NH_CHECK(nh_identity_provider_stage(
               store, "00000000-0000-4000-8000-000000000002",
               acct_local.account_id,
               NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1, "{}", placeholder,
               sizeof placeholder, pid_local) == NH_IDENTITY_OK);
  seal_and_reseal_local(store, &acct_local, pid_local,
                        "00000000-0000-4000-8000-000000000003", sk_a);
  nh_identity_proof_attestation att_local = {0};
  strcpy(att_local.pubkey_hex, acct_local.pubkey_hex);
  att_local.key_generation = acct_local.key_generation;
  NH_CHECK(nh_identity_provider_activate(
               store, "00000000-0000-4000-8000-000000000004", pid_local,
               &att_local) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_store_publish_projection(store, NULL) ==
           NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_activate(
               store, "00000000-0000-4000-8000-000000000001", &state) ==
           NH_IDENTITY_OK);

  /* --- n_both: keypair B, local + nip46 -------------------------------- */
  nh_identity_account acct_both;
  enroll_active(store, "n_both", pubkey_b,
                "00000000-0000-4000-8000-000000000010", 201, &acct_both);
  char pid_both_local[NH_IDENTITY_UUID_CAP];
  NH_CHECK(nh_identity_provider_stage(
               store, "00000000-0000-4000-8000-000000000011",
               acct_both.account_id,
               NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1, "{}", placeholder,
               sizeof placeholder, pid_both_local) == NH_IDENTITY_OK);
  seal_and_reseal_local(store, &acct_both, pid_both_local,
                        "00000000-0000-4000-8000-000000000012", sk_b);
  nh_identity_proof_attestation att_both_local = {0};
  strcpy(att_both_local.pubkey_hex, acct_both.pubkey_hex);
  att_both_local.key_generation = acct_both.key_generation;
  NH_CHECK(nh_identity_provider_activate(
               store, "00000000-0000-4000-8000-000000000013", pid_both_local,
               &att_both_local) == NH_IDENTITY_OK);

  /* nip46 provider record: public_config carries a bunker URI naming keypair
   * A here for shape only — the login-with(nip46) path is exercised in
   * test_broker_login_nip46.c against a matching in-process bunker. This
   * test just proves BEGIN_LOGIN reports both providers; the nip46 secret
   * blob is stored raw and never signed with. */
  char config[512];
  snprintf(config, sizeof config,
           "{\"bunker_uri\":\"bunker://%s?secret=nhtest\"}", ALICE_PK_SEC1);
  uint8_t nip46_client_secret[32];
  hex_to_bytes(NIP46_CLIENT_SK, nip46_client_secret, sizeof nip46_client_secret);
  char pid_both_nip46[NH_IDENTITY_UUID_CAP];
  NH_CHECK(nh_identity_provider_stage(
               store, "00000000-0000-4000-8000-000000000014",
               acct_both.account_id, NH_IDENTITY_PROVIDER_NIP46_BUNKER, 1,
               config, nip46_client_secret, sizeof nip46_client_secret,
               pid_both_nip46) == NH_IDENTITY_OK);
  nh_identity_proof_attestation att_both_nip46 = {0};
  strcpy(att_both_nip46.pubkey_hex, acct_both.pubkey_hex);
  att_both_nip46.key_generation = acct_both.key_generation;
  NH_CHECK(nh_identity_provider_activate(
               store, "00000000-0000-4000-8000-000000000015", pid_both_nip46,
               &att_both_nip46) == NH_IDENTITY_OK);

  NH_CHECK(nh_identity_store_publish_projection(store, NULL) ==
           NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_activate(
               store, "00000000-0000-4000-8000-000000000010", &state) ==
           NH_IDENTITY_OK);
  nh_identity_store_close(store);
  free(pubkey_b);
}

/* Serve `server_conns` broker connections in a child, returning the parent-
 * side client fd. The child terminates after handling that many connections;
 * callers must waitpid before spawning another server. */
struct broker_child {
  pid_t pid;
  int client_fd;
};

static struct broker_child spawn_broker(const char *dir, int server_conns) {
  struct broker_child out = {0, -1};
  int sv[2];
  NH_CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
  pid_t pid = fork();
  NH_CHECK(pid >= 0);
  if (pid == 0) {
    close(sv[0]);
    nh_identity_store *store = open_store(dir, 0);
    nh_auth_broker *broker = nh_auth_broker_new(store);
    NH_CHECK(broker);
    for (int i = 0; i < server_conns; i++)
      (void)nh_auth_broker_handle_connection(broker, sv[1]);
    nh_auth_broker_free(broker);
    nh_identity_store_close(store);
    close(sv[1]);
    _exit(0);
  }
  close(sv[1]);
  out.pid = pid;
  out.client_fd = sv[0];
  return out;
}

static void reap(struct broker_child *bc) {
  close(bc->client_fd);
  int status = 0;
  NH_CHECK(waitpid(bc->pid, &status, 0) == bc->pid);
}

/* --- Test 1: pure PAM helper coverage --------------------------------- */
static void test_provider_choice_parse(void) {
  NH_CHECK(nh_auth_provider_choice_parse("local") &&
           !strcmp(nh_auth_provider_choice_parse("local"), "local"));
  NH_CHECK(nh_auth_provider_choice_parse("  local\n") &&
           !strcmp(nh_auth_provider_choice_parse("  local\n"), "local"));
  NH_CHECK(nh_auth_provider_choice_parse("remote") &&
           !strcmp(nh_auth_provider_choice_parse("remote"), "nip46"));
  NH_CHECK(nh_auth_provider_choice_parse("nip46") &&
           !strcmp(nh_auth_provider_choice_parse("nip46"), "nip46"));
  /* Case-sensitive rejection — B0 contract accepts only lowercase ASCII. */
  NH_CHECK(nh_auth_provider_choice_parse("LOCAL") == NULL);
  NH_CHECK(nh_auth_provider_choice_parse("Local") == NULL);
  NH_CHECK(nh_auth_provider_choice_parse("") == NULL);
  NH_CHECK(nh_auth_provider_choice_parse("   ") == NULL);
  NH_CHECK(nh_auth_provider_choice_parse("something") == NULL);
  NH_CHECK(nh_auth_provider_choice_parse(NULL) == NULL);
  /* Non-ASCII payloads must not be treated as a canonical name. */
  NH_CHECK(nh_auth_provider_choice_parse("loc\xc3\xa1l") == NULL);
  NH_CHECK(nh_auth_provider_choice_parse("local\x01") == NULL);
  /* Retry budget contract from the B0 spec is exactly three. */
  NH_CHECK(NH_AUTH_PAM_MAX_INVALID_ATTEMPTS == 3u);
  printf("choice-parse OK\n");
}

/* --- Test 2: BEGIN_LOGIN reports the enabled provider set ------------- */
static void test_begin_reports_providers(const char *dir, int is_root) {
  /* n_local -> providers = [local] */
  {
    struct broker_child bc = spawn_broker(dir, 1);
    nh_auth_provider_list list = {0};
    nh_auth_result r = NH_AUTH_RESULT_INTERNAL_ERROR;
    NH_CHECK(nh_auth_client_begin_login(bc.client_fd, "n_local",
                                        "gdm-password", &list, &r) == 0);
    reap(&bc);
    if (!is_root) {
      NH_CHECK(r == NH_AUTH_RESULT_DENIED); /* ACL rejects non-root peers */
      NH_CHECK(list.count == 0);
    } else {
      NH_CHECK(r == NH_AUTH_RESULT_OK);
      NH_CHECK(list.count == 1);
      NH_CHECK(nh_auth_provider_list_has(&list, "local"));
      NH_CHECK(!nh_auth_provider_list_has(&list, "nip46"));
    }
  }

  /* n_both -> providers = [local, nip46] */
  if (is_root) {
    struct broker_child bc = spawn_broker(dir, 1);
    nh_auth_provider_list list = {0};
    nh_auth_result r = NH_AUTH_RESULT_INTERNAL_ERROR;
    NH_CHECK(nh_auth_client_begin_login(bc.client_fd, "n_both",
                                        "gdm-password", &list, &r) == 0);
    reap(&bc);
    NH_CHECK(r == NH_AUTH_RESULT_OK);
    NH_CHECK(list.count == 2);
    NH_CHECK(nh_auth_provider_list_has(&list, "local"));
    NH_CHECK(nh_auth_provider_list_has(&list, "nip46"));
  }
  printf("begin-login providers OK\n");
}

/* --- Test 3: login_with(local) succeeds against the both-providers account. */
static void test_login_with_local(const char *dir, int is_root) {
  /* Uses n_both's vault (sealed with keypair B's SK). If the vault binding
   * had accidentally been sealed against another account's pubkey/generation,
   * verification would return INVALID_PROOF here rather than OK. */
  struct broker_child bc = spawn_broker(dir, 1);
  nh_auth_result r = NH_AUTH_RESULT_INTERNAL_ERROR;
  NH_CHECK(nh_auth_client_login_with(bc.client_fd, "n_both", "gdm-password",
                                     "local", PASSPHRASE, &r) == 0);
  reap(&bc);
  NH_CHECK(r == (is_root ? NH_AUTH_RESULT_OK : NH_AUTH_RESULT_DENIED));
  printf("login_with(local) correct -> %s\n", nh_auth_result_name(r));
}

/* --- Test 4: >3 invalid local attempts remain refused --------------- */
static void test_retry_budget_local(const char *dir, int is_root) {
  if (!is_root) {
    printf("retry-budget skipped (non-root peer ACL)\n");
    return;
  }
  /* PAM's budget is NH_AUTH_PAM_MAX_INVALID_ATTEMPTS. Prove each attempt is
   * independently refused at the broker (the PAM module counts these and
   * returns PAM_MAXTRIES after the third) — a bug that let a wrong passphrase
   * through would surface as an unexpected OK here. */
  for (unsigned int i = 0; i < NH_AUTH_PAM_MAX_INVALID_ATTEMPTS + 1u; i++) {
    struct broker_child bc = spawn_broker(dir, 1);
    nh_auth_result r = NH_AUTH_RESULT_INTERNAL_ERROR;
    NH_CHECK(nh_auth_client_login_with(bc.client_fd, "n_local", "gdm-password",
                                       "local", "definitely wrong pass", &r) ==
             0);
    reap(&bc);
    NH_CHECK(r == NH_AUTH_RESULT_INVALID_PROOF);
  }
  printf("retry-budget: %u invalid attempts all refused\n",
         NH_AUTH_PAM_MAX_INVALID_ATTEMPTS + 1u);
}

int main(void) {
  test_provider_choice_parse();

  char dir[] = "/tmp/nostr-login-providers-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  seed(dir);
  int is_root = (geteuid() == 0);

  test_begin_reports_providers(dir, is_root);
  test_login_with_local(dir, is_root);
  test_retry_budget_local(dir, is_root);

  char path[1024];
  const char *files[] = {"authority.db", "authority.db-wal", "authority.db-shm",
                         "authority.lock", "nss.db"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i]);
    unlink(path);
  }
  rmdir(dir);
  printf("RESULT: PASS (root=%d)\n", is_root);
  return 0;
}
