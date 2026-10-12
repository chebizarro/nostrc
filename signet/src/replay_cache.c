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
  char *scope;       /* owned; NULL when unscoped */
  int64_t seen_at;   /* unix seconds */
  GList *link;       /* link in insertion-order queue (oldest->newest) */
} SignetReplayEntry;

struct SignetReplayCache {
  size_t max_entries;
  uint32_t ttl_seconds;
  uint32_t skew_seconds;
  size_t max_entries_per_scope;
  bool refuse_when_full;

  /* scope -> number of cached ids charged to it (GUINT_TO_POINTER) */
  GHashTable *scope_counts;

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
  g_free(e->scope);
  e->scope = NULL;
  e->seen_at = 0;
  e->link = NULL;
  g_free(e);
}

static void signet_replay_evict_entry_locked(SignetReplayCache *c, SignetReplayEntry *e) {
  if (!c || !e) return;

  if (e->scope) {
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(c->scope_counts, e->scope));
    if (n <= 1) (void)g_hash_table_remove(c->scope_counts, e->scope);
    else g_hash_table_insert(c->scope_counts, g_strdup(e->scope),
                             GUINT_TO_POINTER(n - 1));
  }

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
  /* An id must outlive the window in which its created_at passes the skew
   * check ([seen - skew, seen + skew] as seen at mark time). */
  if (c->skew_seconds > 0 && c->ttl_seconds < 2u * c->skew_seconds)
    c->ttl_seconds = 2u * c->skew_seconds;
  c->max_entries_per_scope = cfg->max_entries_per_scope;
  c->refuse_when_full = cfg->refuse_when_full;
  c->scope_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

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
  g_clear_pointer(&c->scope_counts, g_hash_table_destroy);

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

  /* A missing/invalid timestamp on an event we are asked to skew-check is
   * malformed; fail closed rather than silently skipping validation. */
  if (event_created_at <= 0) {
    *out = SIGNET_REPLAY_INVALID;
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
  return signet_replay_check_and_mark_scoped(c, NULL, event_id_hex,
                                             event_created_at, now);
}

SignetReplayResult signet_replay_check_and_mark_scoped(SignetReplayCache *c,
                                                       const char *scope,
                                                       const char *event_id_hex,
                                                       int64_t event_created_at,
                                                       int64_t now) {
  if (!c) return SIGNET_REPLAY_OK; /* replay protection disabled */
  /* Missing/empty event id means we cannot dedup; reject rather than admit. */
  if (!event_id_hex || event_id_hex[0] == '\0') return SIGNET_REPLAY_INVALID;

  /* Defensive bound to avoid pathological memory use if caller passes junk.
   * An overlong id is malformed input, so fail closed rather than admit it. */
  if (strlen(event_id_hex) > 256) return SIGNET_REPLAY_INVALID;
  if (scope && strlen(scope) > 256) return SIGNET_REPLAY_INVALID;

  g_mutex_lock(&c->mu);

  /* First, prune expired entries. */
  signet_replay_prune_expired_locked(c, now);

  /* Duplicate detection comes before the clock window: a redelivered,
   * already-executed request is a duplicate (dropped silently by callers)
   * even when its created_at has since left the window. */
  if (g_hash_table_contains(c->by_id, event_id_hex)) {
    g_mutex_unlock(&c->mu);
    return SIGNET_REPLAY_DUPLICATE;
  }

  SignetReplayResult skew_res = SIGNET_REPLAY_OK;
  (void)signet_replay_skew_check(c, event_created_at, now, &skew_res);
  if (skew_res != SIGNET_REPLAY_OK) {
    g_mutex_unlock(&c->mu);
    return skew_res;
  }

  /* A scope at its quota is refused: its own ids are never dropped early. */
  if (scope && c->max_entries_per_scope > 0 &&
      GPOINTER_TO_UINT(g_hash_table_lookup(c->scope_counts, scope)) >=
          c->max_entries_per_scope) {
    g_mutex_unlock(&c->mu);
    return SIGNET_REPLAY_FULL;
  }
  if (c->refuse_when_full &&
      (size_t)g_hash_table_size(c->by_id) >= c->max_entries) {
    g_mutex_unlock(&c->mu);
    return SIGNET_REPLAY_FULL;
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
  if (scope) {
    e->scope = g_strdup(scope);
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(c->scope_counts, scope));
    g_hash_table_insert(c->scope_counts, g_strdup(scope),
                        GUINT_TO_POINTER(n + 1));
  }

  g_queue_push_tail(c->order, e);
  e->link = g_queue_peek_tail_link(c->order);

  /* Key pointer is e->id; key is not freed separately. */
  g_hash_table_insert(c->by_id, e->id, e);

  g_mutex_unlock(&c->mu);
  return SIGNET_REPLAY_OK;
}