#include "auth_transaction.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

struct nh_auth_transaction {
  nh_auth_authority authority;
  nh_auth_peer_snapshot peer;
  nh_identity_account account;
  nh_auth_purpose purpose;
  char transaction_id[NH_AUTH_TRANSACTION_ID_HEX_LEN + 1];
  char service[65];
  nh_identity_provider_type provider;
  nh_auth_transaction_state state;
  uint64_t deadline_ms;
  uint64_t last_now_ms;
  uint64_t challenge_deadline_ms;
  nh_auth_receipt receipt;
  int receipt_live;
};

static int peer_equal(const nh_auth_peer_snapshot *a,
                      const nh_auth_peer_snapshot *b) {
  return a && b && a->endpoint == b->endpoint && a->uid == b->uid &&
         a->gid == b->gid && a->pid == b->pid &&
         a->process_start_id == b->process_start_id &&
         CRYPTO_memcmp(a->connection_id, b->connection_id,
                       NH_AUTH_CONNECTION_ID_LEN) == 0;
}
static int terminal(nh_auth_transaction_state s) {
  return s == NH_AUTH_TX_CLOSED || s == NH_AUTH_TX_DENIED ||
         s == NH_AUTH_TX_EXPIRED || s == NH_AUTH_TX_CANCELLED ||
         s == NH_AUTH_TX_FAILED;
}
static nh_auth_transaction_rc bound(nh_auth_transaction *t,
                                    const nh_auth_peer_snapshot *p) {
  if (!t || !peer_equal(&t->peer, p))
    return NH_AUTH_TX_UNAUTHORIZED;
  if (t->state == NH_AUTH_TX_CANCELLED)
    return NH_AUTH_TX_CANCELLED_RESULT;
  if (terminal(t->state))
    return NH_AUTH_TX_BAD_STATE;
  return NH_AUTH_TX_OK;
}
static nh_auth_transaction_rc timely(nh_auth_transaction *t, uint64_t now) {
  if (now < t->last_now_ms) {
    t->state = NH_AUTH_TX_FAILED;
    t->receipt_live = 0;
    return NH_AUTH_TX_INVALID;
  }
  t->last_now_ms = now;
  if (now >= t->deadline_ms) {
    t->state = NH_AUTH_TX_EXPIRED;
    t->receipt_live = 0;
    return NH_AUTH_TX_DEADLINE;
  }
  return NH_AUTH_TX_OK;
}
static nh_auth_transaction_rc recheck(nh_auth_transaction *t) {
  nh_identity_status status = 0;
  nh_identity_account current = {0};
  nh_identity_rc r = t->authority.ops->recheck(
      t->authority.context, t->account.account_id, t->account.key_generation,
      t->account.authority_generation, &status, &current);
  if (r == NH_IDENTITY_OK)
    return NH_AUTH_TX_OK;
  if (r == NH_IDENTITY_NOT_ACTIVE) {
    t->state = NH_AUTH_TX_DENIED;
    return NH_AUTH_TX_NOT_ACTIVE;
  }
  if (r == NH_IDENTITY_STALE_GENERATION) {
    t->state = NH_AUTH_TX_DENIED;
    return NH_AUTH_TX_STALE;
  }
  t->state = NH_AUTH_TX_FAILED;
  return NH_AUTH_TX_INTERNAL;
}

nh_auth_transaction_rc nh_auth_transaction_begin(const nh_auth_authority *a,
                                                 const nh_auth_peer_snapshot *p,
                                                 const nh_auth_begin_request *r,
                                                 nh_auth_transaction **out) {
  if (out)
    *out = NULL;
  if (!a || !a->ops || !a->ops->lookup_by_name || !a->ops->lookup_by_uid ||
      !a->ops->recheck || !p || !r || !out || !r->service || !r->service[0] ||
      strlen(r->service) > 64 ||
      r->now_monotonic_ms > UINT64_MAX - (NH_AUTH_TRANSACTION_LIFETIME_SEC +
                                          NH_AUTH_RECEIPT_LIFETIME_SEC) *
                                             1000u)
    return NH_AUTH_TX_INVALID;
  nh_auth_transaction *t = calloc(1, sizeof *t);
  if (!t)
    return NH_AUTH_TX_INTERNAL;
  t->authority = *a;
  t->peer = *p;
  t->purpose = r->purpose;
  t->state = NH_AUTH_TX_NEW;
  nh_identity_rc ir;
  if (r->purpose == NH_AUTH_PURPOSE_LINUX_LOGIN) {
    if (p->endpoint != NH_AUTH_ENDPOINT_AUTH || p->uid != 0 || !r->username) {
      free(t);
      return NH_AUTH_TX_UNAUTHORIZED;
    }
    ir = a->ops->lookup_by_name(a->context, r->username, &t->account);
  } else if (r->purpose == NH_AUTH_PURPOSE_SMB_CREDENTIAL) {
    if (p->endpoint != NH_AUTH_ENDPOINT_USER) {
      free(t);
      return NH_AUTH_TX_UNAUTHORIZED;
    }
    ir = a->ops->lookup_by_uid(a->context, (uint32_t)p->uid, &t->account);
  } else {
    free(t);
    return NH_AUTH_TX_UNAUTHORIZED;
  }
  if (ir == NH_IDENTITY_NOT_FOUND) {
    free(t);
    return NH_AUTH_TX_UNKNOWN_ACCOUNT;
  }
  if (ir != NH_IDENTITY_OK) {
    free(t);
    return NH_AUTH_TX_INTERNAL;
  }
  if (t->account.status != NH_IDENTITY_STATUS_ACTIVE ||
      !t->account.enabled_providers) {
    free(t);
    return NH_AUTH_TX_NOT_ACTIVE;
  }
  if (r->purpose == NH_AUTH_PURPOSE_SMB_CREDENTIAL &&
      t->account.uid != (uint32_t)p->uid) {
    free(t);
    return NH_AUTH_TX_UNAUTHORIZED;
  }
  uint8_t random[32];
  if (RAND_bytes(random, sizeof random) != 1) {
    free(t);
    return NH_AUTH_TX_INTERNAL;
  }
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof random; i++) {
    t->transaction_id[2 * i] = hex[random[i] >> 4];
    t->transaction_id[2 * i + 1] = hex[random[i] & 15];
  }
  t->transaction_id[64] = 0;
  OPENSSL_cleanse(random, sizeof random);
  strcpy(t->service, r->service);
  t->last_now_ms = r->now_monotonic_ms;
  t->deadline_ms =
      r->now_monotonic_ms + NH_AUTH_TRANSACTION_LIFETIME_SEC * 1000u;
  t->state = NH_AUTH_TX_POLICY_CHECKED;
  *out = t;
  return NH_AUTH_TX_OK;
}
void nh_auth_transaction_free(nh_auth_transaction *t) {
  if (!t)
    return;
  OPENSSL_cleanse(t, sizeof *t);
  free(t);
}
nh_auth_transaction_state
nh_auth_transaction_get_state(const nh_auth_transaction *t) {
  return t ? t->state : 0;
}
const nh_identity_account *
nh_auth_transaction_get_account(const nh_auth_transaction *t) {
  return t ? &t->account : NULL;
}
const char *nh_auth_transaction_get_id(const nh_auth_transaction *t) {
  return t ? t->transaction_id : NULL;
}
const char *nh_auth_transaction_get_service(const nh_auth_transaction *t) {
  return t ? t->service : NULL;
}
nh_auth_purpose nh_auth_transaction_get_purpose(const nh_auth_transaction *t) {
  return t ? t->purpose : 0;
}

nh_auth_transaction_rc nh_auth_transaction_select_provider(
    nh_auth_transaction *t, const nh_auth_peer_snapshot *p,
    nh_identity_provider_type provider, uint64_t now) {
  nh_auth_transaction_rc rc = bound(t, p);
  if (rc || (rc = timely(t, now)))
    return rc;
  if (t->state != NH_AUTH_TX_POLICY_CHECKED)
    return NH_AUTH_TX_BAD_STATE;
  if (provider != NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY &&
      provider != NH_IDENTITY_PROVIDER_NIP46_BUNKER)
    return NH_AUTH_TX_INVALID;
  if (!(t->account.enabled_providers & NH_IDENTITY_PROVIDER_BIT(provider)))
    return NH_AUTH_TX_UNAUTHORIZED;
  t->provider = provider;
  t->state = provider == NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY
                 ? NH_AUTH_TX_WAITING_INPUT
                 : NH_AUTH_TX_PREPARING_PROVIDER;
  return NH_AUTH_TX_OK;
}
nh_auth_transaction_rc
nh_auth_transaction_begin_proof(nh_auth_transaction *t,
                                const nh_auth_peer_snapshot *p, uint64_t now,
                                uint64_t *out) {
  nh_auth_transaction_rc rc = bound(t, p);
  if (rc || (rc = timely(t, now)))
    return rc;
  if (t->state != NH_AUTH_TX_WAITING_INPUT &&
      t->state != NH_AUTH_TX_PREPARING_PROVIDER)
    return NH_AUTH_TX_BAD_STATE;
  uint64_t d = now + NH_AUTH_CHALLENGE_LIFETIME_SEC * 1000u;
  t->challenge_deadline_ms = d < t->deadline_ms ? d : t->deadline_ms;
  t->state = NH_AUTH_TX_WAITING_PROOF;
  if (out)
    *out = t->challenge_deadline_ms;
  return NH_AUTH_TX_OK;
}
nh_auth_transaction_rc nh_auth_transaction_start_verification(
    nh_auth_transaction *t, const nh_auth_peer_snapshot *p, uint64_t now) {
  nh_auth_transaction_rc rc = bound(t, p);
  if (rc || (rc = timely(t, now)))
    return rc;
  if (t->state == NH_AUTH_TX_VERIFIED || t->state == NH_AUTH_TX_SESSION_OPENED)
    return NH_AUTH_TX_REPLAY;
  if (t->state != NH_AUTH_TX_WAITING_PROOF)
    return NH_AUTH_TX_BAD_STATE;
  if (now >= t->challenge_deadline_ms) {
    t->state = NH_AUTH_TX_EXPIRED;
    return NH_AUTH_TX_DEADLINE;
  }
  if ((rc = recheck(t)))
    return rc;
  t->state = NH_AUTH_TX_VERIFYING;
  return NH_AUTH_TX_OK;
}
nh_auth_transaction_rc nh_auth_transaction_finish_verification(
    nh_auth_transaction *t, const nh_auth_peer_snapshot *p, uint64_t now,
    nh_auth_proof_rc proof, nh_auth_receipt *out) {
  nh_auth_transaction_rc rc = bound(t, p);
  if (rc)
    return rc;
  if (t->state == NH_AUTH_TX_VERIFIED || t->state == NH_AUTH_TX_SESSION_OPENED)
    return NH_AUTH_TX_REPLAY;
  if (t->state != NH_AUTH_TX_VERIFYING)
    return NH_AUTH_TX_BAD_STATE;
  if ((rc = timely(t, now)))
    return rc;
  if (now >= t->challenge_deadline_ms) {
    t->state = NH_AUTH_TX_EXPIRED;
    return NH_AUTH_TX_DEADLINE;
  }
  if (proof != NH_AUTH_PROOF_OK) {
    t->state = NH_AUTH_TX_FAILED;
    return NH_AUTH_TX_INVALID;
  }
  if ((rc = recheck(t)))
    return rc;
  memset(&t->receipt, 0, sizeof t->receipt);
  if (RAND_bytes(t->receipt.token, NH_AUTH_RECEIPT_LEN) != 1) {
    t->state = NH_AUTH_TX_FAILED;
    return NH_AUTH_TX_INTERNAL;
  }
  memcpy(t->receipt.connection_id, t->peer.connection_id,
         NH_AUTH_CONNECTION_ID_LEN);
  strcpy(t->receipt.account_id, t->account.account_id);
  strcpy(t->receipt.transaction_id, t->transaction_id);
  t->receipt.key_generation = t->account.key_generation;
  t->receipt.authority_generation = t->account.authority_generation;
  t->receipt.expires_monotonic_ms = now + NH_AUTH_RECEIPT_LIFETIME_SEC * 1000u;
  t->receipt_live = 1;
  t->state = NH_AUTH_TX_VERIFIED;
  if (out)
    *out = t->receipt;
  return NH_AUTH_TX_OK;
}
nh_auth_transaction_rc
nh_auth_transaction_cancel(nh_auth_transaction *t,
                           const nh_auth_peer_snapshot *p) {
  if (!t || !peer_equal(&t->peer, p))
    return NH_AUTH_TX_UNAUTHORIZED;
  if (t->state == NH_AUTH_TX_CANCELLED)
    return NH_AUTH_TX_OK;
  if (t->state == NH_AUTH_TX_CLOSED)
    return NH_AUTH_TX_BAD_STATE;
  t->receipt_live = 0;
  OPENSSL_cleanse(&t->receipt, sizeof t->receipt);
  t->state = NH_AUTH_TX_CANCELLED;
  return NH_AUTH_TX_OK;
}
nh_auth_transaction_rc
nh_auth_transaction_open_receipt(nh_auth_transaction *t,
                                 const nh_auth_peer_snapshot *p, uint64_t now,
                                 const nh_auth_receipt *r) {
  nh_auth_transaction_rc rc = bound(t, p);
  if (rc)
    return rc;
  if (t->state == NH_AUTH_TX_SESSION_OPENED)
    return NH_AUTH_TX_REPLAY;
  if (t->state != NH_AUTH_TX_VERIFIED || !t->receipt_live || !r)
    return NH_AUTH_TX_BAD_STATE;
  if (now < t->last_now_ms) {
    t->receipt_live = 0;
    t->state = NH_AUTH_TX_FAILED;
    return NH_AUTH_TX_INVALID;
  }
  t->last_now_ms = now;
  if (now >= t->receipt.expires_monotonic_ms) {
    t->receipt_live = 0;
    t->state = NH_AUTH_TX_EXPIRED;
    return NH_AUTH_TX_DEADLINE;
  }
  if (CRYPTO_memcmp(r->token, t->receipt.token, NH_AUTH_RECEIPT_LEN) ||
      CRYPTO_memcmp(r->connection_id, t->peer.connection_id,
                    NH_AUTH_CONNECTION_ID_LEN) ||
      CRYPTO_memcmp(r->account_id, t->account.account_id,
                    sizeof r->account_id) ||
      CRYPTO_memcmp(r->transaction_id, t->transaction_id,
                    sizeof r->transaction_id) ||
      r->key_generation != t->account.key_generation ||
      r->authority_generation != t->account.authority_generation ||
      r->expires_monotonic_ms != t->receipt.expires_monotonic_ms)
    return NH_AUTH_TX_UNAUTHORIZED;
  if ((rc = recheck(t)))
    return rc;
  t->receipt_live = 0;
  OPENSSL_cleanse(t->receipt.token, sizeof t->receipt.token);
  t->state = NH_AUTH_TX_SESSION_OPENED;
  return NH_AUTH_TX_OK;
}
nh_auth_transaction_rc
nh_auth_transaction_close_session(nh_auth_transaction *t,
                                  const nh_auth_peer_snapshot *p) {
  nh_auth_transaction_rc rc = bound(t, p);
  if (rc)
    return rc;
  if (t->state != NH_AUTH_TX_SESSION_OPENED)
    return NH_AUTH_TX_BAD_STATE;
  t->state = NH_AUTH_TX_CLOSED;
  return NH_AUTH_TX_OK;
}
