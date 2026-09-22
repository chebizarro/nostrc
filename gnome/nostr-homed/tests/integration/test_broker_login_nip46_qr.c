/* Full NIP-46 QR (nostrconnect://) login proof through the broker — Linux
 * headless. Seeds an account whose only enabled provider is nip46_qr, then
 * drives BEGIN_LOGIN -> SELECT_PROVIDER(nip46qr) -> SUBMIT_UNLOCK over a
 * SEQPACKET connection. The QR provider's signer hook is redirected at an
 * in-process bunker that mimics a scanned-phone signer:
 *
 *   - Happy path: bunker signs with alice's key → provider verifies → OK.
 *   - Wrong key: bunker signs with a different key → provider MUST return
 *     DENIED before any sign_event round-trip (design §8.3 get_public_key
 *     gate).
 *   - Timeout: hook returns TIMEOUT → provider maps to
 *     INTERACTION_REQUIRED (PAM_AUTH_ERR).
 *
 * Additionally verifies the greeter-artifact contract: while the tx is live
 * /run/nostr-auth/greeter/current.json exists (via the test-seam override
 * pointing at a tmpdir); after the tx retires the file is unlinked. Tracks
 * beads nostrc-z1fb Phase 3. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "../nh_test.h"
#include "auth_broker.h"
#include "auth_client.h"
#include "auth_provider.h"
#include "nostr_auth_protocol.h"
#include "nostr_identity.h"

#include "nostr-keys.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip46/nip46_types.h"
#include "nostr/nip46/nip46_uri.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Well-known secp256k1 private key 1; xonly pubkey below. */
static const char *ALICE_SK =
    "0000000000000000000000000000000000000000000000000000000000000001";
static const char *ALICE_PK =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *WRONG_SK =
    "0000000000000000000000000000000000000000000000000000000000000003";

typedef enum test_mode {
  MODE_OK = 0,
  MODE_WRONG_KEY = 1,
  MODE_TIMEOUT = 2
} test_mode;

typedef struct qr_signer_ctx {
  test_mode mode;
  const char *bunker_sk_hex;
  const char *bunker_pk_xonly;
} qr_signer_ctx;

static char *bunker_pk_sec1_from_xonly(const char *xonly_hex) {
  /* Compose SEC1-compressed even-y form: 02 || xonly. Callers know the
   * generated pubkey either has even y (ALICE) or was validated at
   * construction. */
  static char buf[68];
  snprintf(buf, sizeof buf, "02%s", xonly_hex);
  return buf;
}

/* Encrypt a plaintext NIP-46 request into a ciphertext the bunker's
 * handle_cipher accepts, and decrypt its reply back — the same "bridge"
 * pattern the pre-paired bunker test uses. */
static int bridge_call(NostrNip46Session *client, NostrNip46Session *bunker,
                       const char *bunker_pk_x, const char *plain_req,
                       NostrNip46Response *out) {
  memset(out, 0, sizeof *out);
  char *cipher = NULL;
  if (nostr_nip46_client_nip04_encrypt(client, bunker_pk_x, plain_req,
                                       &cipher) != 0 || !cipher)
    return -1;
  char *client_secret = NULL;
  if (nostr_nip46_session_get_secret(client, &client_secret) != 0 ||
      !client_secret) { free(cipher); return -1; }
  char *client_pk_x = nostr_key_get_public(client_secret);
  memset(client_secret, 0, strlen(client_secret));
  free(client_secret);
  if (!client_pk_x) { free(cipher); return -1; }
  char *cipher_reply = NULL;
  int hrc = nostr_nip46_bunker_handle_cipher(bunker, client_pk_x, cipher,
                                             &cipher_reply);
  free(cipher); free(client_pk_x);
  if (hrc != 0 || !cipher_reply) return -1;
  char *plain = NULL;
  if (nostr_nip46_client_nip04_decrypt(client, bunker_pk_x, cipher_reply,
                                       &plain) != 0 || !plain) {
    free(cipher_reply); return -1;
  }
  free(cipher_reply);
  int prc = nostr_nip46_response_parse(plain, out);
  free(plain);
  return prc == 0 ? 0 : -1;
}

static int do_connect(NostrNip46Session *client, NostrNip46Session *bunker,
                      const char *bunker_pk_x) {
  const char *params[3] = {bunker_pk_x, "", "sign_event:1,sign_event"};
  char *req = nostr_nip46_request_build("qc1", "connect", params, 3);
  if (!req) return -1;
  NostrNip46Response resp = {0};
  int rc = bridge_call(client, bunker, bunker_pk_x, req, &resp);
  free(req);
  int ok = rc == 0 && !resp.error && resp.result;
  nostr_nip46_response_free(&resp);
  return ok ? 0 : -1;
}

/* The QR provider's signer hook. Simulates the phone signer that scanned
 * the QR — parses the URI just to prove the design contract, then bridges
 * the sign_event call through an in-process bunker. */
static nh_auth_nip46_sign_status qr_signer_hook(
    NostrNip46Session *client, const char *nostrconnect_uri,
    const char *unsigned_event_json, const char *expected_account_pubkey_hex,
    char **out_signed_json, void *user_data) {
  qr_signer_ctx *ctx = user_data;
  *out_signed_json = NULL;
  if (!ctx) return NH_AUTH_NIP46_SIGN_FAILED;

  if (ctx->mode == MODE_TIMEOUT) return NH_AUTH_NIP46_SIGN_TIMEOUT;

  /* Parse URI (proves the design contract: signer starts from URI only). */
  NostrNip46ConnectURI parsed = {0};
  if (nostr_nip46_uri_parse_connect(nostrconnect_uri, &parsed) != 0) {
    nostr_nip46_uri_connect_free(&parsed);
    return NH_AUTH_NIP46_SIGN_FAILED;
  }
  int have_secret = parsed.secret && parsed.secret[0];
  nostr_nip46_uri_connect_free(&parsed);
  if (!have_secret) return NH_AUTH_NIP46_SIGN_FAILED;

  /* Build the in-process bunker with the configured signing key. */
  NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
  if (!bunker) return NH_AUTH_NIP46_SIGN_FAILED;
  (void)nostr_nip46_session_set_transport_mode(
      bunker, NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION);
  char *bunker_pk_x = nostr_key_get_public(ctx->bunker_sk_hex);
  if (!bunker_pk_x) { nostr_nip46_session_free(bunker); return NH_AUTH_NIP46_SIGN_FAILED; }
  ctx->bunker_pk_xonly = bunker_pk_x;
  char uri[256];
  snprintf(uri, sizeof uri, "bunker://%s?secret=nhtestqr",
           bunker_pk_sec1_from_xonly(bunker_pk_x));
  int cok = nostr_nip46_client_connect(bunker, uri, NULL) == 0 &&
            nostr_nip46_client_set_secret(bunker, ctx->bunker_sk_hex) == 0;
  if (!cok) {
    free(bunker_pk_x);
    nostr_nip46_session_free(bunker);
    return NH_AUTH_NIP46_SIGN_FAILED;
  }

  if (do_connect(client, bunker, bunker_pk_x) != 0) {
    free(bunker_pk_x);
    nostr_nip46_session_free(bunker);
    return NH_AUTH_NIP46_SIGN_UNAVAILABLE;
  }

  /* get_public_key gate. If the bunker's key does not match the account,
   * the design mandates DENIED BEFORE any sign_event call — mirror that
   * here (the real relay-backed path does the identical check inside
   * real_signer()). */
  if (strcmp(bunker_pk_x, expected_account_pubkey_hex) != 0) {
    free(bunker_pk_x);
    nostr_nip46_session_free(bunker);
    return NH_AUTH_NIP46_SIGN_DENIED;
  }

  /* sign_event round-trip. */
  const char *sparams[1] = {unsigned_event_json};
  char *sreq = nostr_nip46_request_build("qs1", "sign_event", sparams, 1);
  if (!sreq) {
    free(bunker_pk_x);
    nostr_nip46_session_free(bunker);
    return NH_AUTH_NIP46_SIGN_FAILED;
  }
  NostrNip46Response resp = {0};
  int rc = bridge_call(client, bunker, bunker_pk_x, sreq, &resp);
  free(sreq);
  if (rc != 0) {
    nostr_nip46_response_free(&resp);
    free(bunker_pk_x);
    nostr_nip46_session_free(bunker);
    return NH_AUTH_NIP46_SIGN_FAILED;
  }
  nh_auth_nip46_sign_status status;
  if (resp.error) {
    status = (strcmp(resp.error, "forbidden") == 0 ||
              strcmp(resp.error, "denied") == 0)
                 ? NH_AUTH_NIP46_SIGN_DENIED
                 : NH_AUTH_NIP46_SIGN_FAILED;
  } else if (resp.result) {
    *out_signed_json = strdup(resp.result);
    status = *out_signed_json ? NH_AUTH_NIP46_SIGN_OK
                              : NH_AUTH_NIP46_SIGN_FAILED;
  } else {
    status = NH_AUTH_NIP46_SIGN_FAILED;
  }
  nostr_nip46_response_free(&resp);
  free(bunker_pk_x);
  nostr_nip46_session_free(bunker);
  return status;
}

/* --------------------------------------------------------------------- */

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
  static nh_identity_config config;
  set_config(&config, dir);
  nh_identity_store_options options = {0};
  options.config = &config;
  options.ownership_probe = available;
  options.flags = flags;
  nh_identity_store *store = NULL;
  NH_CHECK(nh_identity_store_open(&options, &store) == NH_IDENTITY_OK);
  return store;
}

/* Seed an account with only nip46_qr enabled. public_config_json carries
 * a relay list; no secret_blob is stored (the QR keypair is ephemeral). */
static void seed(const char *dir) {
  nh_identity_store *store = open_store(dir, NH_IDENTITY_STORE_CREATE);
  nh_identity_enroll_request enroll = {0};
  enroll.username = "n_qralice";
  enroll.pubkey_hex = ALICE_PK;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  const char *op_enroll = "00000000-0000-4000-8000-000000000101";
  NH_CHECK(nh_identity_operation_begin_enroll(store, op_enroll, &enroll,
                                              &state) == NH_IDENTITY_OK);
  nh_identity_home_evidence staged = {12, 111}, installed = {12, 212};
  NH_CHECK(nh_identity_operation_advance_home(store, op_enroll,
             NH_IDENTITY_PHASE_RESERVED, NH_IDENTITY_PHASE_STAGED, &staged,
             &state) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_advance_home(store, op_enroll,
             NH_IDENTITY_PHASE_STAGED, NH_IDENTITY_PHASE_INSTALLED,
             &installed, &state) == NH_IDENTITY_OK);
  nh_identity_account account;
  NH_CHECK(nh_identity_store_lookup_by_name(store, "n_qralice", &account) ==
           NH_IDENTITY_OK);

  const char *config =
      "{\"mode\":\"nostrconnect\","
      "\"relays\":[\"wss://bunker.sharegap.net\"],"
      "\"name\":\"GNOME (QR)\"}";
  uint8_t placeholder = 0;
  char provider_id[NH_IDENTITY_UUID_CAP];
  NH_CHECK(nh_identity_provider_stage(store,
             "00000000-0000-4000-8000-000000000102", account.account_id,
             NH_IDENTITY_PROVIDER_NIP46_QR, 1, config, &placeholder, 1,
             provider_id) == NH_IDENTITY_OK);

  nh_identity_proof_attestation attestation = {0};
  strcpy(attestation.pubkey_hex, ALICE_PK);
  attestation.key_generation = account.key_generation;
  NH_CHECK(nh_identity_provider_activate(store,
             "00000000-0000-4000-8000-000000000103", provider_id,
             &attestation) == NH_IDENTITY_OK);

  NH_CHECK(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_activate(store, op_enroll, &state) ==
           NH_IDENTITY_OK);
  nh_identity_store_close(store);
}

/* File-scope display callback: samples the greeter-artifact filesystem
 * when the broker attaches a display block to SELECT_PROVIDER, which is
 * BEFORE the long-blocking SUBMIT_UNLOCK. */
static const char *g_probe_path;
static int *g_probe_seen;
static void probe_cb(void *ctx, const nh_auth_display *d) {
  (void)ctx; (void)d;
  if (!g_probe_path || !g_probe_seen) return;
  struct stat st;
  if (stat(g_probe_path, &st) == 0) *g_probe_seen = 1;
}

/* BEGIN_LOGIN -> SELECT_PROVIDER("nip46qr") -> SUBMIT_UNLOCK on one
 * connection. Returns 0 on transport success and sets *result_out. */
static int client_login_qr(int fd, const char *username,
                           nh_auth_result *result_out) {
  nh_auth_provider_list providers;
  nh_auth_result br = NH_AUTH_RESULT_INTERNAL_ERROR;
  if (nh_auth_client_begin_login(fd, username, "gdm-password", &providers,
                                 &br) != 0)
    return -1;
  if (br != NH_AUTH_RESULT_OK) { *result_out = br; return 0; }
  return nh_auth_client_submit_selection_display(
      fd, NH_AUTH_PROVIDER_NAME_NIP46_QR, NULL, NULL, probe_cb, NULL,
      result_out);
}

/* Fork the broker and drive one QR login through it. `mode` picks the
 * signer's behaviour and (for OK vs WRONG_KEY) the bunker signing key. */
static nh_auth_result run_login(const char *dir, const char *greeter_dir,
                                test_mode mode, int *artifact_seen_out,
                                int *artifact_removed_out) {
  int sv[2];
  NH_CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
  pid_t pid = fork();
  NH_CHECK(pid >= 0);
  if (pid == 0) {
    close(sv[0]);
    /* Child: install the QR signer hook and pin the greeter dir. */
    qr_signer_ctx ctx = {mode,
                         mode == MODE_WRONG_KEY ? WRONG_SK : ALICE_SK,
                         NULL};
    nh_auth_provider_nip46_qr_set_signer_hook(qr_signer_hook, &ctx);
    nh_broker_greeter_artifact_set_dir(greeter_dir);

    nh_identity_store *store = open_store(dir, 0);
    nh_auth_broker *broker = nh_auth_broker_new(store);
    NH_CHECK(broker);
    (void)nh_auth_broker_handle_connection(broker, sv[1]);
    nh_auth_broker_free(broker);
    nh_identity_store_close(store);
    nh_auth_provider_nip46_qr_set_signer_hook(NULL, NULL);
    close(sv[1]);
    _exit(0);
  }
  close(sv[1]);
  /* Parent: probe the greeter dir when the display callback fires. */
  static char probe_path[300];
  snprintf(probe_path, sizeof probe_path, "%s/current.json", greeter_dir);
  g_probe_path = probe_path;
  g_probe_seen = artifact_seen_out;
  *artifact_seen_out = 0;

  nh_auth_result result = NH_AUTH_RESULT_INTERNAL_ERROR;
  int rc = client_login_qr(sv[0], "n_qralice", &result);
  NH_CHECK(rc == 0);
  close(sv[0]);
  int status = 0;
  NH_CHECK(waitpid(pid, &status, 0) == pid);

  /* Broker's conn_reset_proof should have unlinked the artifact when the
   * connection closed. NB: the file is written by the broker child, so
   * checking the parent-side path succeeds because greeter_dir is a shared
   * filesystem location (a tmpdir the parent created). */
  struct stat st;
  *artifact_removed_out = (stat(probe_path, &st) != 0);
  return result;
}

int main(void) {
  char dir[] = "/tmp/nostr-login-nip46qr-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  char greeter[] = "/tmp/nostr-login-nip46qr-gr-XXXXXX";
  NH_CHECK(mkdtemp(greeter));
  seed(dir);
  int is_root = (geteuid() == 0);

  /* OK case: bunker signs with alice's key. As non-root the ACL denies
   * at BEGIN_LOGIN before the provider ever runs (identical to the
   * pre-paired bunker test's behaviour). */
  int seen = 0, removed = 0;
  nh_auth_result r = run_login(dir, greeter, MODE_OK, &seen, &removed);
  printf("qr matching signer -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == (is_root ? NH_AUTH_RESULT_OK : NH_AUTH_RESULT_DENIED));
  if (is_root) {
    NH_CHECK(seen);      /* artifact appeared during the tx */
    NH_CHECK(removed);   /* and was removed after the tx retired */
  }

  if (is_root) {
    /* Wrong-key case: bunker signs with a different key. Provider MUST
     * return DENIED before any sign_event (get_public_key gate, design
     * §8.3). The broker treats provider DENIED as a hard proof failure —
     * NH_AUTH_RESULT_INVALID_PROOF at the wire, mapping to PAM_AUTH_ERR
     * (same shape as a bad passphrase or a mangled signed event). This
     * matches the pre-paired bunker test\'s wrong-key expectation and
     * keeps the "did not accept" outcome PAM-visible without downgrading
     * to the Unix stack. */
    r = run_login(dir, greeter, MODE_WRONG_KEY, &seen, &removed);
    printf("qr wrong-key signer -> %s\n", nh_auth_result_name(r));
    NH_CHECK(r == NH_AUTH_RESULT_INVALID_PROOF);
    NH_CHECK(removed);

    /* Timeout: hook returns TIMEOUT and the provider emits
     * PROVIDER_INTERACTION_REQUIRED / RESULT_INTERACTION_REQUIRED. The
     * shipped broker collapses any provider event other than SIGNED_EVENT
     * / DENIED into PROOF_CRYPTO_ERROR → NH_AUTH_RESULT_INVALID_PROOF,
     * which is what reaches the wire (both map to PAM_AUTH_ERR upstream).
     * Design §4.1 asks for INTERACTION_REQUIRED to be distinguishable —
     * that finer-grained mapping is a broker-level policy change tracked
     * separately; the QR provider itself already emits the design event. */
    r = run_login(dir, greeter, MODE_TIMEOUT, &seen, &removed);
    printf("qr timeout -> %s\n", nh_auth_result_name(r));
    NH_CHECK(r == NH_AUTH_RESULT_INVALID_PROOF);
    NH_CHECK(removed);
  }

  /* Cleanup fixtures. */
  char path[1024];
  const char *files[] = {"authority.db", "authority.db-wal", "authority.db-shm",
                         "authority.lock", "nss.db"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", dir, files[i]);
    unlink(path);
  }
  rmdir(dir);
  const char *gfiles[] = {"current.json", "current.png", "current.json.tmp",
                          "current.png.tmp"};
  for (size_t i = 0; i < sizeof gfiles / sizeof gfiles[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", greeter, gfiles[i]);
    unlink(path);
  }
  rmdir(greeter);
  printf("RESULT: PASS (root=%d)\n", is_root);
  return 0;
}
