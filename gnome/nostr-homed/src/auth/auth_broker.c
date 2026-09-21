#define _GNU_SOURCE
#include "auth_broker.h"

#include "auth_peer.h"
#include "nostr_auth_protocol.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

struct nh_auth_broker {
  nh_identity_store *store; /* borrowed authority */
};

nh_auth_broker *nh_auth_broker_new(nh_identity_store *store) {
  if (!store) return NULL;
  nh_auth_broker *broker = calloc(1, sizeof *broker);
  if (!broker) return NULL;
  broker->store = store;
  return broker;
}

void nh_auth_broker_free(nh_auth_broker *broker) { free(broker); }

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
    default: return NH_AUTH_RESULT_UNKNOWN_ACCOUNT; /* retired/unknown */
  }
}

static nh_auth_result dispatch_check_account(nh_auth_broker *broker,
                                             const char *payload_json) {
  if (!payload_json) return NH_AUTH_RESULT_PROTOCOL_ERROR;
  json_error_t e;
  json_t *root = json_loads(payload_json, JSON_REJECT_DUPLICATES, &e);
  nh_auth_result result = NH_AUTH_RESULT_PROTOCOL_ERROR;
  if (root && json_is_object(root)) {
    json_t *name = json_object_get(root, "username");
    if (name && json_is_string(name)) {
      const char *username = json_string_value(name);
      if (username[0] && strlen(username) <= NH_IDENTITY_USERNAME_MAX)
        result = account_result(broker->store, username);
    }
  }
  if (root) json_decref(root);
  return result;
}

static int send_result(int fd, const nh_auth_message *request,
                       nh_auth_result result) {
  json_t *payload = json_object();
  if (!payload) return -1;
  if (json_object_set_new(payload, "result",
                          json_string(nh_auth_result_name(result))) != 0) {
    json_decref(payload);
    return -1;
  }
  char *payload_json = json_dumps(payload, JSON_COMPACT);
  json_decref(payload);
  if (!payload_json) return -1;
  nh_auth_message response;
  memset(&response, 0, sizeof response);
  response.operation = request->operation;
  memcpy(response.request_id, request->request_id, sizeof response.request_id);
  response.transaction_id[0] = '\0';
  response.payload_json = payload_json;
  int rc = nh_auth_send_message(fd, &response);
  free(payload_json);
  return rc;
}

int nh_auth_broker_handle_connection(nh_auth_broker *broker, int fd) {
  if (!broker) return -1;
  nh_auth_peer_snapshot peer;
  if (nh_auth_peer_from_fd(fd, NH_AUTH_ENDPOINT_AUTH, &peer) != 0) return -1;

  unsigned char *packet = NULL;
  size_t packet_len = 0;
  if (nh_auth_recv_packet(fd, &packet, &packet_len) != 0) return -1;
  nh_auth_message request;
  int parsed = nh_auth_message_parse(packet, packet_len, &request);
  free(packet);
  if (parsed != 0) {
    nh_auth_message err;
    memset(&err, 0, sizeof err);
    err.operation = NH_AUTH_OP_CHECK_ACCOUNT; /* valid op for a framed reply */
    memset(err.request_id, '0', NH_AUTH_REQUEST_ID_HEX_LEN);
    err.request_id[NH_AUTH_REQUEST_ID_HEX_LEN] = '\0';
    return send_result(fd, &err, NH_AUTH_RESULT_PROTOCOL_ERROR);
  }

  nh_auth_result result;
  if (!nh_auth_operation_allowed(NH_AUTH_ENDPOINT_AUTH, request.operation,
                                 peer.uid, 0)) {
    result = NH_AUTH_RESULT_DENIED;
  } else if (request.operation == NH_AUTH_OP_CHECK_ACCOUNT) {
    result = dispatch_check_account(broker, request.payload_json);
  } else {
    result = NH_AUTH_RESULT_PROTOCOL_ERROR; /* other ops: later stages */
  }
  int rc = send_result(fd, &request, result);
  nh_auth_message_clear(&request);
  return rc;
}
