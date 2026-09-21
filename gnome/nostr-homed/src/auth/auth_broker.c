#define _GNU_SOURCE
#include "auth_broker.h"

#include "auth_challenge.h"
#include "auth_peer.h"
#include "auth_provider.h"
#include "auth_transaction.h"
#include "nostr-event.h"
#include "nostr_auth_protocol.h"

#include <fcntl.h>
#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct nh_auth_broker {
  nh_identity_store *store;
  nh_auth_authority authority;
};

/* Per-connection login state (single-owner, one transaction per connection). */
typedef struct conn_state {
  nh_auth_broker *broker;
  nh_auth_peer_snapshot peer;
  nh_auth_transaction *tx;
  nh_auth_provider *provider;
  nh_auth_challenge challenge;
  int challenge_built;
  char *signed_json;      /* captured provider SIGNED_EVENT */
  nh_auth_result provider_result;
  nh_auth_provider_event_type provider_event;
} conn_state;

nh_auth_broker *nh_auth_broker_new(nh_identity_store *store) {
  if (!store) return NULL;
  nh_auth_broker *broker = calloc(1, sizeof *broker);
  if (!broker) return NULL;
  broker->store = store;
  broker->authority = nh_auth_authority_from_store(store);
  return broker;
}

void nh_auth_broker_free(nh_auth_broker *broker) { free(broker); }

static uint64_t now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void read_boot_id(char out[37]) {
  out[0] = '\0';
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  char buf[64];
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return;
  buf[n] = '\0';
  size_t len = strspn(buf, "0123456789abcdef-");
  if (len == 36) { memcpy(out, buf, 36); out[36] = '\0'; }
}

/* ---- result mapping ---------------------------------------------------- */

static nh_auth_result tx_rc_result(nh_auth_transaction_rc rc) {
  switch (rc) {
    case NH_AUTH_TX_OK: return NH_AUTH_RESULT_OK;
    case NH_AUTH_TX_UNKNOWN_ACCOUNT: return NH_AUTH_RESULT_UNKNOWN_ACCOUNT;
    case NH_AUTH_TX_NOT_ACTIVE: return NH_AUTH_RESULT_NOT_READY;
    case NH_AUTH_TX_STALE: return NH_AUTH_RESULT_NOT_READY;
    case NH_AUTH_TX_UNAUTHORIZED: return NH_AUTH_RESULT_DENIED;
    case NH_AUTH_TX_REPLAY: return NH_AUTH_RESULT_DENIED;
    case NH_AUTH_TX_DEADLINE: return NH_AUTH_RESULT_EXPIRED;
    case NH_AUTH_TX_CANCELLED_RESULT: return NH_AUTH_RESULT_CANCELLED;
    case NH_AUTH_TX_INTERNAL: return NH_AUTH_RESULT_INTERNAL_ERROR;
    default: return NH_AUTH_RESULT_PROTOCOL_ERROR;
  }
}

static nh_auth_result proof_rc_result(nh_auth_proof_rc rc) {
  switch (rc) {
    case NH_AUTH_PROOF_OK: return NH_AUTH_RESULT_OK;
    case NH_AUTH_PROOF_EXPIRED: return NH_AUTH_RESULT_EXPIRED;
    case NH_AUTH_PROOF_STALE_ACCOUNT: return NH_AUTH_RESULT_NOT_READY;
    default: return NH_AUTH_RESULT_INVALID_PROOF;
  }
}

/* ---- responses --------------------------------------------------------- */

static int send_payload(int fd, const nh_auth_message *request,
                        nh_auth_operation op, json_t *payload /*stolen*/) {
  char *payload_json = payload ? json_dumps(payload, JSON_COMPACT) : NULL;
  if (payload) json_decref(payload);
  if (!payload_json) return -1;
  nh_auth_message response;
  memset(&response, 0, sizeof response);
  response.operation = op;
  if (request)
    memcpy(response.request_id, request->request_id, sizeof response.request_id);
  else {
    memset(response.request_id, '0', NH_AUTH_REQUEST_ID_HEX_LEN);
    response.request_id[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
  }
  response.transaction_id[0] = '\0';
  response.payload_json = payload_json;
  int rc = nh_auth_send_message(fd, &response);
  free(payload_json);
  return rc;
}

static int respond(int fd, const nh_auth_message *request,
                   nh_auth_operation op, nh_auth_result result) {
  json_t *payload = json_object();
  if (!payload) return -1;
  if (json_object_set_new(payload, "result",
                          json_string(nh_auth_result_name(result))) != 0) {
    json_decref(payload);
    return -1;
  }
  return send_payload(fd, request, op, payload);
}

/* ---- account status (CHECK_ACCOUNT) ------------------------------------ */

static nh_auth_result account_result(nh_identity_store *store,
                                     const char *username) {
  nh_identity_account account;
  nh_identity_rc rc = nh_identity_store_lookup_by_name(store, username, &account);
  if (rc == NH_IDENTITY_NOT_FOUND) return NH_AUTH_RESULT_UNKNOWN_ACCOUNT;
  if (rc != NH_IDENTITY_OK) return NH_AUTH_RESULT_STORAGE_ERROR;
  switch (account.status) {
    case NH_IDENTITY_STATUS_ACTIVE: return NH_AUTH_RESULT_OK;
    case NH_IDENTITY_STATUS_DISABLED: return NH_AUTH_RESULT_DISABLED;
    case NH_IDENTITY_STATUS_ENROLLING:
    case NH_IDENTITY_STATUS_REPAIR_REQUIRED: return NH_AUTH_RESULT_NOT_READY;
    default: return NH_AUTH_RESULT_UNKNOWN_ACCOUNT;
  }
}

static const char *json_str(const char *payload_json, const char *key,
                            json_t **root_out) {
  if (!payload_json) return NULL;
  json_error_t e;
  json_t *root = json_loads(payload_json, JSON_REJECT_DUPLICATES, &e);
  if (!root || !json_is_object(root)) { if (root) json_decref(root); return NULL; }
  json_t *v = json_object_get(root, key);
  if (!v || !json_is_string(v)) { json_decref(root); return NULL; }
  *root_out = root;
  return json_string_value(v);
}

/* ---- provider event capture ------------------------------------------- */

static void provider_emit(void *context, const nh_auth_provider_event *event) {
  conn_state *cs = context;
  cs->provider_event = event->type;
  cs->provider_result = event->result;
  if (event->type == NH_AUTH_PROVIDER_SIGNED_EVENT && event->data &&
      event->data_len) {
    free(cs->signed_json);
    cs->signed_json = malloc(event->data_len + 1);
    if (cs->signed_json) {
      memcpy(cs->signed_json, event->data, event->data_len);
      cs->signed_json[event->data_len] = '\0';
    }
  }
}

static void conn_reset_proof(conn_state *cs) {
  if (cs->provider) { cs->provider->ops->destroy(cs->provider); cs->provider = NULL; }
  if (cs->challenge_built) { nh_auth_challenge_clear(&cs->challenge); cs->challenge_built = 0; }
  if (cs->signed_json) { free(cs->signed_json); cs->signed_json = NULL; }
  cs->provider_event = 0;
}

/* ---- login operations -------------------------------------------------- */

static nh_auth_result do_begin_login(conn_state *cs, const char *payload_json) {
  if (cs->tx) return NH_AUTH_RESULT_PROTOCOL_ERROR; /* one tx per connection */
  json_t *root = NULL;
  const char *username = json_str(payload_json, "username", &root);
  json_t *sroot = NULL;
  const char *service = json_str(payload_json, "service", &sroot);
  nh_auth_result result = NH_AUTH_RESULT_PROTOCOL_ERROR;
  if (username && username[0] && strlen(username) <= NH_IDENTITY_USERNAME_MAX &&
      service && service[0]) {
    nh_auth_begin_request request = {NH_AUTH_PURPOSE_LINUX_LOGIN, username,
                                     service, now_ms()};
    nh_auth_transaction_rc rc =
        nh_auth_transaction_begin(&cs->broker->authority, &cs->peer, &request,
                                  &cs->tx);
    result = tx_rc_result(rc);
  }
  if (root) json_decref(root);
  if (sroot) json_decref(sroot);
  return result;
}

/* Map the optional "provider" payload field to a provider type. When the
 * client does not name a provider, prefer NIP-46 if the account has it
 * enabled (external signer beats a local passphrase prompt), otherwise fall
 * back to the local encrypted vault. Unknown names are a protocol error. */
static int pick_provider_type(const char *payload_json,
                              const nh_identity_account *account,
                              nh_identity_provider_type *out) {
  json_t *root = NULL;
  const char *name = payload_json ? json_str(payload_json, "provider", &root)
                                  : NULL;
  int rc = 0;
  if (name && *name) {
    if (!strcmp(name, "local") || !strcmp(name, "local_encrypted_key"))
      *out = NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY;
    else if (!strcmp(name, "nip46") || !strcmp(name, "nip46_bunker") ||
             !strcmp(name, "bunker"))
      *out = NH_IDENTITY_PROVIDER_NIP46_BUNKER;
    else
      rc = -1;
  } else if (account->enabled_providers &
             NH_IDENTITY_PROVIDER_BIT(NH_IDENTITY_PROVIDER_NIP46_BUNKER)) {
    *out = NH_IDENTITY_PROVIDER_NIP46_BUNKER;
  } else {
    *out = NH_IDENTITY_PROVIDER_LOCAL_ENCRYPTED_KEY;
  }
  if (root) json_decref(root);
  return rc;
}

static nh_auth_result do_select_provider(conn_state *cs,
                                         const char *payload_json) {
  if (!cs->tx) return NH_AUTH_RESULT_PROTOCOL_ERROR;
  const nh_identity_account *account = nh_auth_transaction_get_account(cs->tx);
  if (!account) return NH_AUTH_RESULT_INTERNAL_ERROR;

  nh_identity_provider_type provider_type;
  if (pick_provider_type(payload_json, account, &provider_type) != 0)
    return NH_AUTH_RESULT_PROTOCOL_ERROR;

  uint64_t now = now_ms();
  nh_auth_transaction_rc rc = nh_auth_transaction_select_provider(
      cs->tx, &cs->peer, provider_type, now);
  if (rc != NH_AUTH_TX_OK) return tx_rc_result(rc);
  uint64_t challenge_deadline = 0;
  rc = nh_auth_transaction_begin_proof(cs->tx, &cs->peer, now,
                                       &challenge_deadline);
  if (rc != NH_AUTH_TX_OK) return tx_rc_result(rc);

  nh_identity_store_info info;
  if (nh_identity_store_get_info(cs->broker->store, &info) != NH_IDENTITY_OK)
    return NH_AUTH_RESULT_STORAGE_ERROR;
  nh_identity_provider_record rec;
  if (nh_identity_store_provider_get(cs->broker->store, account->account_id,
                                     provider_type, true, &rec) !=
      NH_IDENTITY_OK)
    return NH_AUTH_RESULT_PROVIDER_UNAVAILABLE;

  char boot_id[37];
  read_boot_id(boot_id);
  int64_t issued = (int64_t)time(NULL);
  nh_auth_challenge_input in = {0};
  in.purpose = nh_auth_transaction_get_purpose(cs->tx);
  in.transaction_id = nh_auth_transaction_get_id(cs->tx);
  in.authority_id = info.authority_id;
  in.boot_id = boot_id[0] ? boot_id : "00000000-0000-4000-8000-000000000000";
  in.service = nh_auth_transaction_get_service(cs->tx);
  in.context_json = "";
  in.resource_json = "";
  in.account = account;
  in.issued_at = issued;
  in.expires_at = issued + (int64_t)NH_AUTH_CHALLENGE_LIFETIME_SEC;
  in.deadline_monotonic_ms = challenge_deadline;
  in.nonce32 = NULL;
  if (nh_auth_challenge_build(&in, &cs->challenge) != 0)
    return NH_AUTH_RESULT_INTERNAL_ERROR;
  cs->challenge_built = 1;

  char *unsigned_json = nostr_event_serialize_compact(cs->challenge.event);
  if (!unsigned_json) return NH_AUTH_RESULT_INTERNAL_ERROR;
  cs->provider = (provider_type == NH_IDENTITY_PROVIDER_NIP46_BUNKER)
                     ? nh_auth_provider_nip46_new(provider_emit, cs)
                     : nh_auth_provider_local_new(provider_emit, cs);
  nh_auth_result result = NH_AUTH_RESULT_PROVIDER_UNAVAILABLE;
  if (cs->provider) {
    nh_auth_provider_snapshot snap = {0};
    snap.type = provider_type;
    snap.provider_id = rec.provider_id;
    snap.account_id = account->account_id;
    snap.pubkey_hex = account->pubkey_hex;
    snap.key_generation = account->key_generation;
    snap.deadline_monotonic_ms = challenge_deadline;
    snap.public_config_json = rec.public_config_json;
    snap.secret_blob = rec.secret_blob;
    snap.secret_blob_len = rec.secret_blob_len;
    nh_auth_immutable_challenge ic = {0};
    ic.unsigned_event_json = unsigned_json;
    ic.unsigned_event_json_len = strlen(unsigned_json);
    ic.expected_event_id = cs->challenge.expected_id;
    ic.deadline_monotonic_ms = challenge_deadline;
    if (cs->provider->ops->prepare(cs->provider, &snap) == 0 &&
        cs->provider->ops->begin_proof(cs->provider, &ic) == 0)
      result = NH_AUTH_RESULT_INTERACTION_REQUIRED;
  }
  free(unsigned_json);
  return result;
}

static nh_auth_result do_submit_unlock(conn_state *cs, const char *payload_json,
                                       nh_auth_receipt *receipt_out,
                                       int *have_receipt) {
  *have_receipt = 0;
  if (!cs->tx || !cs->provider || !cs->challenge_built)
    return NH_AUTH_RESULT_PROTOCOL_ERROR;
  json_t *root = NULL;
  const char *secret = json_str(payload_json, "secret", &root);
  if (!secret || !secret[0] || strlen(secret) > NH_AUTH_SECRET_MAX) {
    if (root) json_decref(root);
    return NH_AUTH_RESULT_PROTOCOL_ERROR;
  }
  cs->provider_event = 0;
  cs->provider->ops->submit_unlock(cs->provider, (const uint8_t *)secret,
                                   strlen(secret));
  if (root) { /* wipe the transient secret copy in the parsed JSON */
    volatile char *w = (volatile char *)secret;
    for (size_t i = 0; secret[i]; i++) w[i] = 0;
    json_decref(root);
  }

  nh_auth_proof_rc proof;
  if (cs->provider_event == NH_AUTH_PROVIDER_SIGNED_EVENT && cs->signed_json) {
    const nh_identity_account *account = nh_auth_transaction_get_account(cs->tx);
    proof = nh_auth_challenge_verify(&cs->challenge, cs->signed_json, now_ms(),
                                     account);
  } else if (cs->provider_event == NH_AUTH_PROVIDER_DENIED) {
    proof = NH_AUTH_PROOF_INVALID; /* wrong passphrase */
  } else {
    proof = NH_AUTH_PROOF_CRYPTO_ERROR;
  }

  uint64_t now = now_ms();
  nh_auth_transaction_rc rc =
      nh_auth_transaction_start_verification(cs->tx, &cs->peer, now);
  if (rc != NH_AUTH_TX_OK) return tx_rc_result(rc);
  nh_auth_receipt receipt;
  rc = nh_auth_transaction_finish_verification(cs->tx, &cs->peer, now, proof,
                                               &receipt);
  if (proof != NH_AUTH_PROOF_OK) return proof_rc_result(proof);
  if (rc != NH_AUTH_TX_OK) return tx_rc_result(rc);
  *receipt_out = receipt;
  *have_receipt = 1;
  return NH_AUTH_RESULT_OK;
}

/* ---- dispatch ---------------------------------------------------------- */

static int handle_request(conn_state *cs, int fd, const nh_auth_message *req) {
  nh_auth_operation op = req->operation;
  if (!nh_auth_operation_allowed(NH_AUTH_ENDPOINT_AUTH, op, cs->peer.uid,
                                 cs->tx != NULL))
    return respond(fd, req, op, NH_AUTH_RESULT_DENIED);

  switch (op) {
    case NH_AUTH_OP_CHECK_ACCOUNT: {
      json_t *root = NULL;
      const char *username = json_str(req->payload_json, "username", &root);
      nh_auth_result r = (username && username[0] &&
                          strlen(username) <= NH_IDENTITY_USERNAME_MAX)
                             ? account_result(cs->broker->store, username)
                             : NH_AUTH_RESULT_PROTOCOL_ERROR;
      if (root) json_decref(root);
      return respond(fd, req, op, r);
    }
    case NH_AUTH_OP_BEGIN_LOGIN:
      return respond(fd, req, op, do_begin_login(cs, req->payload_json));
    case NH_AUTH_OP_SELECT_PROVIDER:
      return respond(fd, req, op, do_select_provider(cs, req->payload_json));
    case NH_AUTH_OP_SUBMIT_UNLOCK: {
      nh_auth_receipt receipt;
      int have = 0;
      nh_auth_result r = do_submit_unlock(cs, req->payload_json, &receipt, &have);
      if (r == NH_AUTH_RESULT_OK && have) {
        json_t *payload = json_object();
        char hex[NH_AUTH_RECEIPT_LEN * 2 + 1];
        static const char d[] = "0123456789abcdef";
        for (size_t i = 0; i < NH_AUTH_RECEIPT_LEN; i++) {
          hex[i * 2] = d[receipt.token[i] >> 4];
          hex[i * 2 + 1] = d[receipt.token[i] & 0xf];
        }
        hex[NH_AUTH_RECEIPT_LEN * 2] = '\0';
        if (!payload ||
            json_object_set_new(payload, "result",
                                json_string(nh_auth_result_name(r))) != 0 ||
            json_object_set_new(payload, "receipt", json_string(hex)) != 0) {
          if (payload) json_decref(payload);
          return respond(fd, req, op, NH_AUTH_RESULT_INTERNAL_ERROR);
        }
        return send_payload(fd, req, op, payload);
      }
      return respond(fd, req, op, r);
    }
    case NH_AUTH_OP_WAIT_RESULT: {
      nh_auth_transaction_state st =
          cs->tx ? nh_auth_transaction_get_state(cs->tx) : NH_AUTH_TX_NEW;
      nh_auth_result r = (st == NH_AUTH_TX_VERIFIED ||
                          st == NH_AUTH_TX_SESSION_OPENED)
                             ? NH_AUTH_RESULT_OK
                             : (st == NH_AUTH_TX_DENIED
                                    ? NH_AUTH_RESULT_DENIED
                                    : NH_AUTH_RESULT_INTERACTION_REQUIRED);
      return respond(fd, req, op, r);
    }
    case NH_AUTH_OP_CANCEL: {
      if (cs->tx) nh_auth_transaction_cancel(cs->tx, &cs->peer);
      return respond(fd, req, op, NH_AUTH_RESULT_CANCELLED);
    }
    default:
      return respond(fd, req, op, NH_AUTH_RESULT_PROTOCOL_ERROR);
  }
}

int nh_auth_broker_handle_connection(nh_auth_broker *broker, int fd) {
  if (!broker) return -1;
  conn_state cs;
  memset(&cs, 0, sizeof cs);
  cs.broker = broker;
  if (nh_auth_peer_from_fd(fd, NH_AUTH_ENDPOINT_AUTH, &cs.peer) != 0) return -1;

  int rc = 0;
  for (;;) {
    unsigned char *packet = NULL;
    size_t packet_len = 0;
    if (nh_auth_recv_packet(fd, &packet, &packet_len) != 0) break; /* closed */
    nh_auth_message request;
    int parsed = nh_auth_message_parse(packet, packet_len, &request);
    if (packet) { memset(packet, 0, packet_len); free(packet); }
    if (parsed != 0) {
      rc = respond(fd, NULL, NH_AUTH_OP_CHECK_ACCOUNT,
                   NH_AUTH_RESULT_PROTOCOL_ERROR);
    } else {
      rc = handle_request(&cs, fd, &request);
      nh_auth_message_clear(&request);
    }
    if (rc != 0) break;
  }

  conn_reset_proof(&cs);
  if (cs.tx) nh_auth_transaction_free(cs.tx);
  return rc;
}
