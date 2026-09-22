/* NIP-46 external-signer provider for the nostr-homed login runtime.
 *
 * Implements the nh_auth_provider ops for accounts backed by a NIP-46 remote
 * "bunker". The provider owns a NostrNip46Session that is configured from the
 * account's provider record: the bunker:// URI comes from public_config_json
 * ({"bunker_uri":"..."}) and the client transport key comes from secret_blob
 * (raw 32 bytes or 64-char lowercase-hex, optionally NUL-terminated).
 *
 * The signed proof is produced by calling nostr_nip46_client_sign_event on
 * the broker's immutable challenge JSON, which the remote signer approves and
 * returns as a complete signed nostr event. The provider verifies id and
 * pubkey against the account before emitting NH_AUTH_PROVIDER_SIGNED_EVENT.
 * A remote failure is mapped to DENIED / INTERACTION_REQUIRED / UNAVAILABLE
 * / FAILED per the provider event contract.
 *
 * A test-only sign hook (nh_auth_provider_nip46_set_sign_hook) lets headless
 * tests inject an in-process bunker without spinning up real relays. When the
 * hook is unset, the provider uses the real relay-backed RPC path.
 * Tracks beads nostrc-ot2c.
 */
#include "auth_provider.h"
#include "secure_buf.h"
#include "nostr-event.h"
#include "provider_nip46_verify.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_types.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

/* Bounded time for the transport RPC. The broker also enforces a challenge
 * deadline; this cap keeps the sync sign_event call from hanging forever if
 * the bunker never replies. */
#define NH_NIP46_RPC_TIMEOUT_MS 20000u

typedef struct nip46_provider {
  nh_auth_provider base;
  char provider_id[NH_IDENTITY_UUID_CAP];
  char account_id[NH_IDENTITY_UUID_CAP];
  char pubkey[NH_IDENTITY_PUBKEY_HEX_CAP];
  uint64_t generation;
  NostrNip46Session *session;
  int session_started;
  nostr_secure_buf secret_hex; /* 64-char lowercase hex + '\0' */
  char *challenge_json;
  size_t challenge_json_len;
  char expected_id[65];
  uint64_t deadline_monotonic_ms;
} nip46_provider;

/* Test-only global sign hook. Default (NULL) uses the real client RPC. */
static nh_auth_nip46_sign_fn g_sign_fn;
static void *g_sign_ctx;

void nh_auth_provider_nip46_set_sign_hook(nh_auth_nip46_sign_fn fn, void *ctx) {
  g_sign_fn = fn;
  g_sign_ctx = ctx;
}

static void emit_event(nip46_provider *p, nh_auth_provider_event_type type,
                       nh_auth_result result, const void *data, size_t data_len) {
  nh_auth_provider_event event = {type, result, data, data_len};
  if (p->base.emit) p->base.emit(p->base.emit_context, &event);
}

static int is_lc_hex(const char *s, size_t n) {
  if (!s) return 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return 1;
}

/* Accept the client transport secret as either 32 raw bytes, a 64-char
 * lowercase-hex blob, or a 65-byte NUL-terminated lowercase-hex string.
 * Copy into a mlocked buffer we own for the lifetime of the session. */
static int load_secret_hex(nip46_provider *p, const uint8_t *blob, size_t len) {
  static const char digits[] = "0123456789abcdef";
  if (!blob) return -1;
  nostr_secure_buf buf = secure_alloc(65);
  if (!buf.ptr) return -1;
  char *dst = (char *)buf.ptr;
  if (len == 32) {
    for (size_t i = 0; i < 32; i++) {
      dst[i * 2] = digits[blob[i] >> 4];
      dst[i * 2 + 1] = digits[blob[i] & 0xf];
    }
    dst[64] = '\0';
  } else if (len == 64 && is_lc_hex((const char *)blob, 64)) {
    memcpy(dst, blob, 64);
    dst[64] = '\0';
  } else if (len == 65 && blob[64] == '\0' &&
             is_lc_hex((const char *)blob, 64)) {
    memcpy(dst, blob, 65);
  } else {
    secure_free(&buf);
    return -1;
  }
  secure_free(&p->secret_hex);
  p->secret_hex = buf;
  return 0;
}

/* Extract the bunker URI from public_config_json ({"bunker_uri":"..."}). The
 * returned string is owned by the jansson root; caller must keep root alive. */
static const char *extract_bunker_uri(const char *config_json,
                                      json_t **root_out) {
  if (!config_json || !config_json[0]) return NULL;
  json_error_t e;
  json_t *root = json_loads(config_json, JSON_REJECT_DUPLICATES, &e);
  if (!root || !json_is_object(root)) {
    if (root) json_decref(root);
    return NULL;
  }
  json_t *v = json_object_get(root, "bunker_uri");
  if (!v || !json_is_string(v)) {
    json_decref(root);
    return NULL;
  }
  const char *uri = json_string_value(v);
  if (!uri || strncmp(uri, "bunker://", 9) != 0) {
    json_decref(root);
    return NULL;
  }
  *root_out = root;
  return uri;
}

/* Default sign path: use the real relay-backed NIP-46 RPC. */
static nh_auth_nip46_sign_status real_sign(NostrNip46Session *s,
                                           const char *event_json,
                                           char **out_signed_event_json,
                                           void *ctx) {
  (void)ctx;
  int rc = nostr_nip46_client_sign_event(s, event_json, out_signed_event_json);
  return rc == 0 ? NH_AUTH_NIP46_SIGN_OK : NH_AUTH_NIP46_SIGN_FAILED;
}

static void release_session(nip46_provider *p) {
  if (p->session) {
    if (p->session_started) {
      nostr_nip46_client_cancel_all(p->session);
      nostr_nip46_client_stop(p->session);
      p->session_started = 0;
    }
    nostr_nip46_session_free(p->session);
    p->session = NULL;
  }
}

static int prepare(nh_auth_provider *base, const nh_auth_provider_snapshot *s) {
  nip46_provider *p = (nip46_provider *)base;
  if (!s || s->type != NH_IDENTITY_PROVIDER_NIP46_BUNKER ||
      !s->provider_id || !s->account_id || !s->pubkey_hex ||
      strlen(s->provider_id) >= sizeof p->provider_id ||
      strlen(s->account_id) >= sizeof p->account_id ||
      strlen(s->pubkey_hex) != 64 ||
      !s->public_config_json || !s->secret_blob || !s->secret_blob_len) {
    emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
               NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
    return -1;
  }

  json_t *cfg_root = NULL;
  const char *bunker_uri = extract_bunker_uri(s->public_config_json, &cfg_root);
  if (!bunker_uri) {
    emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
               NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
    return -1;
  }

  if (load_secret_hex(p, s->secret_blob, s->secret_blob_len) != 0) {
    if (cfg_root) json_decref(cfg_root);
    emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
               NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
    return -1;
  }

  release_session(p);
  p->session = nostr_nip46_client_new();
  int ok = p->session != NULL;
  /* Install the client transport secret BEFORE parsing the bunker URI:
   * nostr_nip46_client_connect() auto-generates a fresh transport key when
   * the session doesn't already carry one (the URI never supplies it), and
   * that generation can fail transiently (e.g. RAND_bytes not yet seeded on
   * macOS after other libnostr subsystems have started). By pre-seeding the
   * secret, connect skips the generation entirely and cannot fail on that
   * path. */
  if (ok)
    ok = nostr_nip46_client_set_secret(p->session,
                                       (const char *)p->secret_hex.ptr) == 0;
  if (ok) ok = nostr_nip46_client_connect(p->session, bunker_uri, NULL) == 0;
  if (cfg_root) json_decref(cfg_root);
  if (!ok) {
    release_session(p);
    emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
               NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
    return -1;
  }

  nostr_nip46_client_set_timeout(p->session, NH_NIP46_RPC_TIMEOUT_MS);

  strcpy(p->provider_id, s->provider_id);
  strcpy(p->account_id, s->account_id);
  strcpy(p->pubkey, s->pubkey_hex);
  p->generation = s->key_generation;
  p->deadline_monotonic_ms = s->deadline_monotonic_ms;

  /* Only spin the persistent relay pool for the real RPC path. Tests
   * short-circuit the sign step and never touch the network, so leave the
   * session state at CONNECTING and avoid a socket that never resolves. */
  if (!g_sign_fn) {
    if (nostr_nip46_client_start(p->session) != 0) {
      release_session(p);
      emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
                 NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
      return -1;
    }
    p->session_started = 1;

    /* Real bunkers gate sign_event behind an explicit `connect` RPC that
     * authorises the transport pubkey and requested permissions. The URI's
     * `secret=` param is the authorisation token; passing NULL here lets
     * the client library reuse the parsed token. Failure here means the
     * bunker will not honour a subsequent sign_event, so surface it as
     * UNAVAILABLE rather than proceeding to a doomed proof.
     *
     * The auth challenge is kind NH_AUTH_CHALLENGE_KIND. Some bunkers apply
     * a policy engine that permits sign_event only for whitelisted kinds
     * (returning `policy.default_deny` on anything else); ask explicitly
     * for the challenge kind so a kind-scoped ACL can grant it without
     * unlocking arbitrary-kind signing. Bunkers that do not understand the
     * `sign_event:<kind>` extension still see `sign_event` and either grant
     * blanket sign or deny — the outcome is unchanged. */
    char *connect_result = NULL;
    int connect_rc = nostr_nip46_client_connect_rpc(
        p->session, NULL, "sign_event,sign_event:1", &connect_result);
    if (connect_result) {
      secure_wipe(connect_result, strlen(connect_result));
      free(connect_result);
    }
    if (connect_rc != 0) {
      release_session(p);
      emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
                 NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
      return -1;
    }
  }

  emit_event(p, NH_AUTH_PROVIDER_READY, NH_AUTH_RESULT_OK, NULL, 0);
  return 0;
}

static int begin_proof(nh_auth_provider *base,
                       const nh_auth_immutable_challenge *c) {
  nip46_provider *p = (nip46_provider *)base;
  if (!c || !c->unsigned_event_json || !c->expected_event_id ||
      c->unsigned_event_json_len == 0 ||
      c->unsigned_event_json_len > NH_AUTH_PROOF_MAX ||
      strlen(c->expected_event_id) != 64)
    return -1;
  char *j = malloc(c->unsigned_event_json_len + 1);
  if (!j) return -1;
  memcpy(j, c->unsigned_event_json, c->unsigned_event_json_len);
  j[c->unsigned_event_json_len] = '\0';
  if (p->challenge_json) {
    secure_wipe(p->challenge_json, p->challenge_json_len);
    free(p->challenge_json);
  }
  p->challenge_json = j;
  p->challenge_json_len = c->unsigned_event_json_len;
  memcpy(p->expected_id, c->expected_event_id, 64);
  p->expected_id[64] = '\0';
  if (c->deadline_monotonic_ms)
    p->deadline_monotonic_ms = c->deadline_monotonic_ms;
  emit_event(p, NH_AUTH_PROVIDER_APPROVAL_PENDING,
             NH_AUTH_RESULT_INTERACTION_REQUIRED, NULL, 0);
  return 0;
}

static void emit_signer_status(nip46_provider *p,
                               nh_auth_nip46_sign_status status) {
  switch (status) {
    case NH_AUTH_NIP46_SIGN_DENIED:
      emit_event(p, NH_AUTH_PROVIDER_DENIED, NH_AUTH_RESULT_DENIED, NULL, 0);
      return;
    case NH_AUTH_NIP46_SIGN_TIMEOUT:
      emit_event(p, NH_AUTH_PROVIDER_INTERACTION_REQUIRED,
                 NH_AUTH_RESULT_INTERACTION_REQUIRED, NULL, 0);
      return;
    case NH_AUTH_NIP46_SIGN_UNAVAILABLE:
      emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
                 NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
      return;
    case NH_AUTH_NIP46_SIGN_FAILED:
    default:
      emit_event(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_INVALID_PROOF,
                 NULL, 0);
      return;
  }
}

static int submit_unlock(nh_auth_provider *base, const uint8_t *secret,
                         size_t secret_len) {
  nip46_provider *p = (nip46_provider *)base;
  /* The NIP-46 flow authorizes at the remote signer, not with a broker-side
   * secret. Accept any well-bounded input; the value itself is ignored. */
  (void)secret;
  if (secret_len > NH_AUTH_SECRET_MAX) return -1;
  if (!p->session || !p->challenge_json) return -1;

  nh_auth_nip46_sign_fn sign = g_sign_fn ? g_sign_fn : real_sign;
  char *signed_json = NULL;
  nh_auth_nip46_sign_status status =
      sign(p->session, p->challenge_json, &signed_json, g_sign_ctx);
  if (status != NH_AUTH_NIP46_SIGN_OK || !signed_json) {
    if (signed_json) {
      secure_wipe(signed_json, strlen(signed_json));
      free(signed_json);
    }
    emit_signer_status(p, status == NH_AUTH_NIP46_SIGN_OK
                              ? NH_AUTH_NIP46_SIGN_FAILED
                              : status);
    return 0;
  }

  /* Strict verify: parse the returned event, recompute the id and confirm
   * it matches the challenge we asked to be signed, and confirm the pubkey
   * is the account we authenticated. Anything else is treated as
   * INVALID_PROOF so a rogue bunker cannot swap identities under us. The
   * broker's nh_auth_challenge_verify re-checks the full binding once we
   * hand the signed event back. Shared with the QR provider — see
   * provider_nip46_verify.[ch]. */
  int good = nh_nip46_verify_signed_challenge(signed_json, p->expected_id,
                                              p->pubkey);
  if (!good) {
    secure_wipe(signed_json, strlen(signed_json));
    free(signed_json);
    emit_event(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_INVALID_PROOF,
               NULL, 0);
    return 0;
  }

  emit_event(p, NH_AUTH_PROVIDER_SIGNED_EVENT, NH_AUTH_RESULT_OK, signed_json,
             strlen(signed_json));
  free(signed_json);
  return 0;
}

static void cancel_op(nh_auth_provider *base) {
  nip46_provider *p = (nip46_provider *)base;
  if (p->session && p->session_started)
    nostr_nip46_client_cancel_all(p->session);
  emit_event(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_CANCELLED, NULL, 0);
}

static void destroy(nh_auth_provider *base) {
  nip46_provider *p = (nip46_provider *)base;
  if (!p) return;
  release_session(p);
  if (p->challenge_json) {
    secure_wipe(p->challenge_json, p->challenge_json_len);
    free(p->challenge_json);
  }
  secure_free(&p->secret_hex);
  secure_wipe(p, sizeof *p);
  free(p);
}

static const nh_auth_provider_ops ops = {prepare, begin_proof, submit_unlock,
                                         cancel_op, destroy};

nh_auth_provider *nh_auth_provider_nip46_new(nh_auth_provider_event_fn emit_fn,
                                             void *emit_context) {
  nip46_provider *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->base.ops = &ops;
  p->base.emit = emit_fn;
  p->base.emit_context = emit_context;
  return &p->base;
}
