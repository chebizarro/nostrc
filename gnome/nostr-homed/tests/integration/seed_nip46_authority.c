/* nh-seed-nip46-authority — seed an authority DB with one active account whose
 * ONLY enabled provider is either a client-initiated NIP-46 QR
 * (nostrconnect://) pairing (`NH_IDENTITY_PROVIDER_NIP46_QR`) or a pre-paired
 * NIP-46 bunker (`NH_IDENTITY_PROVIDER_NIP46_BUNKER`). Sibling of
 * `nh-seed-authority` (local-vault) — this one leaves the vault out of the
 * enrollment entirely.
 *
 * Purpose: the acceptance/live tests need a real account whose only path to
 * authentication is a phone signer over a real relay. The existing seeder
 * enrols a local vault; a QR- or bunker-only account cannot be produced
 * with that binary without post-hoc DB surgery.
 *
 * Usage (QR):
 *   nh-seed-nip46-authority <dir> <username> <pubkey_hex> \
 *       --provider=nip46qr [--relay=wss://...] [--name=<display>]
 *
 * Usage (pre-paired bunker):
 *   nh-seed-nip46-authority <dir> <username> <pubkey_hex> \
 *       --provider=nip46 --bunker-uri=<bunker://...> --client-sk-file=<path>
 *
 * The pubkey_hex is the account's Nostr identity pubkey (64-char
 * lowercase-hex xonly). The QR provider's public_config_json is
 * {"mode":"nostrconnect","relays":[...],"name":"..."} with a 1-byte
 * placeholder secret_blob (unused, but the stage API requires len >= 1).
 * The bunker provider's public_config_json is {"bunker_uri":"bunker://..."}
 * and secret_blob is the raw 32-byte client transport key.
 *
 * On success the account is `active` and its projection row (uid/gid/home) is
 * published to nss.db.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "nostr_identity.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_RELAYS 4

static nh_identity_ownership_result available(void *c, const char *n,
                                              uint32_t u, uint32_t g) {
  (void)c; (void)n; (void)u; (void)g;
  return NH_IDENTITY_OWNERSHIP_FREE;
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "seed-nip46: %s\n", msg); return 1; } \
  } while (0)

static int is_lc_hex64(const char *s) {
  if (!s || strlen(s) != 64) return 0;
  for (size_t i = 0; i < 64; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return 1;
}

static int hex_to_bytes32(const char *hex, uint8_t out[32]) {
  if (!is_lc_hex64(hex)) return -1;
  for (int i = 0; i < 32; i++) {
    unsigned v;
    if (sscanf(hex + i * 2, "%2x", &v) != 1) return -1;
    out[i] = (uint8_t)v;
  }
  return 0;
}

/* Slurp a file whole (trimmed of trailing whitespace/newlines). */
static char *slurp_trim(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long len = ftell(f);
  if (len < 0 || len > 64 * 1024) { fclose(f); return NULL; }
  rewind(f);
  char *buf = malloc((size_t)len + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)len, f);
  fclose(f);
  buf[n] = '\0';
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
               buf[n - 1] == ' ' || buf[n - 1] == '\t'))
    buf[--n] = '\0';
  return buf;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s <dir> <username> <pubkey_hex> --provider=nip46qr|nip46 "
          "[--relay=wss://... ...] [--name=<display>] "
          "[--bunker-uri=bunker://...] [--client-sk-file=<path>] "
          "[--home-root=<path>] [--fetch-profile|--no-fetch-profile]\n",
          argv0);
}

/* B5-profile: shell out to `nostr-homed-profile refresh` after a
 * successful NIP-46 seed. Best-effort; a stalled or offline relay
 * cannot fail the seed. */
static void seed_profile_refresh(const char *username,
                                 const char *pubkey_hex) {
  if (!username || !pubkey_hex) return;
  const char *skip = getenv("NH_SEED_NO_PROFILE");
  if (skip && *skip && !(skip[0] == '0' && skip[1] == '\0')) {
    fprintf(stderr, "seed-nip46: --fetch-profile skipped (NH_SEED_NO_PROFILE)\n");
    return;
  }
  pid_t pid = fork();
  if (pid < 0) { fprintf(stderr, "seed-nip46: fork: %s\n", strerror(errno)); return; }
  if (pid == 0) {
    char pk_arg[80];
    snprintf(pk_arg, sizeof pk_arg, "--pubkey=%s", pubkey_hex);
    const char *cli = getenv("NH_PROFILE_CLI");
    if (!cli || !*cli) cli = "nostr-homed-profile";
    char *args[] = {
      (char *)cli, (char *)"refresh", (char *)username, pk_arg, NULL,
    };
    if (cli[0] == '/') execv(cli, args); else execvp(cli, args);
    _exit(0);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) { if (errno != EINTR) break; }
  if (WIFEXITED(st))
    fprintf(stderr, "seed-nip46: profile refresh rc=%d\n", WEXITSTATUS(st));
}

/* Compose public_config_json for the QR provider. Owned by caller (malloc). */
static char *build_qr_config(char *const *relays, size_t n_relays,
                             const char *name) {
  /* Compact JSON: {"mode":"nostrconnect","relays":["<r1>","<r2>"],"name":"<n>"} */
  size_t cap = 64;
  for (size_t i = 0; i < n_relays; i++) cap += strlen(relays[i]) + 4;
  if (name) cap += strlen(name) + 12;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  size_t off = 0;
  off += snprintf(buf + off, cap - off,
                  "{\"mode\":\"nostrconnect\",\"relays\":[");
  for (size_t i = 0; i < n_relays; i++) {
    off += snprintf(buf + off, cap - off, "%s\"%s\"", i ? "," : "",
                    relays[i]);
  }
  off += snprintf(buf + off, cap - off, "]");
  if (name && *name) {
    off += snprintf(buf + off, cap - off, ",\"name\":\"%s\"", name);
  }
  off += snprintf(buf + off, cap - off, "}");
  return buf;
}

int main(int argc, char **argv) {
  const char *dir = NULL, *username = NULL, *pubkey_hex = NULL;
  const char *provider_name = NULL;
  const char *bunker_uri = NULL, *client_sk_path = NULL;
  const char *display_name = "GNOME";
  const char *home_root = "/home";
  char *relays[MAX_RELAYS];
  size_t n_relays = 0;
  int fetch_profile = 1;

  /* Positional first, then flags. */
  int posix = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (a[0] == '-' && a[1] == '-') {
      /* Boolean flags with no `=value` land here first. */
      if (!strcmp(a, "--fetch-profile"))    { fetch_profile = 1; continue; }
      if (!strcmp(a, "--no-fetch-profile")) { fetch_profile = 0; continue; }
      const char *eq = strchr(a, '=');
      if (!eq) { usage(argv[0]); return 2; }
      size_t klen = (size_t)(eq - a);
      const char *v = eq + 1;
      if (!strncmp(a, "--provider", klen)) provider_name = v;
      else if (!strncmp(a, "--relay", klen)) {
        if (n_relays >= MAX_RELAYS) {
          fprintf(stderr, "seed-nip46: too many --relay (max %d)\n", MAX_RELAYS);
          return 2;
        }
        relays[n_relays++] = strdup(v);
      }
      else if (!strncmp(a, "--name", klen)) display_name = v;
      else if (!strncmp(a, "--bunker-uri", klen)) bunker_uri = v;
      else if (!strncmp(a, "--client-sk-file", klen)) client_sk_path = v;
      else if (!strncmp(a, "--home-root", klen)) home_root = v;
      else { fprintf(stderr, "seed-nip46: unknown flag %s\n", a); return 2; }
    } else {
      if (posix == 0) dir = a;
      else if (posix == 1) username = a;
      else if (posix == 2) pubkey_hex = a;
      else { usage(argv[0]); return 2; }
      posix++;
    }
  }
  if (!dir || !username || !pubkey_hex || !provider_name) {
    usage(argv[0]);
    return 2;
  }
  if (!is_lc_hex64(pubkey_hex)) {
    fprintf(stderr, "seed-nip46: pubkey_hex must be 64 lowercase-hex chars\n");
    return 2;
  }

  int is_qr = !strcmp(provider_name, "nip46qr");
  int is_bunker = !strcmp(provider_name, "nip46");
  if (!is_qr && !is_bunker) {
    fprintf(stderr, "seed-nip46: --provider must be nip46qr or nip46\n");
    return 2;
  }

  char *config = NULL;
  uint8_t *secret_blob = NULL;
  size_t secret_len = 0;
  uint8_t placeholder = 0;

  if (is_qr) {
    if (n_relays == 0) {
      relays[n_relays++] = strdup("wss://bunker.sharegap.net");
    }
    config = build_qr_config(relays, n_relays, display_name);
    if (!config) { fprintf(stderr, "seed-nip46: config alloc failed\n"); return 1; }
    secret_blob = &placeholder;
    secret_len = 1;
  } else {
    if (!bunker_uri || strncmp(bunker_uri, "bunker://", 9) != 0) {
      fprintf(stderr,
              "seed-nip46: --bunker-uri=<bunker://...> required for --provider=nip46\n");
      return 2;
    }
    if (!client_sk_path) {
      fprintf(stderr,
              "seed-nip46: --client-sk-file=<path> required for --provider=nip46\n");
      return 2;
    }
    char *sk_hex = slurp_trim(client_sk_path);
    if (!sk_hex || !is_lc_hex64(sk_hex)) {
      fprintf(stderr,
              "seed-nip46: client-sk-file must contain 64 lowercase-hex chars\n");
      free(sk_hex);
      return 2;
    }
    static uint8_t sk_bytes[32];
    if (hex_to_bytes32(sk_hex, sk_bytes) != 0) {
      memset(sk_hex, 0, 64);
      free(sk_hex);
      fprintf(stderr, "seed-nip46: hex decode failed\n");
      return 1;
    }
    memset(sk_hex, 0, 64);
    free(sk_hex);
    secret_blob = sk_bytes;
    secret_len = 32;

    /* {"bunker_uri":"..."} — no escaping needed since bunker URIs are ASCII
     * with no ", \ or control chars. */
    size_t cap = strlen(bunker_uri) + 32;
    config = malloc(cap);
    if (!config) return 1;
    snprintf(config, cap, "{\"bunker_uri\":\"%s\"}", bunker_uri);
  }

  nh_identity_config icfg;
  nh_identity_config_defaults(&icfg);
  snprintf(icfg.authority_path, sizeof icfg.authority_path,
           "%s/authority.db", dir);
  snprintf(icfg.projection_path, sizeof icfg.projection_path, "%s/nss.db",
           dir);
  snprintf(icfg.home_root, sizeof icfg.home_root, "%s", home_root);

  nh_identity_store_options options = {0};
  options.config = &icfg;
  options.ownership_probe = available;
  options.flags = NH_IDENTITY_STORE_CREATE;
  nh_identity_store *store = NULL;
  nh_identity_rc oprc = nh_identity_store_open(&options, &store);
  if (oprc != NH_IDENTITY_OK) {
    fprintf(stderr, "seed-nip46: store open: %s (%s)\n",
            nh_identity_rc_name(oprc),
            store ? nh_identity_store_error_detail(store) : "no detail");
    free(config);
    return 1;
  }

  /* nostrc-zcll.7: derive per-username operation UUIDs so a single
   * authority can host more than one seeded account (a hardcoded UUID
   * triple collided with previous seeds and returned INVALID on the
   * second call). Use the FNV-1a hash of the username to keep the
   * function deterministic but distinct per account. */
  uint32_t uhash = 0x811c9dc5u;
  for (const char *p = username; *p; ++p) {
    uhash ^= (uint8_t)*p;
    uhash *= 0x01000193u;
  }
  char op_enroll[NH_IDENTITY_UUID_CAP];
  char op_stage[NH_IDENTITY_UUID_CAP];
  char op_activate[NH_IDENTITY_UUID_CAP];
  snprintf(op_enroll,   sizeof op_enroll,
           "00000000-0000-4000-8000-%08x0001", uhash);
  snprintf(op_stage,    sizeof op_stage,
           "00000000-0000-4000-8000-%08x0002", uhash);
  snprintf(op_activate, sizeof op_activate,
           "00000000-0000-4000-8000-%08x0003", uhash);

  nh_identity_enroll_request enroll = {0};
  enroll.username = username;
  enroll.pubkey_hex = pubkey_hex;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  CHECK(nh_identity_operation_begin_enroll(store, op_enroll, &enroll, &state) ==
            NH_IDENTITY_OK, "enroll begin");
  nh_identity_home_options hopts = {0};
  nh_identity_rc hrc = nh_identity_home_prepare(store, op_enroll, &hopts, &state);
  if (hrc != NH_IDENTITY_OK) {
    fprintf(stderr,
            "seed-nip46: home prepare: %s (need root, %s writable)\n",
            nh_identity_rc_name(hrc), home_root);
    free(config);
    return 1;
  }

  nh_identity_account account;
  CHECK(nh_identity_store_lookup_by_name(store, username, &account) ==
            NH_IDENTITY_OK, "lookup by name");

  char provider_id[NH_IDENTITY_UUID_CAP];
  nh_identity_provider_type ptype = is_qr
      ? NH_IDENTITY_PROVIDER_NIP46_QR
      : NH_IDENTITY_PROVIDER_NIP46_BUNKER;
  CHECK(nh_identity_provider_stage(store,
            op_stage, account.account_id,
            ptype, 1, config, secret_blob, secret_len, provider_id) ==
            NH_IDENTITY_OK, "provider stage");

  nh_identity_proof_attestation attestation = {0};
  strcpy(attestation.pubkey_hex, pubkey_hex);
  attestation.key_generation = account.key_generation;
  CHECK(nh_identity_provider_activate(store,
            op_activate, provider_id,
            &attestation) == NH_IDENTITY_OK, "provider activate");
  CHECK(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK,
        "publish projection");
  CHECK(nh_identity_operation_activate(store, op_enroll, &state) == NH_IDENTITY_OK,
        "operation activate");

  nh_identity_store_close(store);

  printf("seeded %s (uid=%u) pubkey=%s provider=%s in %s\n", username,
         account.uid, pubkey_hex, provider_name, dir);

  if (fetch_profile) seed_profile_refresh(username, pubkey_hex);

  free(config);
  for (size_t i = 0; i < n_relays; i++) free(relays[i]);
  return 0;
}
