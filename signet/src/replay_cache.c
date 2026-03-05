/* SPDX-License-Identifier: MIT
 *
 * replay_cache.c - In-memory replay protection (Phase 2 implementation).
 */

#include "signet/replay_cache.h"

#include <glib.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
  char *id;          /* owned */
  int64_t seen_at;   /* unix seconds */
  GList *link;       /* link in insertion-order queue (oldest->newest) */
} SignetReplayEntry;

struct SignetReplayCache {
  size_t max_entries;
  uint32_t ttl_seconds;
  uint32_t skew_seconds;

  /* key: entry->id (do not free key separately; freed in value destroy) */
  GHashTable *by_id; /* char* -> SignetReplayEntry* */

  /* insertion-order queue for TTL pruning and bounded eviction */
  GQueue *order;     /* SignetReplayEntry* */

  GMutex mu;
};

static void signet_replay_entry_free(SignetReplayEntry *e) {
  if (!e) return;
  g_free(e->id);
  e->id = NULL;
  e->seen_at = 0;
  e->link = NULL;
  g_free(e);
}

static void signet_replay_evict_entry_locked(SignetReplayCache *c, SignetReplayEntry *e) {
  if (!c || !e) return;

  /* Remove from queue first while e is still valid. */
  if (e->link) {
    g_queue_delete_link(c->order, e->link);
    e->link = NULL;
  } else {
    (void)g_queue_remove(c->order, e);
  }

  /* Removing from the hash table frees the entry via value destroy. */
  (void)g_hash_table_remove(c->by_id, e->id);
}

static void signet_replay_prune_expired_locked(SignetReplayCache *c, int64_t now) {
  if (!c) return;
  if (c->ttl_seconds == 0) return;

  while (!g_queue_is_empty(c->order)) {
    SignetReplayEntry *e = (SignetReplayEntry *)g_queue_peek_head(c->order);
    if (!e) {
      (void)g_queue_pop_head(c->order);
      continue;
    }

    int64_t age = now - e->seen_at;
    if (age <= (int64_t)c->ttl_seconds) break;

    signet_replay_evict_entry_locked(c, e);
  }
}

SignetReplayCache *signet_replay_cache_new(const SignetReplayCacheConfig *cfg) {
  if (!cfg) return NULL;
  if (cfg->max_entries == 0) return NULL;

  SignetReplayCache *c = g_new0(SignetReplayCache, 1);
  if (!c) return NULL;

  c->max_entries = cfg->max_entries;
  c->ttl_seconds = cfg->ttl_seconds;
  c->skew_seconds = cfg->skew_seconds;

  g_mutex_init(&c->mu);

  c->order = g_queue_new();
  if (!c->order) {
    g_mutex_clear(&c->mu);
    g_free(c);
    return NULL;
  }

  c->by_id = g_hash_table_new_full(g_str_hash,
                                  g_str_equal,
                                  NULL, /* key freed by value destroy */
                                  (GDestroyNotify)signet_replay_entry_free);
  if (!c->by_id) {
    g_queue_free(c->order);
    g_mutex_clear(&c->mu);
    g_free(c);
    return NULL;
  }

  return c;
}

void signet_replay_cache_free(SignetReplayCache *c) {
  if (!c) return;

  g_mutex_lock(&c->mu);

  /* Destroying the hash table frees all entries. The queue then only holds
   * stale pointers in its nodes; g_queue_free() does not dereference them. */
  g_clear_pointer(&c->by_id, g_hash_table_destroy);
  g_clear_pointer(&c->order, g_queue_free);

  g_mutex_unlock(&c->mu);

  g_mutex_clear(&c->mu);
  g_free(c);
}

static bool signet_replay_skew_check(const SignetReplayCache *c,
                                     int64_t event_created_at,
                                     int64_t now,
                                     SignetReplayResult *out) {
  if (!c || !out) return false;

  if (c->skew_seconds == 0) {
    *out = SIGNET_REPLAY_OK;
    return true;
  }

  /* If created_at is missing/invalid, do not apply skew validation here. */
  if (event_created_at <= 0) {
    *out = SIGNET_REPLAY_OK;
    return true;
  }

  int64_t min_ok = now - (int64_t)c->skew_seconds;
  int64_t max_ok = now + (int64_t)c->skew_seconds;

  if (event_created_at < min_ok) {
    *out = SIGNET_REPLAY_TOO_OLD;
    return true;
  }
  if (event_created_at > max_ok) {
    *out = SIGNET_REPLAY_TOO_FAR_IN_FUTURE;
    return true;
  }

  *out = SIGNET_REPLAY_OK;
  return true;
}

SignetReplayResult signet_replay_check_and_mark(SignetReplayCache *c,
                                                const char *event_id_hex,
                                                int64_t event_created_at,
                                                int64_t now) {
  if (!c || !event_id_hex || event_id_hex[0] == '\0') return SIGNET_REPLAY_OK;

  SignetReplayResult skew_res = SIGNET_REPLAY_OK;
  (void)signet_replay_skew_check(c, event_created_at, now, &skew_res);
  if (skew_res != SIGNET_REPLAY_OK) return skew_res;

  /* Defensive bound to avoid pathological memory use if caller passes junk. */
  if (strlen(event_id_hex) > 256) return SIGNET_REPLAY_OK;

  g_mutex_lock(&c->mu);

  /* First, prune expired entries. */
  signet_replay_prune_expired_locked(c, now);

  /* Duplicate detection. */
  if (g_hash_table_contains(c->by_id, event_id_hex)) {
    g_mutex_unlock(&c->mu);
    return SIGNET_REPLAY_DUPLICATE;
  }

  /* Enforce bound with eviction from oldest. */
  while ((size_t)g_hash_table_size(c->by_id) >= c->max_entries && !g_queue_is_empty(c->order)) {
    SignetReplayEntry *old = (SignetReplayEntry *)g_queue_peek_head(c->order);
    if (!old) {
      (void)g_queue_pop_head(c->order);
      continue;
    }
    signet_replay_evict_entry_locked(c, old);
  }

  /* Insert new entry. */
  SignetReplayEntry *e = g_new0(SignetReplayEntry, 1);
  if (!e) {
    g_mutex_unlock(&c->mu);
    return SIGNET_REPLAY_OK;
  }

  e->id = g_strdup(event_id_hex);
  if (!e->id) {
    g_free(e);
    g_mutex_unlock(&c->mu);
    return SIGNET_REPLAY_OK;
  }

  e->seen_at = now;
  e->link = NULL;

  g_queue_push_tail(c->order, e);
  e->link = g_queue_peek_tail_link(c->order);

  /* Key pointer is e->id; key is not freed separately. */
  g_hash_table_insert(c->by_id, e->id, e);

  g_mutex_unlock(&c->mu);
  return SIGNET_REPLAY_OK;
}