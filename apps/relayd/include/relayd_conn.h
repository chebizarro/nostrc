#ifndef RELAYD_CONN_H
#define RELAYD_CONN_H

#include "rate_limit.h"
#include "relay_ingress.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NIP-01: a subscription id is a non-empty string of at most 64 chars. */
#define RELAYD_SUBID_MAX 64
/* Hard cap on active REQs one connection may retain at once; the
 * effective limit is min(cfg.max_subs, this). */
#define RELAYD_MAX_PENDING_SUBS 16
#define RELAYD_MAX_PENDING_ACKS 256
#define RELAYD_MAX_QUEUED_FRAMES 256
#define RELAYD_MAX_QUEUED_BYTES (2u * 1024u * 1024u)

struct NostrFilter;
typedef struct RelaydFrame {
  struct RelaydFrame *next;
  char *text;
  size_t length;
  char subid[RELAYD_SUBID_MAX + 1]; /* empty for control frames */
  char event_id[65];
} RelaydFrame;

/* Stored catch-up followed by live delivery, until CLOSE/replacement. */
typedef struct {
  void *it; /* storage iterator (from query or search); released by query_free */
  char subid[RELAYD_SUBID_MAX + 1];
  struct NostrFilter **filters;
  size_t nfilters;
  int eose_sent;
} RelaydPendingSub;

/* Storage ownership is independent of publisher connections and their timers. */
typedef struct RelaydPendingStore {
  struct RelaydPendingStore *next;
  RelayIngressResult ingress; /* replay reservation; event is serialized below */
  char *json;
  size_t bytes;
  char *old_replaceable_id;
} RelaydPendingStore;

typedef struct RelaydPendingAck {
  struct RelaydPendingAck *next;
  RelaydPendingStore *store; /* NULL once committed */
  char id[65];
  uint64_t deadline_ms;
  int ready;
  int duplicate;
} RelaydPendingAck;

struct lws;
typedef struct ConnState {
  struct ConnState *next_client;
  struct ConnState *prev_client;
  struct lws *wsi;
  RelaydPendingAck *ack_head;
  RelaydPendingAck *ack_tail;
  size_t nacks;
  char storage_notice_id[65]; /* outcome unknown: NOTICE then close, never OK */
  int storage_notice_sent;
  RelaydFrame *send_head;
  RelaydFrame *send_tail;
  size_t send_count;
  size_t send_bytes;
  int closing; /* queue overflow or write failure: disconnect, never drop silently */
  /* Backfill iterators are streamed in arrival order; EOSE retains filters. */
  RelaydPendingSub subs[RELAYD_MAX_PENDING_SUBS];
  size_t nsubs;
  size_t backfill_cursor;
  int prefer_backfill;
  int authed;
  int vault_authorized; /* Any successfully AUTHed allowlisted identity. */
  int need_auth_chal;
  char auth_chal[64];
  char authed_pubkey[128];
  char peer_ip[64];
  char *rx_buffer;
  size_t rx_length;
  size_t rx_capacity;

  RateLimitBucket frame_rate;
  RateLimitBucket byte_rate;
  RateLimitBucket verification_rate;

  void *neg_state;
  char neg_subid[128];
} ConnState;

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_CONN_H */
