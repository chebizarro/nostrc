/* Linux-only: full NIP-46 external-signer login proof through the broker.
 *
 * Seeds an active account with an enabled nip46_bunker provider (public_config
 * carries a bunker:// URI pointing at the account's own pubkey; secret_blob is
 * the client's 32-byte transport key), then drives
 *   BEGIN_LOGIN -> SELECT_PROVIDER(nip46) -> SUBMIT_UNLOCK
 * over a SEQPACKET connection. An in-process NIP-46 bunker signs the broker's
 * challenge without touching real relays (the roundtrip harness pattern from
 * nips/nip46/tests/test_bunker_roundtrip.c and test_bunker_sign_event_real.c):
 * the provider's sign hook is redirected at a bridge that encrypts to the
 * bunker with the provider's own client session, hands the ciphertext to
 * nostr_nip46_bunker_handle_cipher, and decrypts the reply back.
 *
 * As root the correct-key bunker yields OK (+receipt); a bunker signing with a
 * different key fails with INVALID_PROOF. As a non-root peer the ACL denies.
 * Tracks beads nostrc-ot2c.
 */
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

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* Well-known secp256k1 private key 1; xonly pubkey is the constant below. */
static const char *ALICE_SK =
    "0000000000000000000000000000000000000000000000000000000000000001";
static const char *ALICE_PK =
    "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
/* SEC1-compressed form of ALICE_PK (odd/even byte prefix). The NIP-46 client
 * transport helpers accept either the 64-char xonly or the 66-char SEC1
 * pubkey; the roundtrip harness uses SEC1 for stability with older callers. */
static const char *ALICE_PK_SEC1 =
    "0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798";

/* Client transport key (independent of the user's identity key). */
static const char *CLIENT_SK =
    "0000000000000000000000000000000000000000000000000000000000000002";

/* Wrong signer key: valid secp256k1 SK whose pubkey differs from ALICE_PK. */
static const char *WRONG_SK =
    "0000000000000000000000000000000000000000000000000000000000000003";

/* SEC1-compressed form of WRONG_SK — needed for the bunker's bunker:// URI in
 * the DENIED case so nostr_nip46_client_connect parses it as the remote. */
static const char *WRONG_PK_SEC1 =
    "02F9308A019258C31049344F85F89D5229B531C845836F99B08601F113BCE036F9";

struct signer_ctx {
  NostrNip46Session *bunker;
  char *bunker_pk_xonly; /* 64-char lowercase-hex xonly pubkey of the signer */
};

/* Bridge the provider's sign_event call at the in-process bunker. The
 * provider hands us its owning client session, we encrypt the request with
 * it, feed the ciphertext to the bunker's handle_cipher entry point, and
 * decrypt the reply — no relay involved. */
static nh_auth_nip46_sign_status test_sign(NostrNip46Session *client,
                                           const char *unsigned_event_json,
                                           char **out_signed_event_json,
                                           void *user_data) {
  struct signer_ctx *ctx = user_data;
  *out_signed_event_json = NULL;
  if (!ctx || !ctx->bunker || !ctx->bunker_pk_xonly) return NH_AUTH_NIP46_SIGN_FAILED;

  const char *params[1] = {unsigned_event_json};
  char *req_json = nostr_nip46_request_build("nh1", "sign_event", params, 1);
  if (!req_json) return NH_AUTH_NIP46_SIGN_FAILED;

  char *cipher_req = NULL;
  if (nostr_nip46_client_nip04_encrypt(client, ctx->bunker_pk_xonly, req_json,
                                       &cipher_req) != 0 ||
      !cipher_req) {
    free(req_json);
    return NH_AUTH_NIP46_SIGN_FAILED;
  }
  free(req_json);

  /* The bunker needs the client's xonly pubkey to encrypt the reply back. */
  char *client_secret = NULL;
  if (nostr_nip46_session_get_secret(client, &client_secret) != 0 ||
      !client_secret) {
    free(cipher_req);
    return NH_AUTH_NIP46_SIGN_FAILED;
  }
  char *client_pk_x = nostr_key_get_public(client_secret);
  memset(client_secret, 0, strlen(client_secret));
  free(client_secret);
  if (!client_pk_x) {
    free(cipher_req);
    return NH_AUTH_NIP46_SIGN_FAILED;
  }

  char *cipher_reply = NULL;
  int hrc = nostr_nip46_bunker_handle_cipher(ctx->bunker, client_pk_x,
                                             cipher_req, &cipher_reply);
  free(cipher_req);
  free(client_pk_x);
  if (hrc != 0 || !cipher_reply) return NH_AUTH_NIP46_SIGN_FAILED;

  char *plain = NULL;
  if (nostr_nip46_client_nip04_decrypt(client, ctx->bunker_pk_xonly,
                                       cipher_reply, &plain) != 0 || !plain) {
    free(cipher_reply);
    return NH_AUTH_NIP46_SIGN_FAILED;
  }
  free(cipher_reply);

  NostrNip46Response resp = {0};
  int prc = nostr_nip46_response_parse(plain, &resp);
  free(plain);
  if (prc != 0) return NH_AUTH_NIP46_SIGN_FAILED;
  if (resp.error) {
    nostr_nip46_response_free(&resp);
    return NH_AUTH_NIP46_SIGN_DENIED;
  }
  if (!resp.result) {
    nostr_nip46_response_free(&resp);
    return NH_AUTH_NIP46_SIGN_FAILED;
  }
  *out_signed_event_json = strdup(resp.result);
  nostr_nip46_response_free(&resp);
  return *out_signed_event_json ? NH_AUTH_NIP46_SIGN_OK
                                : NH_AUTH_NIP46_SIGN_FAILED;
}

/* Build an in-process bunker signing with the given hex secret. Mirrors
 * nips/nip46/tests/test_bunker_sign_event_real.c. Uses NIP-04 AEAD v2 since
 * the client encrypt/decrypt helpers negotiate that with the bunker. */
static NostrNip46Session *make_bunker(const char *bunker_sk_hex,
                                      const char *client_pk_sec1) {
  NostrNip46Session *bun = nostr_nip46_bunker_new(NULL);
  if (!bun) return NULL;
  if (nostr_nip46_session_set_transport_mode(
          bun, NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION) != 0) {
    nostr_nip46_session_free(bun);
    return NULL;
  }
  char uri[256];
  snprintf(uri, sizeof uri, "bunker://%s?secret=%s", client_pk_sec1,
           bunker_sk_hex);
  if (nostr_nip46_client_connect(bun, uri, NULL) != 0) {
    nostr_nip46_session_free(bun);
    return NULL;
  }
  if (nostr_nip46_client_set_secret(bun, bunker_sk_hex) != 0) {
    nostr_nip46_session_free(bun);
    return NULL;
  }
  return bun;
}

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

/* Convert 32-byte hex string to raw bytes. */
static void hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
  for (size_t i = 0; i < out_len; i++) {
    unsigned v;
    sscanf(hex + i * 2, "%2x", &v);
    out[i] = (uint8_t)v;
  }
}

/* Seed an active account whose only enabled provider is nip46_bunker.
 * public_config_json holds a bunker:// URI pointing at ALICE_PK (the account's
 * own pubkey); secret_blob is 32 raw bytes of the client's transport key. */
static void seed(const char *dir) {
  nh_identity_store *store = open_store(dir, NH_IDENTITY_STORE_CREATE);
  nh_identity_enroll_request enroll = {0};
  enroll.username = "n_alice";
  enroll.pubkey_hex = ALICE_PK;
  enroll.home_mode = NH_IDENTITY_HOME_CREATE;
  nh_identity_operation_state state;
  const char *op_enroll = "00000000-0000-4000-8000-000000000001";
  NH_CHECK(nh_identity_operation_begin_enroll(store, op_enroll, &enroll, &state) ==
           NH_IDENTITY_OK);
  nh_identity_home_evidence staged = {11, 101}, installed = {11, 202};
  NH_CHECK(nh_identity_operation_advance_home(store, op_enroll,
             NH_IDENTITY_PHASE_RESERVED, NH_IDENTITY_PHASE_STAGED, &staged,
             &state) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_advance_home(store, op_enroll,
             NH_IDENTITY_PHASE_STAGED, NH_IDENTITY_PHASE_INSTALLED,
             &installed, &state) == NH_IDENTITY_OK);
  nh_identity_account account;
  NH_CHECK(nh_identity_store_lookup_by_name(store, "n_alice", &account) ==
           NH_IDENTITY_OK);

  char config[512];
  snprintf(config, sizeof config,
           "{\"bunker_uri\":\"bunker://%s?secret=nhtest\"}", ALICE_PK_SEC1);
  uint8_t secret_bytes[32];
  hex_to_bytes(CLIENT_SK, secret_bytes, sizeof secret_bytes);

  char provider_id[NH_IDENTITY_UUID_CAP];
  NH_CHECK(nh_identity_provider_stage(store,
             "00000000-0000-4000-8000-000000000002", account.account_id,
             NH_IDENTITY_PROVIDER_NIP46_BUNKER, 1, config, secret_bytes,
             sizeof secret_bytes, provider_id) == NH_IDENTITY_OK);

  nh_identity_proof_attestation attestation = {0};
  strcpy(attestation.pubkey_hex, ALICE_PK);
  attestation.key_generation = account.key_generation;
  NH_CHECK(nh_identity_provider_activate(store,
             "00000000-0000-4000-8000-000000000003", provider_id,
             &attestation) == NH_IDENTITY_OK);

  NH_CHECK(nh_identity_store_publish_projection(store, NULL) == NH_IDENTITY_OK);
  NH_CHECK(nh_identity_operation_activate(store, op_enroll, &state) ==
           NH_IDENTITY_OK);
  nh_identity_store_close(store);
}

/* Send BEGIN_LOGIN -> SELECT_PROVIDER("nip46") -> SUBMIT_UNLOCK on one
 * connection, returning the final broker result. The passphrase-shaped
 * "approve" token is a placeholder — the NIP-46 provider ignores its value
 * and gates approval at the bunker. */
static int client_login_nip46(int fd, const char *username,
                              nh_auth_result *result_out) {
  json_t *begin = json_object();
  json_object_set_new(begin, "username", json_string(username));
  json_object_set_new(begin, "service", json_string("gdm-password"));
  char *js = json_dumps(begin, JSON_COMPACT);
  json_decref(begin);
  nh_auth_message req = {0};
  req.operation = NH_AUTH_OP_BEGIN_LOGIN;
  memset(req.request_id, '0', NH_AUTH_REQUEST_ID_HEX_LEN);
  req.request_id[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
  req.payload_json = js;
  if (nh_auth_send_message(fd, &req) != 0) { free(js); return -1; }
  free(js);
  unsigned char *pkt = NULL; size_t pn = 0;
  if (nh_auth_recv_packet(fd, &pkt, &pn) != 0) return -1;
  nh_auth_message resp;
  int rc = nh_auth_message_parse(pkt, pn, &resp);
  free(pkt);
  if (rc != 0) return -1;
  json_error_t je;
  json_t *root = resp.payload_json ? json_loads(resp.payload_json, 0, &je) : NULL;
  json_t *rj = root ? json_object_get(root, "result") : NULL;
  const char *rn = rj && json_is_string(rj) ? json_string_value(rj) : NULL;
  if (!rn || strcmp(rn, nh_auth_result_name(NH_AUTH_RESULT_OK)) != 0) {
    for (int r = 0; r <= NH_AUTH_RESULT_INTERNAL_ERROR; r++) {
      const char *n = nh_auth_result_name((nh_auth_result)r);
      if (rn && n && !strcmp(rn, n)) { *result_out = (nh_auth_result)r; break; }
    }
    if (root) json_decref(root);
    nh_auth_message_clear(&resp);
    return 0;
  }
  json_decref(root);
  nh_auth_message_clear(&resp);

  /* SELECT_PROVIDER("nip46") */
  json_t *sel = json_object();
  json_object_set_new(sel, "provider", json_string("nip46"));
  js = json_dumps(sel, JSON_COMPACT); json_decref(sel);
  memset(&req, 0, sizeof req);
  req.operation = NH_AUTH_OP_SELECT_PROVIDER;
  memset(req.request_id, '0', NH_AUTH_REQUEST_ID_HEX_LEN);
  req.request_id[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
  req.payload_json = js;
  if (nh_auth_send_message(fd, &req) != 0) { free(js); return -1; }
  free(js);
  if (nh_auth_recv_packet(fd, &pkt, &pn) != 0) return -1;
  rc = nh_auth_message_parse(pkt, pn, &resp);
  free(pkt);
  if (rc != 0) return -1;
  root = resp.payload_json ? json_loads(resp.payload_json, 0, NULL) : NULL;
  rj = root ? json_object_get(root, "result") : NULL;
  rn = rj && json_is_string(rj) ? json_string_value(rj) : NULL;
  int keep_going = rn && (!strcmp(rn, nh_auth_result_name(NH_AUTH_RESULT_OK)) ||
                          !strcmp(rn, nh_auth_result_name(NH_AUTH_RESULT_INTERACTION_REQUIRED)));
  if (!keep_going) {
    for (int r = 0; r <= NH_AUTH_RESULT_INTERNAL_ERROR; r++) {
      const char *n = nh_auth_result_name((nh_auth_result)r);
      if (rn && n && !strcmp(rn, n)) { *result_out = (nh_auth_result)r; break; }
    }
    if (root) json_decref(root);
    nh_auth_message_clear(&resp);
    return 0;
  }
  if (root) json_decref(root);
  nh_auth_message_clear(&resp);

  /* SUBMIT_UNLOCK — placeholder secret; NIP-46 provider ignores it. */
  json_t *unlock = json_object();
  json_object_set_new(unlock, "secret", json_string("approve"));
  js = json_dumps(unlock, JSON_COMPACT); json_decref(unlock);
  memset(&req, 0, sizeof req);
  req.operation = NH_AUTH_OP_SUBMIT_UNLOCK;
  memset(req.request_id, '0', NH_AUTH_REQUEST_ID_HEX_LEN);
  req.request_id[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
  req.payload_json = js;
  if (nh_auth_send_message(fd, &req) != 0) { free(js); return -1; }
  free(js);
  if (nh_auth_recv_packet(fd, &pkt, &pn) != 0) return -1;
  rc = nh_auth_message_parse(pkt, pn, &resp);
  free(pkt);
  if (rc != 0) return -1;
  root = resp.payload_json ? json_loads(resp.payload_json, 0, NULL) : NULL;
  rj = root ? json_object_get(root, "result") : NULL;
  rn = rj && json_is_string(rj) ? json_string_value(rj) : NULL;
  int found = 0;
  if (rn) {
    for (int r = 0; r <= NH_AUTH_RESULT_INTERNAL_ERROR; r++) {
      const char *n = nh_auth_result_name((nh_auth_result)r);
      if (n && !strcmp(rn, n)) { *result_out = (nh_auth_result)r; found = 1; break; }
    }
  }
  if (root) json_decref(root);
  nh_auth_message_clear(&resp);
  return found ? 0 : -1;
}

/* Fork the broker and drive one nip46 login through it with a bunker signing
 * with `bunker_sk` and a bunker URI that names `bunker_pk_sec1` as the
 * client-side remote. The child installs the sign hook so the provider sees
 * our in-process signer instead of the real relay RPC. */
static nh_auth_result run_login(const char *dir, const char *bunker_sk,
                                const char *bunker_pk_sec1_client_side,
                                const char *bunker_pk_xonly) {
  int sv[2];
  NH_CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
  pid_t pid = fork();
  NH_CHECK(pid >= 0);
  if (pid == 0) {
    close(sv[0]);
    /* Child: set up bunker + sign hook, then serve one broker connection. */
    struct signer_ctx ctx = {0};
    ctx.bunker = make_bunker(bunker_sk, bunker_pk_sec1_client_side);
    NH_CHECK(ctx.bunker);
    ctx.bunker_pk_xonly = strdup(bunker_pk_xonly);
    NH_CHECK(ctx.bunker_pk_xonly);
    nh_auth_provider_nip46_set_sign_hook(test_sign, &ctx);

    nh_identity_store *store = open_store(dir, 0);
    nh_auth_broker *broker = nh_auth_broker_new(store);
    NH_CHECK(broker);
    (void)nh_auth_broker_handle_connection(broker, sv[1]);
    nh_auth_broker_free(broker);
    nh_identity_store_close(store);
    nh_auth_provider_nip46_set_sign_hook(NULL, NULL);
    free(ctx.bunker_pk_xonly);
    nostr_nip46_session_free(ctx.bunker);
    close(sv[1]);
    _exit(0);
  }
  close(sv[1]);
  nh_auth_result result = NH_AUTH_RESULT_INTERNAL_ERROR;
  NH_CHECK(client_login_nip46(sv[0], "n_alice", &result) == 0);
  close(sv[0]);
  int status = 0;
  NH_CHECK(waitpid(pid, &status, 0) == pid);
  return result;
}

int main(void) {
  char dir[] = "/tmp/nostr-login-nip46-XXXXXX";
  NH_CHECK(mkdtemp(dir));
  seed(dir);
  int is_root = (geteuid() == 0);

  /* OK case: bunker signs with alice's key, so the returned event pubkey
   * matches the account. Non-root: ACL denies at BEGIN_LOGIN. */
  nh_auth_result r = run_login(dir, ALICE_SK, ALICE_PK_SEC1, ALICE_PK);
  printf("matching signer -> %s\n", nh_auth_result_name(r));
  NH_CHECK(r == (is_root ? NH_AUTH_RESULT_OK : NH_AUTH_RESULT_DENIED));

  if (is_root) {
    /* Failure case: bunker signs with a different key. The provider verifies
     * pubkey against the account and rejects the proof. */
    char *wrong_pk = nostr_key_get_public(WRONG_SK);
    NH_CHECK(wrong_pk);
    r = run_login(dir, WRONG_SK, WRONG_PK_SEC1, wrong_pk);
    free(wrong_pk);
    printf("wrong-key signer -> %s\n", nh_auth_result_name(r));
    NH_CHECK(r == NH_AUTH_RESULT_INVALID_PROOF);
  }

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
