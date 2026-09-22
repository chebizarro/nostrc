/* nh-seed-authority: dev/test helper — create a persistent authority DB with one
 * active account backed by a real local encrypted-key vault, for live broker/PAM
 * testing. Seals the vault to the provider_id the store assigns and swaps it into
 * providers.secret_blob directly (test-only; see the filed A enrollment bug).
 *
 * Usage: nh-seed-authority <dir> <username> <passphrase>
 * The account's key is secp256k1 private key 1 (well-known test key).
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "auth_vault.h"
#include "nostr_identity.h"
#include "nostr-keys.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


static nh_identity_ownership_result available(void *c, const char *n,
                                              uint32_t u, uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

static int hex32(const char *hex, uint8_t out[32]) {
  if (!hex || strlen(hex) != 64) return -1;
  for (int i = 0; i < 32; i++) {
    unsigned v;
    if (sscanf(hex + i * 2, "%2x", &v) != 1) return -1;
    out[i] = (uint8_t)v;
  }
  return 0;
}

#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "seed: %s\n", msg); return 1; } } while (0)

/* B5-profile: fetch kind-0 metadata for @username (pubkey @pubkey_hex) via
 * the shipped `nostr-homed-profile refresh` CLI. Best-effort: never blocks
 * or fails the seed. Skipped when NH_SEED_NO_PROFILE=1 is exported (used
 * by CI which cannot reach public relays). */
static void seed_profile_refresh(const char *username, const char *pubkey_hex) {
  if (!username || !pubkey_hex) return;
  const char *skip = getenv("NH_SEED_NO_PROFILE");
  if (skip && *skip && !(skip[0] == '0' && skip[1] == '\0')) {
    fprintf(stderr, "seed: --fetch-profile skipped (NH_SEED_NO_PROFILE set)\n");
    return;
  }
  /* Fork + exec so a CLI stall (no relays reachable) cannot hang the
   * seeder past its own timeout window. */
  pid_t pid = fork();
  if (pid < 0) { fprintf(stderr, "seed: fork for profile refresh: %s\n", strerror(errno)); return; }
  if (pid == 0) {
    char pk_arg[80];
    snprintf(pk_arg, sizeof pk_arg, "--pubkey=%s", pubkey_hex);
    /* Absolute path override: NH_PROFILE_CLI, otherwise search PATH. */
    const char *cli = getenv("NH_PROFILE_CLI");
    if (!cli || !*cli) cli = "nostr-homed-profile";
    char *args[] = {
      (char *)cli, (char *)"refresh", (char *)username, pk_arg, NULL,
    };
    if (cli[0] == '/') execv(cli, args); else execvp(cli, args);
    fprintf(stderr, "seed: exec %s: %s\n", cli, strerror(errno));
    _exit(0); /* best-effort */
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) { if (errno != EINTR) break; }
  if (WIFEXITED(st))
    fprintf(stderr, "seed: profile refresh rc=%d\n", WEXITSTATUS(st));
}

int main(int argc, char **argv) {
  /* Positional: <dir> <username> <passphrase> [auth_privkey_hex] [vault_privkey_hex]
   * Flags (may appear after the positional args):
   *   --fetch-profile / --no-fetch-profile — default is on. */
  int fetch_profile = 1;
  const char *pos[5] = {0};
  size_t np = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--fetch-profile") == 0)         { fetch_profile = 1; continue; }
    if (strcmp(a, "--no-fetch-profile") == 0)      { fetch_profile = 0; continue; }
    if (np < 5) pos[np++] = a;
    else { fprintf(stderr, "usage: %s <dir> <username> <passphrase> [auth_privkey_hex] [vault_privkey_hex] [--fetch-profile|--no-fetch-profile]\n", argv[0]); return 2; }
  }
  if (np < 3) {
    fprintf(stderr, "usage: %s <dir> <username> <passphrase> [auth_privkey_hex] [vault_privkey_hex] [--fetch-profile|--no-fetch-profile]\n", argv[0]);
    return 2;
  }
  const char *dir = pos[0], *username = pos[1], *passphrase = pos[2];
  const char *auth_sk = (np >= 4) ? pos[3] : "0000000000000000000000000000000000000000000000000000000000000001";
  const char *vault_sk = (np >= 5) ? pos[4] : auth_sk;
  char *pubkey = nostr_key_get_public(auth_sk);
  if (!pubkey || strlen(pubkey) != 64) { fprintf(stderr, "seed: bad auth private key\n"); return 2; }
  uint8_t secret[32];
  if (hex32(vault_sk, secret) != 0) { fprintf(stderr, "seed: bad vault private key\n"); return 2; }
  if (strlen(passphrase) < NH_AUTH_VAULT_PASSPHRASE_MIN) {
    fprintf(stderr, "seed: passphrase must be >= %u chars\n", NH_AUTH_VAULT_PASSPHRASE_MIN);
    return 2;
  }
  nh_identity_config config;
  nh_identity_config_defaults(&config);
  snprintf(config.authority_path, sizeof config.authority_path, "%s/authority.db", dir);
  snprintf(config.projection_path, sizeof config.projection_path, "%s/nss.db", dir);
  snprintf(config.home_root, sizeof config.home_root, "/home");
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = NH_IDENTITY_STORE_CREATE;
  nh_identity_store *store = NULL;
  nh_identity_rc oprc = nh_identity_store_open(&options, &store);
  if (oprc != NH_IDENTITY_OK) { fprintf(stderr, "seed: store open: %s (%s)\n", nh_identity_rc_name(oprc), store ? nh_identity_store_error_detail(store) : "no detail"); return 1; }

  nh_identity_enroll_request enroll = {0};
  enroll.username = username;
  enroll.pubkey_hex = pubkey;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  const char *op = "00000000-0000-4000-8000-000000000001";
  CHECK(nh_identity_operation_begin_enroll(store, op, &enroll, &state) == NH_IDENTITY_OK, "enroll");
  nh_identity_home_options hopts = {0}; /* empty home (no skel) */
  nh_identity_rc hrc = nh_identity_home_prepare(store, op, &hopts, &state);
  if (hrc != NH_IDENTITY_OK) { fprintf(stderr, "seed: home prepare: %s (need root, /home writable)\n", nh_identity_rc_name(hrc)); return 1; }
  nh_identity_account account;
  CHECK(nh_identity_store_lookup_by_name(store, username, &account) == NH_IDENTITY_OK, "lookup");
  char provider_id[NH_IDENTITY_UUID_CAP];
  const uint8_t placeholder[] = {1, 2, 3, 4};
  CHECK(nh_identity_provider_stage(store, "00000000-0000-4000-8000-000000000002",
          account.account_id, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1, "{}",
          placeholder, sizeof placeholder, provider_id) == NH_IDENTITY_OK, "provider stage");
  /* Seal the vault to the assigned provider_id, then reseal the staged record
   * (no direct DB write). */
  nh_auth_vault_binding binding = {provider_id, account.account_id, pubkey, account.key_generation};
  uint8_t *blob = NULL; size_t blob_len = 0;
  CHECK(nh_auth_vault_seal(secret, (const uint8_t *)passphrase, strlen(passphrase),
          &binding, &blob, &blob_len) == NH_AUTH_VAULT_OK, "vault seal");
  CHECK(nh_identity_provider_reseal(store, "00000000-0000-4000-8000-000000000004",
          provider_id, blob, blob_len) == NH_IDENTITY_OK, "provider reseal");
  free(blob);
  nh_identity_proof_attestation attestation = {0};
  strcpy(attestation.pubkey_hex, pubkey);
  attestation.key_generation = account.key_generation;
  CHECK(nh_identity_provider_activate(store, "00000000-0000-4000-8000-000000000003",
          provider_id, &attestation) == NH_IDENTITY_OK, "provider activate");
  CHECK(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK, "publish");
  CHECK(nh_identity_operation_activate(store, op, &state) == NH_IDENTITY_OK, "activate account");
  nh_identity_store_close(store);

  printf("seeded %s (uid=%u) pubkey=%s in %s\n", username, account.uid, pubkey, dir);

  if (fetch_profile) seed_profile_refresh(username, pubkey);

  free(pubkey);
  return 0;
}
