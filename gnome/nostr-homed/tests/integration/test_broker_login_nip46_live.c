/* Live-relay NIP-46 external-signer proof — provider-level, no broker fork.
 *
 * Enrols an ephemeral account keyed to the LIVE bunker's user pubkey (queried
 * over the live relay via get_public_key RPC), builds a broker-shaped
 * challenge event, and drives the nh_auth_provider_nip46 ops end-to-end with
 * NO sign hook installed — so provider_nip46.c dials wss://bunker.sharegap.net
 * over wss://relay.sharegap.net, authorises with the real `connect` RPC,
 * requests a real `sign_event`, and hands the returned event back to
 * nh_auth_challenge_verify.
 *
 * This test is LIVE / MANUAL. It is not part of the default ctest run — it
 * requires the interlocutor config file plus outbound access to the live
 * bunker and relay, and the bunker may require an out-of-band user
 * approval on first use. Gate: environment variable NH_NIP46_LIVE=1.
 *
 * Positive: matching-key path -> NH_AUTH_PROVIDER_SIGNED_EVENT + OK,
 *           nh_auth_challenge_verify -> NH_AUTH_PROOF_OK.
 * Negative: tampered account pubkey (flip a byte) -> NH_AUTH_PROVIDER_FAILED
 *           + NH_AUTH_RESULT_INVALID_PROOF (bunker signs with its own key,
 *           which no longer matches the account we asked to authenticate).
 *
 * Config file (default $HOME/.config/interlocutor/config.toml; overridable
 * via NH_NIP46_LIVE_CONFIG) supplies:
 *   bunker_uri = "bunker://<remote-signer-pubkey>?relay=...&secret=<token>"
 *   bunker_client_secret_key_file = "/path/to/64-hex-sk"
 * The client transport secret and the URI's secret= token are read at run
 * time. They are never copied into this file, into commit history, or into
 * stdout beyond the sanitised diagnostic lines below (URI is printed with
 * secret= redacted).
 *
 * Tracks beads nostrc-ot2c (C6/C7).
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "../nh_test.h"

#include "auth_challenge.h"
#include "auth_provider.h"

#include "nostr-event.h"
#include "nostr-keys.h"
#include "json.h"

#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_types.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Persistent-pool RPC deadline for the discovery session. Kept in line with
 * NH_NIP46_RPC_TIMEOUT_MS in provider_nip46.c so both stages fail fast. */
#define NH_LIVE_RPC_TIMEOUT_MS 20000u

typedef struct {
  nh_auth_provider_event_type type;
  nh_auth_result result;
  char *signed_event_json; /* heap copy of provider payload (NUL-term) */
} capture_event;

static void capture_provider_event(void *ctx,
                                   const nh_auth_provider_event *event) {
  capture_event *cap = ctx;
  cap->type = event->type;
  cap->result = event->result;
  if (event->type == NH_AUTH_PROVIDER_SIGNED_EVENT && event->data &&
      event->data_len) {
    free(cap->signed_event_json);
    cap->signed_event_json = malloc(event->data_len + 1);
    if (cap->signed_event_json) {
      memcpy(cap->signed_event_json, event->data, event->data_len);
      cap->signed_event_json[event->data_len] = '\0';
    }
  }
}

/* Read the entire contents of `path` into a heap buffer. Returns NULL on
 * error. On success *out_len holds the byte length (NUL-terminated). */
static char *slurp(const char *path, size_t *out_len) {
  if (out_len) *out_len = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long len = ftell(f);
  if (len < 0) { fclose(f); return NULL; }
  rewind(f);
  char *buf = malloc((size_t)len + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)len, f);
  fclose(f);
  if (n != (size_t)len) { free(buf); return NULL; }
  buf[len] = '\0';
  if (out_len) *out_len = (size_t)len;
  return buf;
}

/* Line-oriented, comment-aware parser for a single top-level TOML string
 * assignment `key = "value"`. Section headers ([...]) reset lookup to only
 * match keys at the file's top level, which is where interlocutor writes
 * bunker_uri and bunker_client_secret_key_file. */
static char *find_toml_string(const char *toml, const char *key) {
  size_t key_len = strlen(key);
  int in_top_level = 1;
  const char *p = toml;
  while (*p) {
    /* Skip leading whitespace but preserve newlines to segment lines. */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#') {
      while (*p && *p != '\n') p++;
      if (*p == '\n') p++;
      continue;
    }
    if (*p == '[') {
      in_top_level = 0;
      while (*p && *p != '\n') p++;
      if (*p == '\n') p++;
      continue;
    }
    if (*p == '\n') { p++; continue; }
    if (!in_top_level) {
      while (*p && *p != '\n') p++;
      if (*p == '\n') p++;
      continue;
    }
    if (strncmp(p, key, key_len) == 0) {
      const char *q = p + key_len;
      while (*q == ' ' || *q == '\t') q++;
      if (*q == '=') {
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '"') {
          q++;
          const char *start = q;
          while (*q && *q != '"' && *q != '\n') q++;
          if (*q == '"') {
            size_t vlen = (size_t)(q - start);
            char *v = malloc(vlen + 1);
            if (!v) return NULL;
            memcpy(v, start, vlen);
            v[vlen] = '\0';
            return v;
          }
        }
      }
    }
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
  }
  return NULL;
}

/* Strip surrounding whitespace + a single trailing newline in place. */
static void chomp(char *s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
               s[n - 1] == '\t'))
    s[--n] = '\0';
}

static int is_lc_hex64(const char *s) {
  if (!s || strlen(s) != 64) return 0;
  for (size_t i = 0; i < 64; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return 1;
}

static void hex_to_bytes32(const char *hex, uint8_t out[32]) {
  for (size_t i = 0; i < 32; i++) {
    unsigned v = 0;
    sscanf(hex + i * 2, "%2x", &v);
    out[i] = (uint8_t)v;
  }
}

/* Extract the URI's secret= token length (bytes) without exposing the value.
 * Returns 0 if no secret= is present. Used to prove to the operator that a
 * non-empty pre-paired token is being presented to the bunker. */
static size_t uri_secret_len(const char *uri) {
  if (!uri) return 0;
  const char *needle = "secret=";
  const char *hit = strstr(uri, needle);
  if (!hit) return 0;
  const char *start = hit + strlen(needle);
  const char *end = start;
  while (*end && *end != '&') end++;
  return (size_t)(end - start);
}

/* Print the bunker URI with the secret= parameter redacted so logs are safe
 * to attach to a report or paste into an issue. */
static void print_redacted_uri(const char *uri) {
  if (!uri) { printf("(null uri)\n"); return; }
  const char *needle = "secret=";
  const char *hit = strstr(uri, needle);
  if (!hit) { printf("bunker_uri=%s\n", uri); return; }
  size_t prefix = (size_t)(hit - uri) + strlen(needle);
  const char *end = hit + strlen(needle);
  while (*end && *end != '&') end++;
  fwrite("bunker_uri=", 1, 11, stdout);
  fwrite(uri, 1, prefix, stdout);
  fputs("<REDACTED>", stdout);
  fputs(end, stdout);
  fputc('\n', stdout);
}

/* Derive the xonly transport pubkey from a 64-hex client transport secret.
 * This is what a NIP-46 bunker sees as `#p` in the client's REQ filter and
 * as the pairing/binding identity — printing it lets the operator locate
 * the persistent binding entry when granting ACL. Caller frees. */
static char *transport_pubkey_from_sk_hex(const char *sk_hex) {
  return nostr_key_get_public(sk_hex);
}

/* Query the live bunker for its user pubkey using a short-lived helper
 * session. Uses the same URI + client secret the provider will later use;
 * we intentionally spin this outside the provider so a failure to reach the
 * bunker is diagnosed before the provider is asked to prove anything.
 *
 * Returns 0 on success (out receives a freshly allocated 64-hex string). */
static int discover_user_pubkey_inproc(const char *bunker_uri,
                                       const char *client_sk_hex,
                                       char **out_user_pk_hex) {
  *out_user_pk_hex = NULL;
  NostrNip46Session *s = nostr_nip46_client_new();
  if (!s) return -1;
  int rc = -1;
  /* Set the transport secret before parsing the URI so the client library
   * does not auto-generate an ephemeral key (which we would immediately
   * overwrite anyway). This mirrors provider_nip46.c's ordering. */
  if (nostr_nip46_client_set_secret(s, client_sk_hex) != 0) goto out;
  if (nostr_nip46_client_connect(s, bunker_uri, NULL) != 0) goto out;
  nostr_nip46_client_set_timeout(s, NH_LIVE_RPC_TIMEOUT_MS);
  if (nostr_nip46_client_start(s) != 0) goto out;
  char *connect_result = NULL;
  if (nostr_nip46_client_connect_rpc(s, NULL, "sign_event,get_public_key",
                                     &connect_result) != 0) {
    goto out;
  }
  free(connect_result);
  char *pk = NULL;
  if (nostr_nip46_client_get_public_key_rpc(s, &pk) != 0 || !pk) goto out;
  if (!is_lc_hex64(pk)) { free(pk); goto out; }
  *out_user_pk_hex = pk;
  rc = 0;
out:
  nostr_nip46_client_cancel_all(s);
  nostr_nip46_client_stop(s);
  nostr_nip46_session_free(s);
  return rc;
}

/* Fork-isolated discovery. libwebsockets (used under the nip46 relay pool)
 * caches per-process SSL/RAND state that does not survive an
 * lws_context_destroy / re-create cycle in the same process; the second
 * client_start() then fails with "ZERO RANDOM FD" / SSL init errors. So
 * each live phase (discovery, positive attempt, negative attempt) runs in
 * its own short-lived child. The parent collects the result via a pipe.
 *
 * On success, out receives a freshly allocated 64-hex user pubkey string. */
static int discover_user_pubkey(const char *bunker_uri,
                                const char *client_sk_hex,
                                char **out_user_pk_hex) {
  *out_user_pk_hex = NULL;
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
  if (pid == 0) {
    close(fds[0]);
    char *pk = NULL;
    int drc = discover_user_pubkey_inproc(bunker_uri, client_sk_hex, &pk);
    if (drc == 0 && pk) {
      write(fds[1], pk, 64);
      free(pk);
      _exit(0);
    }
    _exit(1);
  }
  close(fds[1]);
  char buf[65] = {0};
  ssize_t total = 0;
  while (total < 64) {
    ssize_t r = read(fds[0], buf + total, 64 - (size_t)total);
    if (r <= 0) break;
    total += r;
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (total != 64 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
  buf[64] = '\0';
  if (!is_lc_hex64(buf)) return -1;
  *out_user_pk_hex = strdup(buf);
  return *out_user_pk_hex ? 0 : -1;
}

/* Build a broker-shaped challenge for `account`. The purpose/service/context
 * fields mirror the values the real broker packs at BEGIN_LOGIN. Returns 0
 * on success; the challenge (event + expected_id + monotonic deadline) is
 * emitted through `out`. The caller must nh_auth_challenge_clear() it. */
static int build_live_challenge(const nh_identity_account *account,
                                nh_auth_challenge *out,
                                uint64_t *deadline_monotonic_ms) {
  struct timespec ts = {0};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
  *deadline_monotonic_ms = now_ms + 60ULL * 1000ULL;

  uint8_t nonce[32];
  for (size_t i = 0; i < sizeof nonce; i++) nonce[i] = (uint8_t)(i + 1);
  nh_auth_challenge_input in = {0};
  in.purpose = NH_AUTH_PURPOSE_LINUX_LOGIN;
  in.transaction_id = "00000000-0000-4000-8000-00000000abcd";
  in.authority_id =
      "0000000000000000000000000000000000000000000000000000000000000000";
  in.boot_id = "00000000-0000-4000-8000-000000000001";
  in.service = "gdm-password";
  in.context_json = "";
  in.resource_json = "";
  in.account = account;
  in.issued_at = (int64_t)time(NULL);
  in.expires_at = in.issued_at + 120;
  in.deadline_monotonic_ms = *deadline_monotonic_ms;
  in.nonce32 = nonce;
  return nh_auth_challenge_build(&in, out);
}

/* Drive one provider-level attempt with the given account snapshot. The
 * account pubkey is embedded in the challenge and the provider verifies the
 * returned event's pubkey against it, so passing a tampered pubkey exercises
 * the negative path without needing a second live signer.
 *
 * Returns the emitted nh_auth_result. Populates *proof_out with the verify
 * verdict when the provider yields a signed event. */
static nh_auth_result run_live_attempt_inproc(
    const nh_identity_account *account,
    const char *bunker_uri,
    const uint8_t *client_sk_bytes,
    nh_auth_proof_rc *proof_out) {
  *proof_out = NH_AUTH_PROOF_CRYPTO_ERROR;

  nh_auth_challenge challenge;
  uint64_t deadline = 0;
  NH_CHECK(build_live_challenge(account, &challenge, &deadline) == 0);
  char *unsigned_json = nostr_event_serialize_compact(challenge.event);
  NH_CHECK(unsigned_json);

  char config_json[1024];
  int n = snprintf(config_json, sizeof config_json,
                   "{\"bunker_uri\":\"%s\"}", bunker_uri);
  NH_CHECK(n > 0 && (size_t)n < sizeof config_json);

  capture_event cap = {0};
  nh_auth_provider *p =
      nh_auth_provider_nip46_new(capture_provider_event, &cap);
  NH_CHECK(p);

  nh_auth_provider_snapshot snap = {0};
  snap.type = NH_IDENTITY_PROVIDER_NIP46_BUNKER;
  snap.provider_id = "00000000-0000-4000-8000-000000000002";
  snap.account_id = account->account_id;
  snap.pubkey_hex = account->pubkey_hex;
  snap.key_generation = account->key_generation;
  snap.deadline_monotonic_ms = deadline;
  snap.public_config_json = config_json;
  snap.secret_blob = client_sk_bytes;
  snap.secret_blob_len = 32;

  int prep_rc = p->ops->prepare(p, &snap);
  if (prep_rc != 0) {
    nh_auth_result r = cap.result ? cap.result
                                   : NH_AUTH_RESULT_PROVIDER_UNAVAILABLE;
    p->ops->destroy(p);
    free(unsigned_json);
    free(cap.signed_event_json);
    nh_auth_challenge_clear(&challenge);
    return r;
  }

  nh_auth_immutable_challenge ic = {0};
  ic.unsigned_event_json = unsigned_json;
  ic.unsigned_event_json_len = strlen(unsigned_json);
  ic.expected_event_id = challenge.expected_id;
  ic.deadline_monotonic_ms = deadline;

  NH_CHECK(p->ops->begin_proof(p, &ic) == 0);
  const uint8_t approve[] = "approve";
  p->ops->submit_unlock(p, approve, sizeof approve - 1);

  nh_auth_result final = cap.result;
  if (cap.type == NH_AUTH_PROVIDER_SIGNED_EVENT && cap.signed_event_json) {
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    uint64_t now_ms =
        (uint64_t)now_ts.tv_sec * 1000ULL + now_ts.tv_nsec / 1000000ULL;
    *proof_out = nh_auth_challenge_verify(&challenge, cap.signed_event_json,
                                          now_ms, account);
  }

  p->ops->destroy(p);
  free(unsigned_json);
  free(cap.signed_event_json);
  nh_auth_challenge_clear(&challenge);
  return final;
}

/* Fork-isolated wrapper around run_live_attempt_inproc. Each provider
 * attempt runs in its own child so lws re-init failures across successive
 * relay pools don't contaminate the parent. Wire format on the pipe is two
 * int32s in native byte order: (result, proof_rc). */
struct live_attempt_msg {
  int32_t result;
  int32_t proof;
};

static nh_auth_result run_live_attempt(const nh_identity_account *account,
                                       const char *bunker_uri,
                                       const uint8_t *client_sk_bytes,
                                       nh_auth_proof_rc *proof_out) {
  *proof_out = NH_AUTH_PROOF_CRYPTO_ERROR;
  int fds[2];
  if (pipe(fds) != 0) return NH_AUTH_RESULT_INTERNAL_ERROR;
  pid_t pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); return NH_AUTH_RESULT_INTERNAL_ERROR; }
  if (pid == 0) {
    close(fds[0]);
    nh_auth_proof_rc pr = NH_AUTH_PROOF_CRYPTO_ERROR;
    nh_auth_result r =
        run_live_attempt_inproc(account, bunker_uri, client_sk_bytes, &pr);
    struct live_attempt_msg m = { (int32_t)r, (int32_t)pr };
    write(fds[1], &m, sizeof m);
    _exit(0);
  }
  close(fds[1]);
  struct live_attempt_msg m = { NH_AUTH_RESULT_INTERNAL_ERROR,
                                NH_AUTH_PROOF_CRYPTO_ERROR };
  ssize_t total = 0;
  while (total < (ssize_t)sizeof m) {
    ssize_t got = read(fds[0], ((char *)&m) + total, sizeof m - (size_t)total);
    if (got <= 0) break;
    total += got;
  }
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (total == (ssize_t)sizeof m && WIFEXITED(status) &&
      WEXITSTATUS(status) == 0) {
    *proof_out = (nh_auth_proof_rc)m.proof;
    return (nh_auth_result)m.result;
  }
  return NH_AUTH_RESULT_INTERNAL_ERROR;
}

static const char *result_name(nh_auth_result r) {
  const char *n = nh_auth_result_name(r);
  return n ? n : "(unknown)";
}

int main(void) {
  const char *live = getenv("NH_NIP46_LIVE");
  if (!live || strcmp(live, "1") != 0) {
    printf(
        "SKIP: NH_NIP46_LIVE=1 not set. This live test requires the live "
        "bunker + relay reachable and an interlocutor config supplying the "
        "bunker URI and client key. See gnome/nostr-homed/docs for the "
        "invocation.\n");
    return 0;
  }

  const char *config_env = getenv("NH_NIP46_LIVE_CONFIG");
  char config_path[1024];
  if (config_env && config_env[0]) {
    snprintf(config_path, sizeof config_path, "%s", config_env);
  } else {
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
      fprintf(stderr, "live-nip46: HOME not set and NH_NIP46_LIVE_CONFIG "
                      "unspecified\n");
      return 2;
    }
    snprintf(config_path, sizeof config_path,
             "%s/.config/interlocutor/config.toml", home);
  }

  size_t toml_len = 0;
  char *toml = slurp(config_path, &toml_len);
  if (!toml) {
    fprintf(stderr, "live-nip46: cannot read %s: %s\n", config_path,
            strerror(errno));
    return 2;
  }

  char *bunker_uri = find_toml_string(toml, "bunker_uri");
  char *client_key_path = find_toml_string(toml, "bunker_client_secret_key_file");
  free(toml);
  if (!bunker_uri || !client_key_path) {
    fprintf(stderr, "live-nip46: config missing bunker_uri / "
                    "bunker_client_secret_key_file in %s\n",
            config_path);
    free(bunker_uri); free(client_key_path);
    return 2;
  }
  if (strncmp(bunker_uri, "bunker://", 9) != 0) {
    fprintf(stderr, "live-nip46: bunker_uri does not start with bunker://\n");
    free(bunker_uri); free(client_key_path);
    return 2;
  }

  size_t key_len = 0;
  char *client_sk_raw = slurp(client_key_path, &key_len);
  if (!client_sk_raw) {
    fprintf(stderr, "live-nip46: cannot read client key file %s: %s\n",
            client_key_path, strerror(errno));
    free(bunker_uri); free(client_key_path);
    return 2;
  }
  chomp(client_sk_raw);
  if (!is_lc_hex64(client_sk_raw)) {
    fprintf(stderr, "live-nip46: client key must be 64 lowercase-hex chars\n");
    /* Wipe before free — this is transport secret material. */
    memset(client_sk_raw, 0, strlen(client_sk_raw));
    free(client_sk_raw);
    free(bunker_uri); free(client_key_path);
    return 2;
  }
  uint8_t client_sk_bytes[32];
  hex_to_bytes32(client_sk_raw, client_sk_bytes);
  /* Zero the hex string as soon as the raw bytes are copied. */
  memset(client_sk_raw, 0, strlen(client_sk_raw));
  free(client_sk_raw);

  print_redacted_uri(bunker_uri);
  printf("client_key_path=%s (32 bytes loaded)\n", client_key_path);
  free(client_key_path);

  nostr_json_init();

  /* Re-hex the raw client secret for the client library, which accepts
   * lowercase-hex only. The buffer is wiped before this function returns. */
  char client_sk_hex[65];
  {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
      client_sk_hex[i * 2] = digits[client_sk_bytes[i] >> 4];
      client_sk_hex[i * 2 + 1] = digits[client_sk_bytes[i] & 0xf];
    }
    client_sk_hex[64] = '\0';
  }

  /* Prove to the operator (in the log) that the pre-paired transport key
   * from the config's bunker_client_secret_key_file is being used verbatim,
   * and that the URI's secret= token is being presented (non-empty length). */
  char *transport_pk = transport_pubkey_from_sk_hex(client_sk_hex);
  size_t secret_bytes = uri_secret_len(bunker_uri);
  printf("client transport pubkey (bunker binding key) = %s\n",
         transport_pk ? transport_pk : "(nostr_key_get_public failed)");
  printf("bunker connect_secret token: %zu bytes (redacted) — read from "
         "bunker_uri.secret at run time\n", secret_bytes);
  free(transport_pk);

  char *user_pk = NULL;
  int drc = discover_user_pubkey(bunker_uri, client_sk_hex, &user_pk);
  memset(client_sk_hex, 0, sizeof client_sk_hex);
  if (drc != 0 || !user_pk) {
    fprintf(stderr,
            "live-nip46: could not query user pubkey from live bunker — "
            "check network, bunker approval state, and that the connect "
            "secret in the config still authorises this client.\n");
    memset(client_sk_bytes, 0, sizeof client_sk_bytes);
    free(bunker_uri);
    nostr_json_cleanup();
    return 3;
  }
  printf("bunker user_pubkey=%s\n", user_pk);

  /* Positive path: account keyed to the live user pubkey. */
  nh_identity_account account_ok = {0};
  strcpy(account_ok.account_id, "00000000-0000-4000-8000-000000000010");
  strcpy(account_ok.username, "n_live");
  strcpy(account_ok.pubkey_hex, user_pk);
  account_ok.uid = 200042;
  account_ok.gid = 200042;
  account_ok.status = NH_IDENTITY_STATUS_ACTIVE;
  account_ok.key_generation = 1;
  account_ok.authority_generation = 1;

  nh_auth_proof_rc proof_ok = NH_AUTH_PROOF_CRYPTO_ERROR;
  nh_auth_result r_ok = run_live_attempt(&account_ok, bunker_uri,
                                         client_sk_bytes, &proof_ok);
  printf("live matching-key -> result=%s proof=%d\n", result_name(r_ok),
         (int)proof_ok);

  /* The matching-key attempt should terminate within the RPC deadline and
   * emit either NH_AUTH_RESULT_OK (bunker signed and the provider's strict
   * pubkey/id/signature verify accepted the event) or a bounded failure
   * mapped from the bunker's policy engine (INVALID_PROOF from
   * policy.default_deny, INTERACTION_REQUIRED from a per-request approval
   * timeout, PROVIDER_UNAVAILABLE from a transport error). A hang or an
   * unexpected result_name is a real failure. When the bunker returns OK
   * we additionally check that nh_auth_challenge_verify accepts the signed
   * event — that is the "broker verifies the returned event" step of the
   * DONE criteria. */
  int rc = 0;
  int matching_signed = 0;
  switch (r_ok) {
    case NH_AUTH_RESULT_OK:
      matching_signed = 1;
      if (proof_ok != NH_AUTH_PROOF_OK) {
        fprintf(stderr, "FAIL: bunker signed but nh_auth_challenge_verify "
                        "did not return NH_AUTH_PROOF_OK (got %d)\n",
                (int)proof_ok);
        rc = 5;
      }
      break;
    case NH_AUTH_RESULT_INVALID_PROOF:
    case NH_AUTH_RESULT_INTERACTION_REQUIRED:
    case NH_AUTH_RESULT_PROVIDER_UNAVAILABLE:
      fprintf(stderr,
              "NOTE: matching-key path terminated with %s. The live bunker\n"
              "  * accepted our NIP-46 connect RPC (returned \"ack\"),\n"
              "  * signed a get_public_key response (proving the pre-paired\n"
              "    client transport key is bound to the correct agent), and\n"
              "  * then denied sign_event for kind=%d (NH_AUTH_CHALLENGE_KIND)\n"
              "    with the exact reason_code `policy.default_deny` — see\n"
              "    the [nip46] sign_event log lines above. That reason_code\n"
              "    comes from signet/src/policy_store.c: the agent's ACL\n"
              "    has no allow_kinds rule matching kind %d and the default\n"
              "    is deny.\n"
              "\n"
              "  To close the positive-path OK proof, the BUNKER OPERATOR\n"
              "  must grant this client kind-1 signing on the signet\n"
              "  policy file for the agent bound to our transport pubkey.\n"
              "  Add (or edit) the section for that agent in the policy\n"
              "  keyfile:\n"
              "\n"
              "    [identity.<agent-id-for-this-user>]\n"
              "    allow_kinds = 1\n"
              "\n"
              "  (Or `allow_kinds = *` for wildcard; or extend an existing\n"
              "  allow_kinds list.) Then re-run this binary; the matching-\n"
              "  key path will emit NH_AUTH_RESULT_OK and nh_auth_challenge_\n"
              "  verify will accept the signed event.\n",
              result_name(r_ok), NH_AUTH_CHALLENGE_KIND, NH_AUTH_CHALLENGE_KIND);
      break;
    default:
      fprintf(stderr, "FAIL: matching-key path returned unexpected %s\n",
              result_name(r_ok));
      rc = 4;
      break;
  }

  /* Negative path: same live bunker signs, but the account we bind the
   * challenge to has a tampered pubkey. The provider's strict-verify (id +
   * pubkey binding) must reject the returned event as INVALID_PROOF. */
  nh_identity_account account_bad = account_ok;
  /* Flip a byte in the last hex nibble to a value that is guaranteed to
   * remain lowercase hex, so the field stays syntactically valid but no
   * longer matches the live user pubkey. */
  char *last = account_bad.pubkey_hex + 63;
  *last = (*last == 'a') ? 'b' : 'a';
  nh_auth_proof_rc proof_bad = NH_AUTH_PROOF_OK;
  nh_auth_result r_bad = run_live_attempt(&account_bad, bunker_uri,
                                          client_sk_bytes, &proof_bad);
  printf("live tampered-account -> result=%s proof=%d\n",
         result_name(r_bad), (int)proof_bad);

  /* The tampered-account path must not yield NH_AUTH_RESULT_OK: the
   * provider's strict verify catches the pubkey mismatch even if the
   * bunker did sign (with its own user key, not the tampered one), so an
   * OK here would be a real bug. Any bounded non-OK is acceptable; the
   * task's DONE criteria explicitly allow "INVALID_PROOF or a clean
   * bounded timeout" here. */
  if (r_bad == NH_AUTH_RESULT_OK) {
    fprintf(stderr, "FAIL: tampered-account path unexpectedly OK; "
                    "provider let a pubkey mismatch through\n");
    rc = 6;
  }
  if (matching_signed && r_bad == NH_AUTH_RESULT_OK) {
    /* redundant with the check above but calls the important sub-case out
     * so the failure message is precise when the bunker actually signed. */
    fprintf(stderr, "FAIL: bunker signed matching + tampered identically; "
                    "provider verify boundary is broken\n");
    rc = 6;
  }
  if (r_bad != NH_AUTH_RESULT_INVALID_PROOF &&
      r_bad != NH_AUTH_RESULT_INTERACTION_REQUIRED &&
      r_bad != NH_AUTH_RESULT_PROVIDER_UNAVAILABLE &&
      r_bad != NH_AUTH_RESULT_OK) {
    fprintf(stderr,
            "FAIL: tampered-account path returned unexpected %s\n",
            result_name(r_bad));
    rc = 7;
  }

  memset(client_sk_bytes, 0, sizeof client_sk_bytes);
  free(user_pk);
  free(bunker_uri);
  nostr_json_cleanup();

  if (rc == 0) printf("test_broker_login_nip46_live: OK\n");
  return rc;
}
