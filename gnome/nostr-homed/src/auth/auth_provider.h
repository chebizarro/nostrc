#ifndef NH_AUTH_PROVIDER_H
#define NH_AUTH_PROVIDER_H

#include <stddef.h>
#include <stdint.h>
#include "nostr_auth_protocol.h"
#include "nostr_identity.h"

typedef struct nh_auth_provider nh_auth_provider;
typedef enum nh_auth_provider_event_type { NH_AUTH_PROVIDER_READY=1, NH_AUTH_PROVIDER_UNLOCK_REQUIRED, NH_AUTH_PROVIDER_APPROVAL_PENDING, NH_AUTH_PROVIDER_SIGNED_EVENT, NH_AUTH_PROVIDER_DENIED, NH_AUTH_PROVIDER_UNAVAILABLE, NH_AUTH_PROVIDER_INTERACTION_REQUIRED, NH_AUTH_PROVIDER_FAILED } nh_auth_provider_event_type;

typedef struct nh_auth_provider_snapshot {
  nh_identity_provider_type type;
  const char *provider_id, *account_id, *pubkey_hex;
  uint64_t key_generation, deadline_monotonic_ms;
  const char *public_config_json;
  const uint8_t *secret_blob; size_t secret_blob_len;
} nh_auth_provider_snapshot;
typedef struct nh_auth_immutable_challenge {
  const char *unsigned_event_json, *expected_event_id;
  size_t unsigned_event_json_len;
  uint64_t deadline_monotonic_ms;
} nh_auth_immutable_challenge;
typedef struct nh_auth_provider_event {
  nh_auth_provider_event_type type; nh_auth_result result;
  const uint8_t *data; size_t data_len;
} nh_auth_provider_event;
typedef void (*nh_auth_provider_event_fn)(void *context,const nh_auth_provider_event *event);

typedef struct nh_auth_provider_ops {
  int (*prepare)(nh_auth_provider *,const nh_auth_provider_snapshot *);
  int (*begin_proof)(nh_auth_provider *,const nh_auth_immutable_challenge *);
  int (*submit_unlock)(nh_auth_provider *,const uint8_t *,size_t);
  void (*cancel)(nh_auth_provider *);
  void (*destroy)(nh_auth_provider *);
} nh_auth_provider_ops;

/* Internal, non-plugin ABI. Call inputs are borrowed only for that call. Event
 * data is immutable and borrowed only for the callback duration. A
 * signed_event is a complete JSON event, never success. */
struct nh_auth_provider { const nh_auth_provider_ops *ops; nh_auth_provider_event_fn emit; void *emit_context; };

nh_auth_provider *nh_auth_provider_local_new(nh_auth_provider_event_fn emit,
                                             void *emit_context);

#endif
