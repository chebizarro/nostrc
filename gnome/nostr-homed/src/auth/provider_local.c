#include "auth_provider.h"
#include "auth_vault.h"
#include "auth_worker.h"
#include "nostr-event.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Local encrypted-key provider. The expensive scrypt+AES-GCM vault unlock
 * (N=262144, ~hundreds of ms, ~300MB) plus the follow-up event sign now
 * runs in a supervised worker subprocess so a hostile or slow unlock
 * cannot block the broker's main context. See auth_worker.{h,c} and
 * beads nostrc-zcll.2.
 *
 * Contract preserved by the refactor:
 *   - submit_unlock still emits exactly one of SIGNED_EVENT, DENIED, or
 *     FAILED via the provider's event callback before it returns.
 *   - The private key never leaves the child; only the signed JSON
 *     event crosses the pipe back to the parent.
 *   - The passphrase copy in the child is wiped; the parent's copy is
 *     wiped by the worker runtime after being sent.
 */

/* Hard cap on the scrypt worker's wall time. Real scrypt with the tuned
 * parameters completes in well under a second on the broker host; give a
 * generous ceiling so slow VMs don't false-timeout, but small enough that
 * a stuck child is reaped in reasonable time. */
#define NH_LOCAL_WORKER_TIMEOUT_MS 15000u
/* Maximum bytes we accept back from the child (the signed event JSON is
 * bounded by NH_AUTH_PROOF_MAX). */
#define NH_LOCAL_WORKER_OUTPUT_MAX NH_AUTH_PROOF_MAX

/* Child exit codes. 0 = success (signed event written). Non-zero values
 * are mapped by the parent to DENIED/FAILED. */
#define NH_LOCAL_CHILD_OK        0
#define NH_LOCAL_CHILD_DENIED    2  /* wrong passphrase (vault unlock failed) */
#define NH_LOCAL_CHILD_PROOF_BAD 3  /* signed event failed to build/validate */
#define NH_LOCAL_CHILD_VAULT_ERR 4  /* vault error other than wrong password */
#define NH_LOCAL_CHILD_INTERNAL  5  /* OOM / malformed payload / setup */

typedef struct local_provider {
  nh_auth_provider base;
  char provider_id[NH_IDENTITY_UUID_CAP];
  char account_id[NH_IDENTITY_UUID_CAP];
  char pubkey[NH_IDENTITY_PUBKEY_HEX_CAP];
  uint64_t generation;
  uint8_t *blob;
  size_t blob_len;
  char *challenge_json;
  char expected_id[65];
} local_provider;

static void emit(local_provider *p, nh_auth_provider_event_type type,
                 nh_auth_result result, const void *d, size_t n) {
  nh_auth_provider_event e = {type, result, d, n};
  if (p->base.emit) p->base.emit(p->base.emit_context, &e);
}

/* -------- wire format for parent<->child payload ----------------------- */

/* Little-endian pack/unpack of a fixed header, followed by variable-length
 * fields concatenated in the order declared. Kept local to this file. */
typedef struct payload_hdr {
  uint32_t blob_len;
  uint32_t pass_len;
  uint32_t challenge_json_len;
  uint32_t provider_id_len;
  uint32_t account_id_len;
  uint32_t pubkey_hex_len;
  uint32_t expected_id_len;
  uint64_t key_generation;
} payload_hdr;

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) {
  return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}
#define PAYLOAD_HDR_WIRE_LEN 36u

/* -------- child worker entry ------------------------------------------- */

/* Runs in the forked child. Reads a packed payload, does vault_open +
 * event sign, writes the signed event JSON to out_fd. Exits with a code
 * that the parent maps to a provider event. */
static int local_child_unlock_sign(const uint8_t *input, size_t input_len,
                                   int out_fd, void *user_data) {
  (void)user_data;
  if (!input || input_len < PAYLOAD_HDR_WIRE_LEN) return NH_LOCAL_CHILD_INTERNAL;

  payload_hdr h;
  h.blob_len            = get32(input +  0);
  h.pass_len            = get32(input +  4);
  h.challenge_json_len  = get32(input +  8);
  h.provider_id_len     = get32(input + 12);
  h.account_id_len      = get32(input + 16);
  h.pubkey_hex_len      = get32(input + 20);
  h.expected_id_len     = get32(input + 24);
  h.key_generation      = get64(input + 28);

  /* Overflow-safe sum of the section lengths. */
  uint64_t total = (uint64_t)PAYLOAD_HDR_WIRE_LEN
    + h.blob_len + h.pass_len + h.challenge_json_len
    + h.provider_id_len + h.account_id_len
    + h.pubkey_hex_len + h.expected_id_len;
  if (total != input_len) return NH_LOCAL_CHILD_INTERNAL;
  if (h.pubkey_hex_len != 64 || h.expected_id_len != 64)
    return NH_LOCAL_CHILD_INTERNAL;

  const uint8_t *p = input + PAYLOAD_HDR_WIRE_LEN;
  const uint8_t *blob    = p; p += h.blob_len;
  const uint8_t *pass    = p; p += h.pass_len;
  const uint8_t *cj      = p; p += h.challenge_json_len;
  const uint8_t *pid     = p; p += h.provider_id_len;
  const uint8_t *aid     = p; p += h.account_id_len;
  const uint8_t *pk      = p; p += h.pubkey_hex_len;
  const uint8_t *eid     = p; p += h.expected_id_len;

  /* Zero-copy is fine for blob (public). For strings that the binding
   * and event code expect NUL-terminated, we allocate small stack-ish
   * copies. UUID/pubkey/expected_id are bounded. challenge_json is
   * bounded by NH_AUTH_PROOF_MAX; use malloc. */
  char provider_id[NH_IDENTITY_UUID_CAP] = {0};
  char account_id[NH_IDENTITY_UUID_CAP] = {0};
  char pubkey_hex[NH_IDENTITY_PUBKEY_HEX_CAP] = {0};
  char expected_id[65] = {0};
  if (h.provider_id_len >= sizeof provider_id ||
      h.account_id_len  >= sizeof account_id)
    return NH_LOCAL_CHILD_INTERNAL;
  memcpy(provider_id, pid, h.provider_id_len);
  memcpy(account_id,  aid, h.account_id_len);
  memcpy(pubkey_hex,  pk,  h.pubkey_hex_len);
  memcpy(expected_id, eid, h.expected_id_len);

  char *cj_z = malloc((size_t)h.challenge_json_len + 1);
  if (!cj_z) return NH_LOCAL_CHILD_INTERNAL;
  memcpy(cj_z, cj, h.challenge_json_len);
  cj_z[h.challenge_json_len] = 0;

  /* Vault unlock -- the expensive scrypt+AES-GCM step. */
  nh_auth_vault_binding binding = { provider_id, account_id, pubkey_hex,
                                    h.key_generation };
  nostr_secure_buf key = {0};
  nh_auth_vault_rc vr = nh_auth_vault_open(blob, h.blob_len, pass, h.pass_len,
                                           &binding, &key);
  /* We hold pass via a read-only pointer into the caller's buffer, but
   * the runtime will wipe that whole input buffer for us on exit. */
  if (vr != NH_AUTH_VAULT_OK) {
    free(cj_z);
    secure_free(&key);
    if (vr == NH_AUTH_VAULT_UNLOCK_FAILED) return NH_LOCAL_CHILD_DENIED;
    return NH_LOCAL_CHILD_VAULT_ERR;
  }

  /* Build and sign the challenge event entirely in the child; only the
   * signed JSON crosses back to the parent. */
  NostrEvent *e = nostr_event_new();
  char id[65];
  char *out = NULL;
  int ok = e &&
    nostr_event_deserialize_unsigned(e, cj_z, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_compute_id(e, id) == NOSTR_EVENT_VALIDATION_OK &&
    strcmp(id, expected_id) == 0 &&
    e->pubkey && strcmp(e->pubkey, pubkey_hex) == 0 &&
    nostr_event_sign_secure(e, &key) == 0 &&
    nostr_event_validate(e, NULL) == NOSTR_EVENT_VALIDATION_OK;
  secure_free(&key);
  if (ok) out = nostr_event_serialize_compact(e);
  nostr_event_free(e);
  free(cj_z);
  if (!out) return NH_LOCAL_CHILD_PROOF_BAD;

  size_t out_len = strlen(out);
  int wrc = 0;
  size_t off = 0;
  while (off < out_len) {
    ssize_t n = write(out_fd, out + off, out_len - off);
    if (n < 0) { wrc = -1; break; }
    if (n == 0) { wrc = -1; break; }
    off += (size_t)n;
  }
  /* out was a plain public JSON blob; free normally. */
  free(out);
  if (wrc != 0) return NH_LOCAL_CHILD_INTERNAL;
  return NH_LOCAL_CHILD_OK;
}

/* -------- provider ops ------------------------------------------------- */

static int prepare(nh_auth_provider *b, const nh_auth_provider_snapshot *s) {
  local_provider *p = (local_provider *)b;
  if (!s || s->type != NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY ||
      !s->provider_id || !s->account_id || !s->pubkey_hex ||
      strlen(s->provider_id) >= sizeof p->provider_id ||
      strlen(s->account_id) >= sizeof p->account_id ||
      strlen(s->pubkey_hex) != 64 || !s->secret_blob || !s->secret_blob_len)
    return -1;
  uint8_t *copy = malloc(s->secret_blob_len);
  if (!copy) return -1;
  memcpy(copy, s->secret_blob, s->secret_blob_len);
  free(p->blob);
  p->blob = copy;
  p->blob_len = s->secret_blob_len;
  strcpy(p->provider_id, s->provider_id);
  strcpy(p->account_id, s->account_id);
  strcpy(p->pubkey, s->pubkey_hex);
  p->generation = s->key_generation;
  emit(p, NH_AUTH_PROVIDER_READY, NH_AUTH_RESULT_OK, NULL, 0);
  return 0;
}

static int begin_proof(nh_auth_provider *b, const nh_auth_immutable_challenge *c) {
  local_provider *p = (local_provider *)b;
  if (!c || !c->unsigned_event_json || !c->expected_event_id ||
      c->unsigned_event_json_len > NH_AUTH_PROOF_MAX ||
      strlen(c->expected_event_id) != 64) return -1;
  char *j = malloc(c->unsigned_event_json_len + 1);
  if (!j) return -1;
  memcpy(j, c->unsigned_event_json, c->unsigned_event_json_len);
  j[c->unsigned_event_json_len] = 0;
  free(p->challenge_json);
  p->challenge_json = j;
  strcpy(p->expected_id, c->expected_event_id);
  emit(p, NH_AUTH_PROVIDER_UNLOCK_REQUIRED, NH_AUTH_RESULT_INTERACTION_REQUIRED,
       NULL, 0);
  return 0;
}

static int submit(nh_auth_provider *b, const uint8_t *secret, size_t n) {
  local_provider *p = (local_provider *)b;
  if (!p->blob || !p->challenge_json || !secret || n > NH_AUTH_SECRET_MAX)
    return -1;

  /* Build the packed payload in a secure buffer so the passphrase is
   * mlock()ed and wiped on free. The worker runtime also wipes this
   * buffer after sending it to the child. */
  size_t cj_len = strlen(p->challenge_json);
  size_t pid_len = strlen(p->provider_id);
  size_t aid_len = strlen(p->account_id);
  size_t pk_len  = strlen(p->pubkey);
  size_t eid_len = strlen(p->expected_id);
  size_t total = PAYLOAD_HDR_WIRE_LEN + p->blob_len + n + cj_len +
                 pid_len + aid_len + pk_len + eid_len;
  if (total > NH_AUTH_PACKET_MAX) {
    emit(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_INTERNAL_ERROR, NULL, 0);
    return -1;
  }
  nostr_secure_buf payload = secure_alloc(total);
  if (!payload.ptr) {
    emit(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_INTERNAL_ERROR, NULL, 0);
    return -1;
  }
  uint8_t *w = (uint8_t *)payload.ptr;
  put32(w +  0, (uint32_t)p->blob_len);
  put32(w +  4, (uint32_t)n);
  put32(w +  8, (uint32_t)cj_len);
  put32(w + 12, (uint32_t)pid_len);
  put32(w + 16, (uint32_t)aid_len);
  put32(w + 20, (uint32_t)pk_len);
  put32(w + 24, (uint32_t)eid_len);
  put64(w + 28, p->generation);
  uint8_t *cur = w + PAYLOAD_HDR_WIRE_LEN;
  memcpy(cur, p->blob, p->blob_len); cur += p->blob_len;
  memcpy(cur, secret, n); cur += n;
  memcpy(cur, p->challenge_json, cj_len); cur += cj_len;
  memcpy(cur, p->provider_id, pid_len); cur += pid_len;
  memcpy(cur, p->account_id, aid_len); cur += aid_len;
  memcpy(cur, p->pubkey, pk_len); cur += pk_len;
  memcpy(cur, p->expected_id, eid_len);

  uint8_t *out = NULL;
  size_t out_len = 0;
  nh_auth_worker_result wr = {0};
  nh_auth_worker_status ws = nh_auth_worker_run(
      local_child_unlock_sign, NULL,
      (uint8_t *)payload.ptr, total,
      NH_LOCAL_WORKER_TIMEOUT_MS, NH_LOCAL_WORKER_OUTPUT_MAX,
      &out, &out_len, &wr);
  /* nh_auth_worker_run wiped the payload buffer after send. Freeing it
   * still safely re-wipes and munlocks. */
  secure_free(&payload);

  switch (ws) {
    case NH_AUTH_WORKER_OK:
      if (out && out_len > 0 && out_len <= NH_AUTH_PROOF_MAX) {
        emit(p, NH_AUTH_PROVIDER_SIGNED_EVENT, NH_AUTH_RESULT_OK, out, out_len);
      } else {
        emit(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_INVALID_PROOF, NULL, 0);
      }
      free(out);
      return 0;
    case NH_AUTH_WORKER_FAILED:
      free(out);
      if (wr.exit_code == NH_LOCAL_CHILD_DENIED) {
        /* Wrong passphrase -- surface as DENIED, exactly like the
         * pre-worker path did on NH_AUTH_VAULT_UNLOCK_FAILED. */
        emit(p, NH_AUTH_PROVIDER_DENIED, NH_AUTH_RESULT_DENIED, NULL, 0);
      } else if (wr.exit_code == NH_LOCAL_CHILD_PROOF_BAD) {
        emit(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_INVALID_PROOF, NULL, 0);
      } else {
        emit(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_INTERNAL_ERROR, NULL, 0);
      }
      return 0;
    case NH_AUTH_WORKER_TIMEOUT:
      free(out);
      emit(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_EXPIRED, NULL, 0);
      return 0;
    case NH_AUTH_WORKER_BUSY:
      /* Placeholder single-slot guard: broker is single-flight today so
       * this is not reachable, but map it to PROVIDER_UNAVAILABLE for
       * when the concurrent broker lands. */
      free(out);
      emit(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_PROVIDER_UNAVAILABLE,
           NULL, 0);
      return 0;
    case NH_AUTH_WORKER_INTERNAL:
    default:
      free(out);
      emit(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_INTERNAL_ERROR, NULL, 0);
      return 0;
  }
}

static void cancel(nh_auth_provider *b) {
  local_provider *p = (local_provider *)b;
  emit(p, NH_AUTH_PROVIDER_FAILED, NH_AUTH_RESULT_CANCELLED, NULL, 0);
}

static void destroy(nh_auth_provider *b) {
  local_provider *p = (local_provider *)b;
  if (!p) return;
  if (p->blob) {
    secure_wipe(p->blob, p->blob_len);
    free(p->blob);
  }
  if (p->challenge_json) {
    secure_wipe(p->challenge_json, strlen(p->challenge_json));
    free(p->challenge_json);
  }
  secure_wipe(p, sizeof *p);
  free(p);
}

static const nh_auth_provider_ops ops = {prepare, begin_proof, submit, cancel, destroy};

nh_auth_provider *nh_auth_provider_local_new(nh_auth_provider_event_fn fn, void *ctx) {
  local_provider *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->base.ops = &ops;
  p->base.emit = fn;
  p->base.emit_context = ctx;
  return &p->base;
}
