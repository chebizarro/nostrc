#include "auth_transaction.h"
#include <assert.h>
#include "../nh_test.h"
#include <string.h>

typedef struct mock_authority {
  nh_identity_account account;
  nh_identity_rc lookup_result;
  unsigned rechecks;
} mock_authority;

static nh_identity_rc lookup_name(void *context, const char *name,
                                  nh_identity_account *out) {
  mock_authority *mock = context;
  if (mock->lookup_result != NH_IDENTITY_OK)
    return mock->lookup_result;
  if (strcmp(name, mock->account.username))
    return NH_IDENTITY_NOT_FOUND;
  *out = mock->account;
  return NH_IDENTITY_OK;
}
static nh_identity_rc lookup_uid(void *context, uint32_t uid,
                                 nh_identity_account *out) {
  mock_authority *mock = context;
  if (mock->lookup_result != NH_IDENTITY_OK)
    return mock->lookup_result;
  *out = mock->account;
  (void)uid; /* Core independently rejects a mismatched returned UID. */
  return NH_IDENTITY_OK;
}
static nh_identity_rc recheck(void *context, const char *account_id,
                              uint64_t key_generation,
                              uint64_t authority_generation,
                              nh_identity_status *status,
                              nh_identity_account *out) {
  mock_authority *mock = context;
  mock->rechecks++;
  if (status)
    *status = mock->account.status;
  if (out)
    *out = mock->account;
  if (strcmp(account_id, mock->account.account_id))
    return NH_IDENTITY_NOT_FOUND;
  if (mock->account.status != NH_IDENTITY_STATUS_ACTIVE)
    return NH_IDENTITY_NOT_ACTIVE;
  if (key_generation != mock->account.key_generation ||
      authority_generation != mock->account.authority_generation)
    return NH_IDENTITY_STALE_GENERATION;
  return NH_IDENTITY_OK;
}
static const nh_auth_authority_ops authority_ops = {lookup_name, lookup_uid,
                                                    recheck};

static mock_authority make_authority(void) {
  mock_authority mock = {0};
  strcpy(mock.account.account_id, "11111111-1111-1111-1111-111111111111");
  strcpy(mock.account.username, "n_test");
  strcpy(mock.account.pubkey_hex,
         "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
  mock.account.uid = 200001;
  mock.account.gid = 200001;
  mock.account.status = NH_IDENTITY_STATUS_ACTIVE;
  mock.account.key_generation = 3;
  mock.account.authority_generation = 7;
  mock.account.enabled_providers =
      NH_IDENTITY_PROVIDER_BIT(NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY) |
      NH_IDENTITY_PROVIDER_BIT(NH_IDENTITY_PROVIDER_NIP46_BUNKER);
  mock.lookup_result = NH_IDENTITY_OK;
  return mock;
}
static nh_auth_peer_snapshot peer(uid_t uid, nh_auth_endpoint endpoint,
                                  unsigned connection_byte) {
  nh_auth_peer_snapshot p = {0};
  p.endpoint = endpoint;
  p.uid = uid;
  p.gid = uid;
  p.pid = 4242;
  p.process_start_id = 998877;
  memset(p.connection_id, (int)connection_byte, sizeof p.connection_id);
  return p;
}
static nh_auth_transaction *begin_login(mock_authority *mock,
                                        const nh_auth_peer_snapshot *p) {
  nh_auth_authority authority = {&authority_ops, mock};
  nh_auth_begin_request request = {NH_AUTH_PURPOSE_LINUX_LOGIN, "n_test",
                                   "gdm-password", 1000};
  nh_auth_transaction *tx = NULL;
  NH_CHECK(nh_auth_transaction_begin(&authority, p, &request, &tx) ==
         NH_AUTH_TX_OK);
  return tx;
}
static nh_auth_receipt verify_login(nh_auth_transaction *tx,
                                    const nh_auth_peer_snapshot *p) {
  uint64_t challenge_deadline = 0;
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, p, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_OK);
  NH_CHECK(nh_auth_transaction_begin_proof(tx, p, 1002, &challenge_deadline) ==
         NH_AUTH_TX_OK);
  NH_CHECK(challenge_deadline == 121002);
  NH_CHECK(nh_auth_transaction_start_verification(tx, p, 1003) == NH_AUTH_TX_OK);
  nh_auth_receipt receipt;
  NH_CHECK(nh_auth_transaction_finish_verification(tx, p, 1004, NH_AUTH_PROOF_OK,
                                                 &receipt) == NH_AUTH_TX_OK);
  return receipt;
}

static void test_admission(void) {
  mock_authority mock = make_authority();
  nh_auth_authority authority = {&authority_ops, &mock};
  nh_auth_peer_snapshot root = peer(0, NH_AUTH_ENDPOINT_AUTH, 1);
  nh_auth_begin_request request = {NH_AUTH_PURPOSE_LINUX_LOGIN, "n_test",
                                   "gdm-password", 1000};
  nh_auth_transaction *tx = NULL;
  NH_CHECK(nh_auth_transaction_begin(&authority, &root, &request, &tx) ==
         NH_AUTH_TX_OK);
  NH_CHECK(strlen(nh_auth_transaction_get_id(tx)) == 64);
  NH_CHECK(!strcmp(nh_auth_transaction_get_service(tx), "gdm-password"));
  NH_CHECK(nh_auth_transaction_get_purpose(tx) == NH_AUTH_PURPOSE_LINUX_LOGIN);
  NH_CHECK(nh_auth_transaction_get_account(tx)->uid == 200001);
  nh_auth_transaction_free(tx);

  nh_auth_peer_snapshot unprivileged = peer(1000, NH_AUTH_ENDPOINT_AUTH, 1);
  NH_CHECK(nh_auth_transaction_begin(&authority, &unprivileged, &request, &tx) ==
         NH_AUTH_TX_UNAUTHORIZED);
  nh_auth_peer_snapshot wrong_endpoint = peer(0, NH_AUTH_ENDPOINT_USER, 1);
  NH_CHECK(nh_auth_transaction_begin(&authority, &wrong_endpoint, &request,
                                   &tx) == NH_AUTH_TX_UNAUTHORIZED);
  request.purpose = NH_AUTH_PURPOSE_ENROLLMENT;
  NH_CHECK(nh_auth_transaction_begin(&authority, &root, &request, &tx) ==
         NH_AUTH_TX_UNAUTHORIZED);
  request.purpose = NH_AUTH_PURPOSE_LINUX_LOGIN;
  request.service = "";
  NH_CHECK(nh_auth_transaction_begin(&authority, &root, &request, &tx) ==
         NH_AUTH_TX_INVALID);
  request.service = "gdm-password";
  mock.lookup_result = NH_IDENTITY_NOT_FOUND;
  NH_CHECK(nh_auth_transaction_begin(&authority, &root, &request, &tx) ==
         NH_AUTH_TX_UNKNOWN_ACCOUNT);
  mock = make_authority();
  mock.account.status = NH_IDENTITY_STATUS_DISABLED;
  NH_CHECK(nh_auth_transaction_begin(&authority, &root, &request, &tx) ==
         NH_AUTH_TX_NOT_ACTIVE);
  mock = make_authority();
  mock.account.enabled_providers = 0;
  NH_CHECK(nh_auth_transaction_begin(&authority, &root, &request, &tx) ==
         NH_AUTH_TX_NOT_ACTIVE);

  mock = make_authority();
  nh_auth_peer_snapshot own = peer(200001, NH_AUTH_ENDPOINT_USER, 2);
  request = (nh_auth_begin_request){NH_AUTH_PURPOSE_SMB_CREDENTIAL, "ignored",
                                    "smb-credential", 1000};
  NH_CHECK(nh_auth_transaction_begin(&authority, &own, &request, &tx) ==
         NH_AUTH_TX_OK);
  nh_auth_transaction_free(tx);
  own.uid = 200002;
  NH_CHECK(nh_auth_transaction_begin(&authority, &own, &request, &tx) ==
         NH_AUTH_TX_UNAUTHORIZED);
}

static void test_binding_and_deadlines(void) {
  mock_authority mock = make_authority();
  nh_auth_peer_snapshot owner = peer(0, NH_AUTH_ENDPOINT_AUTH, 3);
  nh_auth_transaction *tx = begin_login(&mock, &owner);
  nh_auth_peer_snapshot changed = owner;
  changed.uid = 1;
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &changed, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_UNAUTHORIZED);
  changed = owner;
  changed.gid = 1;
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &changed, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_UNAUTHORIZED);
  changed = owner;
  changed.pid++;
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &changed, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_UNAUTHORIZED);
  changed = owner;
  changed.process_start_id++;
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &changed, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_UNAUTHORIZED);
  changed = owner;
  changed.connection_id[0] ^= 1;
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &changed, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_UNAUTHORIZED);
  changed = owner;
  changed.endpoint = NH_AUTH_ENDPOINT_USER;
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &changed, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_UNAUTHORIZED);
  NH_CHECK(nh_auth_transaction_select_provider(tx, &owner,
                                             (nh_identity_provider_type)99,
                                             1001) == NH_AUTH_TX_INVALID);
  nh_auth_transaction_free(tx);

  tx = begin_login(&mock, &owner);
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &owner, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 181001) ==
         NH_AUTH_TX_DEADLINE);
  NH_CHECK(nh_auth_transaction_get_state(tx) == NH_AUTH_TX_EXPIRED);
  nh_auth_transaction_free(tx);

  tx = begin_login(&mock, &owner);
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &owner, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_OK);
  NH_CHECK(nh_auth_transaction_begin_proof(tx, &owner, 1002, NULL) ==
         NH_AUTH_TX_OK);
  NH_CHECK(nh_auth_transaction_start_verification(tx, &owner, 121003) ==
         NH_AUTH_TX_DEADLINE);
  nh_auth_transaction_free(tx);
}

static void test_one_winner_and_receipts(void) {
  mock_authority mock = make_authority();
  nh_auth_peer_snapshot owner = peer(0, NH_AUTH_ENDPOINT_AUTH, 4);
  nh_auth_transaction *tx = begin_login(&mock, &owner);
  nh_auth_receipt receipt = verify_login(tx, &owner);
  NH_CHECK(mock.rechecks == 2);
  NH_CHECK(nh_auth_transaction_finish_verification(
             tx, &owner, 1005, NH_AUTH_PROOF_OK, NULL) == NH_AUTH_TX_REPLAY);
  nh_auth_receipt bad = receipt;
  bad.token[0] ^= 1;
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &owner, 1006, &bad) ==
         NH_AUTH_TX_UNAUTHORIZED);
  bad = receipt;
  bad.transaction_id[0] = bad.transaction_id[0] == 'a' ? 'b' : 'a';
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &owner, 1006, &bad) ==
         NH_AUTH_TX_UNAUTHORIZED);
  nh_auth_peer_snapshot other_connection = owner;
  other_connection.connection_id[0] ^= 1;
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &other_connection, 1006,
                                          &receipt) == NH_AUTH_TX_UNAUTHORIZED);
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &owner, 1006, &receipt) ==
         NH_AUTH_TX_OK);
  NH_CHECK(mock.rechecks == 3);
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &owner, 1007, &receipt) ==
         NH_AUTH_TX_REPLAY);
  NH_CHECK(nh_auth_transaction_close_session(tx, &owner) == NH_AUTH_TX_OK);
  nh_auth_transaction_free(tx);

  tx = begin_login(&mock, &owner);
  receipt = verify_login(tx, &owner);
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &owner,
                                          receipt.expires_monotonic_ms + 1,
                                          &receipt) == NH_AUTH_TX_DEADLINE);
  nh_auth_transaction_free(tx);

  tx = begin_login(&mock, &owner);
  receipt = verify_login(tx, &owner);
  mock.account.authority_generation++;
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &owner, 1006, &receipt) ==
         NH_AUTH_TX_STALE);
  NH_CHECK(nh_auth_transaction_get_state(tx) == NH_AUTH_TX_DENIED);
  nh_auth_transaction_free(tx);
}

static void test_failure_and_cancel(void) {
  mock_authority mock = make_authority();
  nh_auth_peer_snapshot owner = peer(0, NH_AUTH_ENDPOINT_AUTH, 5);
  nh_auth_transaction *tx = begin_login(&mock, &owner);
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &owner, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_OK);
  NH_CHECK(nh_auth_transaction_begin_proof(tx, &owner, 1002, NULL) ==
         NH_AUTH_TX_OK);
  mock.account.key_generation++;
  NH_CHECK(nh_auth_transaction_start_verification(tx, &owner, 1003) ==
         NH_AUTH_TX_STALE);
  NH_CHECK(nh_auth_transaction_get_state(tx) == NH_AUTH_TX_DENIED);
  nh_auth_transaction_free(tx);

  mock = make_authority();
  tx = begin_login(&mock, &owner);
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &owner, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_OK);
  NH_CHECK(nh_auth_transaction_begin_proof(tx, &owner, 1002, NULL) ==
         NH_AUTH_TX_OK);
  NH_CHECK(nh_auth_transaction_start_verification(tx, &owner, 1003) ==
         NH_AUTH_TX_OK);
  NH_CHECK(nh_auth_transaction_finish_verification(tx, &owner, 1004,
                                                 NH_AUTH_PROOF_INVALID,
                                                 NULL) == NH_AUTH_TX_INVALID);
  NH_CHECK(nh_auth_transaction_finish_verification(
             tx, &owner, 1005, NH_AUTH_PROOF_OK, NULL) == NH_AUTH_TX_BAD_STATE);
  nh_auth_transaction_free(tx);

  tx = begin_login(&mock, &owner);
  NH_CHECK(nh_auth_transaction_cancel(tx, &owner) == NH_AUTH_TX_OK);
  NH_CHECK(nh_auth_transaction_cancel(tx, &owner) == NH_AUTH_TX_OK);
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &owner, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 1001) ==
         NH_AUTH_TX_CANCELLED_RESULT);
  nh_auth_transaction_free(tx);

  tx = begin_login(&mock, &owner);
  nh_auth_receipt receipt = verify_login(tx, &owner);
  NH_CHECK(nh_auth_transaction_cancel(tx, &owner) == NH_AUTH_TX_OK);
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &owner, 1006, &receipt) ==
         NH_AUTH_TX_CANCELLED_RESULT);
  nh_auth_transaction_free(tx);
}

static void test_boundary_and_malformed_receipts(void) {
  mock_authority mock = make_authority();
  nh_auth_peer_snapshot owner = peer(0, NH_AUTH_ENDPOINT_AUTH, 9);
  nh_auth_transaction *tx = begin_login(&mock, &owner);
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &owner, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 999) ==
         NH_AUTH_TX_INVALID);
  nh_auth_transaction_free(tx);
  tx = begin_login(&mock, &owner);
  NH_CHECK(nh_auth_transaction_select_provider(
             tx, &owner, NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY, 181000) ==
         NH_AUTH_TX_DEADLINE);
  nh_auth_transaction_free(tx);
  tx = begin_login(&mock, &owner);
  nh_auth_receipt receipt = verify_login(tx, &owner), bad = receipt;
  memset(bad.account_id, 'x', sizeof bad.account_id);
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &owner, 1005, &bad) ==
         NH_AUTH_TX_UNAUTHORIZED);
  bad = receipt;
  memset(bad.transaction_id, 'x', sizeof bad.transaction_id);
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &owner, 1005, &bad) ==
         NH_AUTH_TX_UNAUTHORIZED);
  NH_CHECK(nh_auth_transaction_open_receipt(tx, &owner,
                                          receipt.expires_monotonic_ms,
                                          &receipt) == NH_AUTH_TX_DEADLINE);
  nh_auth_transaction_free(tx);
}

int main(void) {
  test_boundary_and_malformed_receipts();
  test_admission();
  test_binding_and_deadlines();
  test_one_winner_and_receipts();
  test_failure_and_cancel();
  return 0;
}
