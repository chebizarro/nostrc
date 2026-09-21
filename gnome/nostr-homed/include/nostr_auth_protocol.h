#ifndef NOSTR_AUTH_PROTOCOL_H
#define NOSTR_AUTH_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NH_AUTH_PROTOCOL_VERSION 1u
#define NH_AUTH_PACKET_MAX 65536u
#define NH_AUTH_SECRET_MAX 1024u
#define NH_AUTH_PROOF_MAX 16384u
#define NH_AUTH_REQUEST_ID_HEX_LEN 64u
#define NH_AUTH_TRANSACTION_ID_HEX_LEN 64u
#define NH_AUTH_CHALLENGE_KIND 1
#define NH_AUTH_CHALLENGE_LIFETIME_SEC 120u
#define NH_AUTH_TRANSACTION_LIFETIME_SEC 180u
#define NH_AUTH_RECEIPT_LIFETIME_SEC 300u

typedef enum nh_auth_endpoint {
  NH_AUTH_ENDPOINT_AUTH = 1,
  NH_AUTH_ENDPOINT_USER = 2,
  NH_AUTH_ENDPOINT_WORKER = 3
} nh_auth_endpoint;

typedef enum nh_auth_operation {
  NH_AUTH_OP_BEGIN_LOGIN = 1,
  NH_AUTH_OP_SELECT_PROVIDER,
  NH_AUTH_OP_SUBMIT_UNLOCK,
  NH_AUTH_OP_WAIT_RESULT,
  NH_AUTH_OP_CANCEL,
  NH_AUTH_OP_CHECK_ACCOUNT,
  NH_AUTH_OP_OPEN_LOCAL_SESSION,
  NH_AUTH_OP_CLOSE_LOCAL_SESSION,
  NH_AUTH_OP_BEGIN_SMB_PROOF,
  NH_AUTH_OP_ADMIN
} nh_auth_operation;

typedef enum nh_auth_result {
  NH_AUTH_RESULT_OK = 0,
  NH_AUTH_RESULT_UNKNOWN_ACCOUNT,
  NH_AUTH_RESULT_DISABLED,
  NH_AUTH_RESULT_NOT_READY,
  NH_AUTH_RESULT_DENIED,
  NH_AUTH_RESULT_INVALID_PROOF,
  NH_AUTH_RESULT_EXPIRED,
  NH_AUTH_RESULT_CANCELLED,
  NH_AUTH_RESULT_RATE_LIMITED,
  NH_AUTH_RESULT_PROVIDER_UNAVAILABLE,
  NH_AUTH_RESULT_NETWORK_UNAVAILABLE,
  NH_AUTH_RESULT_INTERACTION_REQUIRED,
  NH_AUTH_RESULT_STORAGE_ERROR,
  NH_AUTH_RESULT_PROTOCOL_ERROR,
  NH_AUTH_RESULT_INTERNAL_ERROR
} nh_auth_result;

typedef struct nh_auth_message {
  nh_auth_operation operation;
  char request_id[NH_AUTH_REQUEST_ID_HEX_LEN + 1];
  char transaction_id[NH_AUTH_TRANSACTION_ID_HEX_LEN + 1];
  char *payload_json;
} nh_auth_message;

const char *nh_auth_operation_name(nh_auth_operation operation);
const char *nh_auth_result_name(nh_auth_result result);
void nh_auth_message_clear(nh_auth_message *message);
int nh_auth_message_parse(const void *packet, size_t packet_len,
                          nh_auth_message *out);
int nh_auth_operation_allowed(nh_auth_endpoint endpoint,
                              nh_auth_operation operation, uid_t peer_uid,
                              int owns_transaction);

/* Reads exactly one SOCK_SEQPACKET record. MSG_TRUNC and any ancillary data
 * are rejected. The caller owns *packet_out and must wipe it if secret. */
int nh_auth_recv_packet(int fd, unsigned char **packet_out, size_t *packet_len);

/* Serializes one request/response envelope and writes it as a single
 * SOCK_SEQPACKET record. payload_json must be a JSON object string (NULL or
 * empty is treated as {}). No ancillary data is sent. Returns 0 on success. */
int nh_auth_send_message(int fd, const nh_auth_message *message);

#endif
