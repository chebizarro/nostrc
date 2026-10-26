#ifndef RELAYD_CTX_H
#define RELAYD_CTX_H

#include <libwebsockets.h>
#include "nostr-storage.h"
#include "rate_limit.h"
#include "relay_policy.h"
#include "relayd_config.h"
#include "relayd_async_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ConnState;
struct RelaydPendingStore;
typedef struct {
  NostrStorage *storage;
  const RelaydAsyncStorageOps *async_storage;
  unsigned int ack_timeout_ms;
  struct lws_context *lws_context;
  lws_sorted_usec_list_t storage_read_retry;
  unsigned int storage_read_retry_ms;
  int storage_read_retry_active;
  struct ConnState *clients;
  struct RelaydPendingStore *pending_stores;
  size_t pending_store_count;
  size_t pending_store_bytes;
  size_t pending_acks; /* includes committed responses not yet sent */
  unsigned long long storage_saturation_count;
  RelaydConfig cfg;
  RelayPolicy *policy;
  VerificationBudget *verification_budget;
} RelaydCtx;

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_CTX_H */
