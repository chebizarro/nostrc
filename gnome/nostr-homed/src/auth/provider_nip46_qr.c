/* NIP-46 QR / nostrconnect:// provider — client-initiated pairing at the
 * greeter. Design: docs/designs/nip46-qr-login-greeter.md §4, §5.1.
 *
 * The account record for this provider carries only relay configuration
 * (public_config_json = {"mode":"nostrconnect","relays":[...],"name":"..."}
 * — all optional); the client keypair is per-login and ephemeral. The
 * provider flow:
 *
 *   prepare()   – mint an ephemeral secp256k1 keypair, a 16-byte connect
 *                 secret, and a nostrconnect:// URI; emit a DISPLAY_REQUIRED
 *                 event carrying the URI + pairing code so the broker can
 *                 hand it to the PAM module before the blocking wait.
 *   begin_proof() – stash the immutable challenge and emit APPROVAL_PENDING.
 *   submit_unlock() – start the relay pool, await an unsolicited `connect`
 *                     ack, run get_public_key (which MUST equal the account
 *                     pubkey, else DENIED and never sign), then sign_event
 *                     of the immutable challenge. Verified via the shared
 *                     strict-verify helper.
 *
 * A signer hook (nh_auth_provider_nip46_qr_set_signer_hook) lets tests bypass
 * the relay pool with an in-process bunker. Test hooks also let the test pin
 * the ephemeral key + connect secret so the URI / pairing code are
 * deterministic.
 * Tracks beads nostrc-z1fb Phase 3. */
#include "auth_provider.h"
#include "provider_nip46_verify.h"
#include "secure_buf.h"

#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_types.h"
#include "nostr/nip46/nip46_uri.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Budgets are subordinate to the broker's NH_AUTH_CHALLENGE_LIFETIME_SEC
 * (120 s → ~78 s scan window). Design §4.1. */
#define NH_NIP46_QR_START_MS   10000u
#define NH_NIP46_QR_WAIT_MS    78000u
#define NH_NIP46_QR_RPC_MS     15000u
#define NH_NIP46_QR_SIGN_MS    20000u

/* Cap on relays parsed from a per-account public_config_json list, mirrors
 * design §4.3 (max 4). */
#define NH_NIP46_QR_MAX_RELAYS 4u

/* Cap on the display JSON payload we hand to the broker. 512 covers a
 * ~230-byte URI + pairing code + hint comfortably; hard cap catches malformed
 * config with a truncated URI rather than sending a partial payload. */
#define NH_NIP46_QR_DISPLAY_MAX 2048u

typedef struct nip46_qr_provider {
  nh_auth_provider base;
  char provider_id[NH_IDENTITY_UUID_CAP];
  char account_id[NH_IDENTITY_UUID_CAP];
  char pubkey[NH_IDENTITY_PUBKEY_HEX_CAP];
  uint64_t generation;
  uint64_t deadline_monotonic_ms;

  NostrNip46Session *session;
  int session_started;
  char *uri;             /* nostrconnect://... (contains the connect secret) */
  char *client_pk_hex;   /* xonly hex of the ephemeral client keypair */
  char *connect_secret;  /* 32-char lc-hex, single-use */
  char pairing_code[10]; /* "XXXX-XXXX" */

  char *challenge_json;
  size_t challenge_json_len;
  char expected_id[65];
} nip46_qr_provider;

/* --- module-scoped state ------------------------------------------------ */
static const char *const *g_default_relays;   /* broker-installed fallback */
static size_t g_default_relays_n;

static nh_auth_nip46_qr_signer_fn g_signer_fn;
static void *g_signer_ctx;

void nh_auth_provider_nip46_qr_set_default_relays(const char *const *relays,
                                                  size_t n_relays) {
  g_default_relays = relays;
  g_default_relays_n = relays ? n_relays : 0;
}
void nh_auth_provider_nip46_qr_set_signer_hook(nh_auth_nip46_qr_signer_fn fn,
                                               void *ctx) {
  g_signer_fn = fn;
  g_signer_ctx = ctx;
}

static void emit_event(nip46_qr_provider *p, nh_auth_provider_event_type type,
                       nh_auth_result result, const void *data,
                       size_t data_len) {
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

int nh_auth_provider_nip46_qr_pairing_code(const char *client_pubkey_hex,
                                           char out[10]) {
  if (!out) return -1;
  if (!client_pubkey_hex || strlen(client_pubkey_hex) < 8) return -1;
  if (!is_lc_hex(client_pubkey_hex, 8)) return -1;
  for (int i = 0; i < 4; i++) {
    char c = client_pubkey_hex[i];
    out[i] = (char)toupper((unsigned char)c);
  }
  out[4] = '-';
  for (int i = 0; i < 4; i++) {
    char c = client_pubkey_hex[4 + i];
    out[5 + i] = (char)toupper((unsigned char)c);
  }
  out[9] = '\0';
  return 0;
}

/* Extract relays from public_config_json (may be empty). Returns 0 on success
 * with *out_relays_owned / *out_n populated; caller must free each string and
 * the array. Missing / empty list is not an error; returns 0 with *out_n=0. */
static int extract_relays(const char *config_json, char ***out_relays_owned,
                          size_t *out_n) {
  *out_relays_owned = NULL;
  *out_n = 0;
  if (!config_json || !config_json[0]) return 0;
  json_error_t e;
  json_t *root = json_loads(config_json, JSON_REJECT_DUPLICATES, &e);
  if (!root) return 0; /* tolerate bad JSON at the provider; broker falls
                          back to defaults */
  if (!json_is_object(root)) { json_decref(root); return 0; }
  json_t *arr = json_object_get(root, "relays");
  if (!arr || !json_is_array(arr)) { json_decref(root); return 0; }
  size_t idx;
  json_t *v;
  char **acc = calloc(NH_NIP46_QR_MAX_RELAYS, sizeof *acc);
  size_t n = 0;
  if (!acc) { json_decref(root); return -1; }
  json_array_foreach(arr, idx, v) {
    if (n >= NH_NIP46_QR_MAX_RELAYS) break;
    if (!json_is_string(v)) continue;
    const char *s = json_string_value(v);
    if (!s || (strncmp(s, "ws://", 5) != 0 && strncmp(s, "wss://", 6) != 0))
      continue;
    acc[n] = strdup(s);
    if (!acc[n]) { for (size_t i = 0; i < n; i++) free(acc[i]); free(acc);
                    json_decref(root); return -1; }
    n++;
  }
  json_decref(root);
  *out_relays_owned = acc;
  *out_n = n;
  return 0;
}

/* Read an optional string field from public_config_json (returns malloc'd
 * copy or NULL). */
static char *extract_string(const char *config_json, const char *key) {
  if (!config_json || !config_json[0]) return NULL;
  json_t *root = json_loads(config_json, JSON_REJECT_DUPLICATES, NULL);
  if (!root || !json_is_object(root)) { if (root) json_decref(root); return NULL; }
  json_t *v = json_object_get(root, key);
  char *out = NULL;
  if (v && json_is_string(v) && json_string_value(v))
    out = strdup(json_string_value(v));
  json_decref(root);
  return out;
}

static void release_session(nip46_qr_provider *p) {
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

static void free_relays(char **relays, size_t n) {
  if (!relays) return;
  for (size_t i = 0; i < n; i++) free(relays[i]);
  free(relays);
}

static json_t *build_display_payload(nip46_qr_provider *p, uint32_t wait_ms) {
  json_t *o = json_object();
  if (!o) return NULL;
  /* expires_at is an ABSOLUTE unix-seconds timestamp (design §5.3 + the
   * greeter-extension consumer contract in greeter-extension/README.md).
   * The gnome-shell extension compares it against
   * Math.floor(GLib.get_real_time()/1e6) and hides when
   * expires_at <= now_sec, so a relative "expires_in_ms" here would make
   * the QR look already expired. Compute the deadline from CLOCK_REALTIME
   * at publish time. expires_in_ms is kept so PAM can still drive a local
   * countdown, but the artifact/wire truth is expires_at. */
  time_t now_sec = time(NULL);
  int64_t expires_at = (int64_t)now_sec + (int64_t)(wait_ms / 1000u);
  if (json_object_set_new(o, "kind", json_string("nostrconnect")) ||
      json_object_set_new(o, "uri", json_string(p->uri)) ||
      json_object_set_new(o, "pairing_code", json_string(p->pairing_code)) ||
      json_object_set_new(o, "hint",
                          json_string("Scan this with your Nostr signer app")) ||
      json_object_set_new(o, "expires_in_ms",
                          json_integer((json_int_t)wait_ms)) ||
      json_object_set_new(o, "expires_at",
                          json_integer((json_int_t)expires_at))) {
    json_decref(o);
    return NULL;
  }
  return o;
}

static int prepare(nh_auth_provider *base,
                   const nh_auth_provider_snapshot *s) {
  nip46_qr_provider *p = (nip46_qr_provider *)base;
  if (!s || s->type != NH_IDENTITY_PROVIDER_NIP46_QR || !s->provider_id ||
      !s->account_id || !s->pubkey_hex ||
      strlen(s->provider_id) >= sizeof p->provider_id ||
      strlen(s->account_id) >= sizeof p->account_id ||
      strlen(s->pubkey_hex) != 64) {
    emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
               NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
    return -1;
  }

  /* Resolve the relay list: per-account first, then the broker-installed
   * default (auth.conf), then the compiled-in single-relay default. Design
   * §4.3. */
  char **cfg_relays = NULL;
  size_t cfg_relays_n = 0;
  (void)extract_relays(s->public_config_json, &cfg_relays, &cfg_relays_n);

  const char *default_fallback = NH_AUTH_NIP46_QR_DEFAULT_RELAY;
  const char *const *relays;
  size_t n_relays;
  if (cfg_relays_n > 0) {
    relays = (const char *const *)cfg_relays;
    n_relays = cfg_relays_n;
  } else if (g_default_relays && g_default_relays_n > 0) {
    relays = g_default_relays;
    n_relays = g_default_relays_n;
  } else {
    relays = &default_fallback;
    n_relays = 1;
  }

  char *name = extract_string(s->public_config_json, "name");
  const char *display_name = name ? name : "GNOME";

  release_session(p);
  p->session = nostr_nip46_client_new();
  if (!p->session) {
    free(name);
    free_relays(cfg_relays, cfg_relays_n);
    emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
               NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
    return -1;
  }

  char *uri = NULL;
  if (nostr_nip46_client_new_qr_session(p->session, relays, n_relays,
                                        "sign_event:1", display_name,
                                        &uri) != 0 ||
      !uri) {
    release_session(p);
    free(uri);
    free(name);
    free_relays(cfg_relays, cfg_relays_n);
    emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
               NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
    return -1;
  }

  free(name);
  free_relays(cfg_relays, cfg_relays_n);
  p->uri = uri;

  /* Populate the pairing code from the client's public key. */
  char *client_pk = NULL;
  if (nostr_nip46_session_get_client_pubkey(p->session, &client_pk) != 0 ||
      !client_pk ||
      nh_auth_provider_nip46_qr_pairing_code(client_pk, p->pairing_code) != 0) {
    free(client_pk);
    release_session(p);
    free(p->uri); p->uri = NULL;
    emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
               NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
    return -1;
  }
  p->client_pk_hex = client_pk; /* borrowed for logging; ownership taken */

  /* Retain the connect secret in RAM for constant-time compare in
   * submit_unlock. */
  char *token = NULL;
  if (nostr_nip46_session_get_connect_token(p->session, &token) == 0 && token) {
    p->connect_secret = token; /* owned */
  }

  strcpy(p->provider_id, s->provider_id);
  strcpy(p->account_id, s->account_id);
  strcpy(p->pubkey, s->pubkey_hex);
  p->generation = s->key_generation;
  p->deadline_monotonic_ms = s->deadline_monotonic_ms;

  /* Real relay path: bring the pool up now so the subscription is live
   * BEFORE we publish the URI to the greeter (design step 2, subscription-
   * before-publish). With a signer hook installed we skip the pool entirely
   * — the test harness never touches the network. */
  if (!g_signer_fn) {
    if (nostr_nip46_client_start(p->session) != 0) {
      release_session(p);
      free(p->uri); p->uri = NULL;
      free(p->client_pk_hex); p->client_pk_hex = NULL;
      free(p->connect_secret); p->connect_secret = NULL;
      emit_event(p, NH_AUTH_PROVIDER_UNAVAILABLE,
                 NH_AUTH_RESULT_PROVIDER_UNAVAILABLE, NULL, 0);
      return -1;
    }
    p->session_started = 1;
  }

  /* Emit the display payload so the broker can serialise it into the
   * SELECT_PROVIDER reply for the PAM module. */
  uint32_t wait_ms = NH_NIP46_QR_WAIT_MS;
  json_t *disp = build_display_payload(p, wait_ms);
  if (disp) {
    char *j = json_dumps(disp, JSON_COMPACT);
    json_decref(disp);
    if (j) {
      size_t jl = strlen(j);
      if (jl <= NH_NIP46_QR_DISPLAY_MAX)
        emit_event(p, NH_AUTH_PROVIDER_DISPLAY_REQUIRED, NH_AUTH_RESULT_OK, j,
                   jl);
      free(j);
    }
  }

  emit_event(p, NH_AUTH_PROVIDER_READY, NH_AUTH_RESULT_OK, NULL, 0);
  return 0;
}

static int begin_proof(nh_auth_provider *base,
                       const nh_auth_immutable_challenge *c) {
  nip46_qr_provider *p = (nip46_qr_provider *)base;
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

/* Real relay path: wait for a signer `connect`, run get_public_key, run
 * sign_event. Returns a SIGN status + heap signed_json on success. */
static nh_auth_nip46_sign_status real_signer(nip46_qr_provider *p,
                                             char **out_signed_json) {
  *out_signed_json = NULL;
  if (!p->session || !p->session_started || !p->connect_secret)
    return NH_AUTH_NIP46_SIGN_UNAVAILABLE;

  /* Step 4: await unsolicited connect. */
  char *signer_pk = NULL;
  if (nostr_nip46_client_await_connect(p->session, p->connect_secret,
                                       NH_NIP46_QR_WAIT_MS, &signer_pk) != 0 ||
      !signer_pk)
    return NH_AUTH_NIP46_SIGN_TIMEOUT;
  free(signer_pk); /* set_signer_pubkey was already done inside await */

  nostr_nip46_client_set_timeout(p->session, NH_NIP46_QR_RPC_MS);

  /* Step 5: get_public_key. MUST match the account pubkey. */
  char *user_pk = NULL;
  int gp = nostr_nip46_client_get_public_key_rpc(p->session, &user_pk);
  if (gp != 0 || !user_pk) { free(user_pk); return NH_AUTH_NIP46_SIGN_TIMEOUT; }
  int mismatch = strcmp(user_pk, p->pubkey) != 0;
  free(user_pk);
  if (mismatch) return NH_AUTH_NIP46_SIGN_DENIED;

  /* Step 6: sign_event. */
  nostr_nip46_client_set_timeout(p->session, NH_NIP46_QR_SIGN_MS);
  char *signed_json = NULL;
  int sr = nostr_nip46_client_sign_event(p->session, p->challenge_json,
                                         &signed_json);
  if (sr != 0 || !signed_json) {
    if (signed_json) { secure_wipe(signed_json, strlen(signed_json));
                       free(signed_json); }
    return NH_AUTH_NIP46_SIGN_FAILED;
  }
  *out_signed_json = signed_json;
  return NH_AUTH_NIP46_SIGN_OK;
}

static void emit_signer_status(nip46_qr_provider *p,
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
  nip46_qr_provider *p = (nip46_qr_provider *)base;
  (void)secret;
  if (secret_len > NH_AUTH_SECRET_MAX) return -1;
  if (!p->session || !p->challenge_json) return -1;

  char *signed_json = NULL;
  nh_auth_nip46_sign_status status;
  if (g_signer_fn) {
    status = g_signer_fn(p->session, p->uri, p->challenge_json, p->pubkey,
                         &signed_json, g_signer_ctx);
  } else {
    status = real_signer(p, &signed_json);
  }
  if (status != NH_AUTH_NIP46_SIGN_OK || !signed_json) {
    if (signed_json) { secure_wipe(signed_json, strlen(signed_json));
                       free(signed_json); }
    emit_signer_status(p, status == NH_AUTH_NIP46_SIGN_OK
                              ? NH_AUTH_NIP46_SIGN_FAILED
                              : status);
    return 0;
  }

  /* Strict verify — shared with the pre-paired bunker provider so a rogue
   * signer cannot swap identities under us. */
  int good = nh_nip46_verify_signed_challenge(signed_json, p->expected_id,
                                              p->pubkey);
  if (!good) {
    secure_wipe(signed_json, strlen(signed_json));
    free(signed_json);
    emit_event(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_INVALID_PROOF, NULL,
               0);
    return 0;
  }

  emit_event(p, NH_AUTH_PROVIDER_SIGNED_EVENT, NH_AUTH_RESULT_OK, signed_json,
             strlen(signed_json));
  free(signed_json);
  return 0;
}

static void cancel_op(nh_auth_provider *base) {
  nip46_qr_provider *p = (nip46_qr_provider *)base;
  if (p->session && p->session_started)
    nostr_nip46_client_cancel_all(p->session);
  emit_event(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_CANCELLED, NULL, 0);
}

static void destroy(nh_auth_provider *base) {
  nip46_qr_provider *p = (nip46_qr_provider *)base;
  if (!p) return;
  release_session(p);
  if (p->challenge_json) {
    secure_wipe(p->challenge_json, p->challenge_json_len);
    free(p->challenge_json);
  }
  if (p->uri) {
    secure_wipe(p->uri, strlen(p->uri));
    free(p->uri);
  }
  if (p->connect_secret) {
    secure_wipe(p->connect_secret, strlen(p->connect_secret));
    free(p->connect_secret);
  }
  free(p->client_pk_hex);
  secure_wipe(p, sizeof *p);
  free(p);
}

static const nh_auth_provider_ops ops = {prepare, begin_proof, submit_unlock,
                                         cancel_op, destroy};

nh_auth_provider *nh_auth_provider_nip46_qr_new(nh_auth_provider_event_fn emit,
                                                void *emit_context) {
  nip46_qr_provider *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->base.ops = &ops;
  p->base.emit = emit;
  p->base.emit_context = emit_context;
  return &p->base;
}
