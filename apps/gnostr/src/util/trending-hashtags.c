/**
 * trending-hashtags.c - Local trending hashtag computation
 *
 * Scans recent kind-1 notes in NDB, extracts "t" tags, and returns the
 * top N by frequency. Runs entirely against the local store.
 */

#define G_LOG_DOMAIN "gnostr-trending"

#include "trending-hashtags.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include <string.h>
#include <ctype.h>

/* NDB internal headers needed for direct iteration */
#include <nostrdb.h>

/* --- Helpers --- */

void
gnostr_trending_hashtag_free(GnostrTrendingHashtag *ht)
{
  if (!ht) return;
  g_free(ht->tag);
  g_free(ht);
}

/* Normalize a hashtag: lowercase, strip leading/trailing whitespace.
 * Returns a newly allocated string or NULL if invalid. */
static char *
normalize_hashtag(const char *raw)
{
  if (!raw || !*raw) return NULL;

  /* Skip leading whitespace */
  while (*raw && g_ascii_isspace(*raw)) raw++;
  if (!*raw) return NULL;

  /* Must be valid UTF-8 */
  if (!g_utf8_validate(raw, -1, NULL)) return NULL;

  gchar *lower = g_utf8_strdown(raw, -1);
  if (!lower) return NULL;

  /* Trim trailing whitespace */
  gsize len = strlen(lower);
  while (len > 0 && g_ascii_isspace(lower[len - 1])) {
    lower[--len] = '\0';
  }

  /* Filter: must be at least 2 chars */
  if (len < 2) {
    g_free(lower);
    return NULL;
  }

  /* Filter: must contain at least one alphanumeric char */
  gboolean has_alnum = FALSE;
  for (gsize i = 0; i < len; i++) {
    if (g_ascii_isalnum(lower[i])) { has_alnum = TRUE; break; }
  }
  if (!has_alnum) {
    g_free(lower);
    return NULL;
  }

  /* Filter: reject excessively long hashtags (likely spam) */
  if (len > 64) {
    g_free(lower);
    return NULL;
  }

  return lower;
}

/* Comparison for sorting by count descending */
static gint
compare_by_count_desc(gconstpointer a, gconstpointer b)
{
  const GnostrTrendingHashtag *ha = (const GnostrTrendingHashtag *)a;
  const GnostrTrendingHashtag *hb = (const GnostrTrendingHashtag *)b;
  
  /* Guard against NULL pointers */
  if (!ha && !hb) return 0;
  if (!ha) return 1;  /* NULL sorts last */
  if (!hb) return -1;
  
  if (ha->count > hb->count) return -1;
  if (ha->count < hb->count) return 1;
  return g_strcmp0(ha->tag, hb->tag);
}

/* --- Core computation --- */

GPtrArray *
gnostr_compute_trending_hashtags(guint max_events, guint top_n)
{
  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_trending_hashtag_free);

  if (max_events == 0 || top_n == 0)
    return result;

  /* Build NDB filter for recent kind-1 notes */
  struct ndb_filter filter;
  ndb_filter_init(&filter);
  ndb_filter_start_field(&filter, NDB_FILTER_KINDS);
  ndb_filter_add_int_element(&filter, 1);
  ndb_filter_end_field(&filter);
  ndb_filter_start_field(&filter, NDB_FILTER_LIMIT);
  ndb_filter_add_int_element(&filter, (int)max_events);
  ndb_filter_end_field(&filter);

  /* Open read transaction */
  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10, NULL) != 0) {
    g_warning("trending: failed to open NDB read transaction");
    ndb_filter_destroy(&filter);
    return result;
  }

  /* Execute query */
  enum { QUERY_CAP = 512 };
  struct ndb_query_result qres[QUERY_CAP];
  int got = 0;
  int cap = MIN((int)max_events, QUERY_CAP);

  if (!ndb_query((struct ndb_txn *)txn, &filter, 1, qres, cap, &got)) {
    g_warning("trending: NDB query failed");
    storage_ndb_end_query(txn);
    ndb_filter_destroy(&filter);
    return result;
  }

  /* Count hashtag occurrences: tag_lowercase -> count */
  GHashTable *counts = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);

  for (int i = 0; i < got; i++) {
    struct ndb_note *note = qres[i].note;
    if (!note) continue;

    /* Track which hashtags this event contributed (dedup within event)
     * seen_in_event owns all its keys and will free them when destroyed */
    GHashTable *seen_in_event = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    struct ndb_iterator iter;
    ndb_tags_iterate_start(note, &iter);

    while (ndb_tags_iterate_next(&iter)) {
      struct ndb_tag *tag = iter.tag;
      int nelem = ndb_tag_count(tag);
      if (nelem < 2) continue;

      /* Check for tag key "t" (must be packed string, NUL-terminated) */
      struct ndb_str key = ndb_tag_str(note, tag, 0);
      if (key.flag != NDB_PACKED_STR || !key.str || key.str[0] != 't' || key.str[1] != '\0') continue;

      struct ndb_str value = ndb_tag_str(note, tag, 1);
      if (value.flag != NDB_PACKED_STR || !value.str) continue;

      int value_len = ndb_str_len(&value);
      if (value_len <= 0) continue;

      /* ndb_str is not guaranteed to be safe for -1/strlen-based APIs */
      char *value_copy = g_strndup(value.str, (gsize)value_len);
      if (!value_copy) continue;

      char *normalized = normalize_hashtag(value_copy);
      g_free(value_copy);
      if (!normalized) continue;

      /* Skip if already counted for this event */
      if (g_hash_table_contains(seen_in_event, normalized)) {
        g_free(normalized);
        continue;
      }

      /* Mark as seen in this event - seen_in_event takes ownership */
      g_hash_table_insert(seen_in_event, normalized, GINT_TO_POINTER(1));

      /* Increment global count - counts table owns its own copy of the key */
      gpointer existing = g_hash_table_lookup(counts, normalized);
      guint existing_count = GPOINTER_TO_UINT(existing);
      
      /* Guard against overflow (extremely unlikely but theoretically possible) */
      if (existing_count >= G_MAXUINT) {
        continue;
      }
      
      guint count = existing_count + 1;
      if (!existing) {
        /* New hashtag - counts needs its own copy of the string */
        g_hash_table_insert(counts, g_strdup(normalized), GUINT_TO_POINTER(count));
      } else {
        /* Existing hashtag - just update the count, counts already owns the key */
        gpointer orig_key;
        if (g_hash_table_lookup_extended(counts, normalized, &orig_key, NULL)) {
          g_hash_table_replace(counts, orig_key, GUINT_TO_POINTER(count));
        }
      }
    }

    /* Clean up seen_in_event - it owns all its keys and will free them */
    g_hash_table_destroy(seen_in_event);
  }

  storage_ndb_end_query(txn);
  ndb_filter_destroy(&filter);

  /* Convert counts to result array */
  GHashTableIter ht_iter;
  gpointer key, value;
  GPtrArray *all = g_ptr_array_new();  /* No free function - we'll manage memory manually */

  g_hash_table_iter_init(&ht_iter, counts);
  while (g_hash_table_iter_next(&ht_iter, &key, &value)) {
    guint count = GPOINTER_TO_UINT(value);
    if (count < 2) continue; /* Require at least 2 events to be "trending" */

    GnostrTrendingHashtag *ht = g_new0(GnostrTrendingHashtag, 1);
    /* Duplicate the string so it survives hash table destruction */
    ht->tag = g_strdup((const char *)key);
    ht->count = count;
    g_ptr_array_add(all, ht);
  }

  /* Sort by count descending BEFORE destroying counts hash table */
  g_ptr_array_sort(all, compare_by_count_desc);

  /* Now safe to destroy counts - we've duplicated all the strings */
  g_hash_table_destroy(counts);

  /* Take top N and move to result */
  guint take = MIN(top_n, all->len);
  for (guint i = 0; i < take; i++) {
    g_ptr_array_add(result, g_ptr_array_index(all, i));
  }
  
  /* Remove items from 'all' that we moved to result (without freeing them) */
  if (take > 0) {
    g_ptr_array_remove_range(all, 0, take);
  }
  
  /* Now 'all' only contains items we didn't take - set free function and unref to free them */
  g_ptr_array_set_free_func(all, (GDestroyNotify)gnostr_trending_hashtag_free);
  g_ptr_array_unref(all);

  g_debug("trending: computed %u hashtags from %d events", result->len, got);
  return result;
}

/* --- Async wrapper --- */

typedef struct {
  guint max_events;
  guint top_n;
  GnostrTrendingHashtagsCallback callback;
  gpointer user_data;
  GPtrArray *result;
  GCancellable *cancellable;
} TrendingAsyncData;

static void
trending_async_thread(GTask *task, gpointer source_object,
                      gpointer task_data, GCancellable *cancellable)
{
  (void)source_object;
  TrendingAsyncData *data = task_data;

  /* Check if cancelled before doing work */
  if (g_task_return_error_if_cancelled(task)) {
    return;
  }

  data->result = gnostr_compute_trending_hashtags(data->max_events, data->top_n);

  /* Return result via GTask - this handles main thread delivery automatically */
  g_task_return_pointer(task, data, NULL);
}

static void
trending_async_callback(GObject *source_object, GAsyncResult *result, gpointer user_data)
{
  (void)source_object;
  (void)user_data;
  
  GTask *task = G_TASK(result);
  TrendingAsyncData *data = g_task_propagate_pointer(task, NULL);
  
  if (data) {
    /* Deliver result to original callback */
    if (data->result) {
      data->callback(data->result, data->user_data);
    }
    
    g_clear_object(&data->cancellable);
    g_free(data);
  }
}

void
gnostr_compute_trending_hashtags_async(guint max_events,
                                       guint top_n,
                                       GnostrTrendingHashtagsCallback callback,
                                       gpointer user_data,
                                       GCancellable *cancellable)
{
  g_return_if_fail(callback != NULL);

  TrendingAsyncData *data = g_new0(TrendingAsyncData, 1);
  data->max_events = max_events;
  data->top_n = top_n;
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable ? g_object_ref(cancellable) : NULL;

  GTask *task = g_task_new(NULL, cancellable, trending_async_callback, NULL);
  g_task_set_task_data(task, data, NULL); /* data freed in callback */
  g_task_run_in_thread(task, trending_async_thread);
  g_object_unref(task);
}
