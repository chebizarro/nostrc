#ifndef NH_AUTH_TRANSACTION_H
#define NH_AUTH_TRANSACTION_H

#include "auth_challenge.h"
#include "nostr_auth_protocol.h"
#include "nostr_identity.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NH_AUTH_CONNECTION_ID_LEN 32u
#define NH_AUTH_RECEIPT_LEN 32u

typedef enum nh_auth_transaction_state {
  NH_AUTH_TX_NEW = 1,
  NH_AUTH_TX_POLICY_CHECKED,
  NH_AUTH_TX_WAITING_INPUT,
  NH_AUTH_TX_PREPARING_PROVIDER,
  NH_AUTH_TX_WAITING_PROOF,
  NH_AUTH_TX_VERIFYING,
  NH_AUTH_TX_VERIFIED,
  NH_AUTH_TX_SESSION_OPENED,
  NH_AUTH_TX_CLOSED,
  NH_AUTH_TX_DENIED,
  NH_AUTH_TX_EXPIRED,
  NH_AUTH_TX_CANCELLED,
  NH_AUTH_TX_FAILED
} nh_auth_transaction_state;

typedef enum nh_auth_transaction_rc {
  NH_AUTH_TX_OK = 0,
  NH_AUTH_TX_INVALID,
  NH_AUTH_TX_UNAUTHORIZED,
  NH_AUTH_TX_UNKNOWN_ACCOUNT,
  NH_AUTH_TX_NOT_ACTIVE,
  NH_AUTH_TX_STALE,
  NH_AUTH_TX_BAD_STATE,
  NH_AUTH_TX_DEADLINE,
  NH_AUTH_TX_REPLAY,
  NH_AUTH_TX_CANCELLED_RESULT,
  NH_AUTH_TX_INTERNAL
} nh_auth_transaction_rc;

typedef struct nh_auth_peer_snapshot {
  nh_auth_endpoint endpoint;
  uid_t uid;
  gid_t gid;
  pid_t pid;
  uint64_t process_start_id;
  uint8_t connection_id[NH_AUTH_CONNECTION_ID_LEN];
} nh_auth_peer_snapshot;

typedef struct nh_auth_authority_ops {
  nh_identity_rc (*lookup_by_name)(void *, const char *, nh_identity_account *);
  nh_identity_rc (*lookup_by_uid)(void *, uint32_t, nh_identity_account *);
  nh_identity_rc (*recheck)(void *, const char *, uint64_t, uint64_t,
                            nh_identity_status *, nh_identity_account *);
} nh_auth_authority_ops;

typedef struct nh_auth_authority {
  const nh_auth_authority_ops *ops;
  void *context;
} nh_auth_authority;

typedef struct nh_auth_begin_request {
  nh_auth_purpose purpose;
  const char *username; /* required for linux-login; ignored for own-UID SMB */
  const char *service;
  uint64_t now_monotonic_ms;
} nh_auth_begin_request;

typedef struct nh_auth_receipt {
  uint8_t token[NH_AUTH_RECEIPT_LEN];
  uint8_t connection_id[NH_AUTH_CONNECTION_ID_LEN];
  char account_id[NH_IDENTITY_UUID_CAP];
  char transaction_id[NH_AUTH_TRANSACTION_ID_HEX_LEN + 1];
  uint64_t key_generation;
  uint64_t authority_generation;
  uint64_t expires_monotonic_ms;
} nh_auth_receipt;

/* Single-owner broker event-loop object, not thread safe. All state
 * transitions, including provider completion, cancellation and receipt
 * consumption, must be serialized by that owner. Peers and timestamps are
 * broker/kernel-derived; never populate them from client JSON. Store must
 * outlive the transaction. finish_verification accepts only the broker strict
 * verifier's result, never a provider/client success claim. */
typedef struct nh_auth_transaction nh_auth_transaction;

nh_auth_authority nh_auth_authority_from_store(nh_identity_store *store);
nh_auth_transaction_rc nh_auth_transaction_begin(
    const nh_auth_authority *authority, const nh_auth_peer_snapshot *peer,
    const nh_auth_begin_request *request, nh_auth_transaction **out);
void nh_auth_transaction_free(nh_auth_transaction *transaction);
nh_auth_transaction_state
nh_auth_transaction_get_state(const nh_auth_transaction *transaction);
const nh_identity_account *
nh_auth_transaction_get_account(const nh_auth_transaction *transaction);
const char *nh_auth_transaction_get_id(const nh_auth_transaction *transaction);
const char *
nh_auth_transaction_get_service(const nh_auth_transaction *transaction);
nh_auth_purpose
nh_auth_transaction_get_purpose(const nh_auth_transaction *transaction);

nh_auth_transaction_rc nh_auth_transaction_select_provider(
    nh_auth_transaction *transaction, const nh_auth_peer_snapshot *peer,
    nh_identity_provider_type provider, uint64_t now_monotonic_ms);
nh_auth_transaction_rc nh_auth_transaction_begin_proof(
    nh_auth_transaction *transaction, const nh_auth_peer_snapshot *peer,
    uint64_t now_monotonic_ms, uint64_t *challenge_deadline_out);
nh_auth_transaction_rc
nh_auth_transaction_start_verification(nh_auth_transaction *transaction,
                                       const nh_auth_peer_snapshot *peer,
                                       uint64_t now_monotonic_ms);
nh_auth_transaction_rc nh_auth_transaction_finish_verification(
    nh_auth_transaction *transaction, const nh_auth_peer_snapshot *peer,
    uint64_t now_monotonic_ms, nh_auth_proof_rc proof_result,
    nh_auth_receipt *receipt_out);
nh_auth_transaction_rc
nh_auth_transaction_cancel(nh_auth_transaction *transaction,
                           const nh_auth_peer_snapshot *peer);
nh_auth_transaction_rc nh_auth_transaction_open_receipt(
    nh_auth_transaction *transaction, const nh_auth_peer_snapshot *peer,
    uint64_t now_monotonic_ms, const nh_auth_receipt *receipt);
nh_auth_transaction_rc
nh_auth_transaction_close_session(nh_auth_transaction *transaction,
                                  const nh_auth_peer_snapshot *peer);

#endif
