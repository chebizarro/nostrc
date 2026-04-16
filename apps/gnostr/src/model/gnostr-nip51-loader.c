/* gnostr-nip51-loader.c — NIP-51 list loader and FilterSet converter.
 *
 * SPDX-License-Identifier: MIT
 *
 * nostrc-yg8j.8.
 */

#define G_LOG_DOMAIN "gnostr-nip51-loader"

#include "gnostr-nip51-loader.h"

#include <stdlib.h>
#include <string.h>

#include <nostr-event.h>
#include <nostr-tag.h>
#include <json.h>
#include <nostr-gobject-1.0/nostr_utils.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr/nip51/nip51.h>

/* ------------------------------------------------------------------------
 * Container
 * ------------------------------------------------------------------------ */

struct _GnostrNip51UserLists {
  /* Elements: NostrList*. We own each entry and free it via a custom
   * destructor on the GPtrArray. */
  GPtrArray *lists;
};

static void
nostr_list_free_cb(gpointer data)
{
  /* nostr_nip51_list_free is NULL-safe. */
  nostr_nip51_list_free((NostrList *)data);
}

static GnostrNip51UserLists *
user_lists_new(void)
{
  GnostrNip51UserLists *u = g_new0(GnostrNip51UserLists, 1);
  u->lists = g_ptr_array_new_with_free_func(nostr_list_free_cb);
  return u;
}

void
gnostr_nip51_user_lists_free(GnostrNip51UserLists *lists)
{
  if (!lists) return;
  if (lists->lists)
    g_ptr_array_unref(lists->lists);
  g_free(lists);
}

gsize
gnostr_nip51_user_lists_get_count(const GnostrNip51UserLists *lists)
{
  return (lists && lists->lists) ? lists->lists->len : 0;
}

const void *
gnostr_nip51_user_lists_get_nth(const GnostrNip51UserLists *lists,
                                 gsize index)
{
  if (!lists || !lists->lists || index >= lists->lists->len)
    return NULL;
  return g_ptr_array_index(lists->lists, index);
}

/* ------------------------------------------------------------------------
 * Dedup helpers
 *
 * Kind 30000 is an addressable (NIP-33 parameterized-replaceable) event,
 * so the canonical identity is (pubkey, kind, d-tag). NDB caches all
 * versions we've ever seen, so the loader must dedup by d-tag and keep
 * the most recent created_at. We do that as a first pass over the
 * NostrEvent instances: keep the winner for each d-tag, drop the rest.
 * ------------------------------------------------------------------------ */

/* Extract the first "d" tag value from an event, or NULL if none.
 * The returned pointer borrows from the event's tag storage and is
 * valid while @event is alive. */
static const char *
event_get_d_tag(NostrEvent *event)
{
  NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
  if (!tags) return NULL;
  size_t n = nostr_tags_size(tags);
  for (size_t i = 0; i < n; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2) continue;
    const char *k = nostr_tag_get(tag, 0);
    const char *v = nostr_tag_get(tag, 1);
    if (k && v && strcmp(k, "d") == 0) return v;
  }
  return NULL;
}

/* ------------------------------------------------------------------------
 * Worker
 * ------------------------------------------------------------------------ */

typedef struct {
  gchar *pubkey_hex;
} Nip51LoadCtx;

static void
nip51_load_ctx_free(gpointer data)
{
  Nip51LoadCtx *ctx = data;
  if (!ctx) return;
  g_free(ctx->pubkey_hex);
  g_free(ctx);
}

/* Entry-count helper for the has-any-p-tag filter. */
static gboolean
list_has_p_entry(const NostrList *list)
{
  if (!list || !list->entries) return FALSE;
  for (size_t i = 0; i < list->count; i++) {
    const NostrListEntry *e = list->entries[i];
    if (e && e->tag_name && strcmp(e->tag_name, "p") == 0)
      return TRUE;
  }
  return FALSE;
}

/* Dedup struct: latest event seen for a given d-tag. */
typedef struct {
  NostrEvent *event;           /* owned */
  int64_t     created_at;
} DedupSlot;

static void
dedup_slot_free(gpointer data)
{
  DedupSlot *s = data;
  if (!s) return;
  if (s->event) nostr_event_free(s->event);
  g_free(s);
}

/* Runs on a worker thread. Takes ownership of @task_data. */
static void
nip51_load_thread(GTask *task, gpointer source_object,
                  gpointer task_data, GCancellable *cancellable)
{
  (void)source_object;
  Nip51LoadCtx *ctx = task_data;

  if (g_task_return_error_if_cancelled(task))
    return;

  /* Build a single-filter JSON array for kind 30000 authored by us.
   * No limit — addressable events should be sparse, and NDB handles
   * large result sets well. */
  g_autofree gchar *filter_json = g_strdup_printf(
      "[{\"kinds\":[30000],\"authors\":[\"%s\"]}]",
      ctx->pubkey_hex);

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10, NULL) != 0) {
    g_debug("nip51-loader: NDB read txn unavailable");
    g_task_return_pointer(task, user_lists_new(),
                          (GDestroyNotify)gnostr_nip51_user_lists_free);
    return;
  }

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count, NULL);

  if (rc != 0 || !results || count <= 0) {
    if (results) storage_ndb_free_results(results, count);
    storage_ndb_end_query(txn);
    g_task_return_pointer(task, user_lists_new(),
                          (GDestroyNotify)gnostr_nip51_user_lists_free);
    return;
  }

  /* Dedup by d-tag: hash table keyed on a copy of the d-tag value,
   * value is a DedupSlot* that owns the winner event. */
  GHashTable *winners = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, dedup_slot_free);

  for (int i = 0; i < count; i++) {
    if (g_cancellable_is_cancelled(cancellable)) break;
    const char *json = results[i];
    if (!json || !*json) continue;

    NostrEvent *ev = nostr_event_new();
    if (!ev) continue;
    if (nostr_event_deserialize(ev, json) != 0) {
      nostr_event_free(ev);
      continue;
    }

    /* Defensive: reject anything that isn't actually kind 30000 or
     * wasn't authored by the expected pubkey. NDB's filter engine
     * should already guarantee this, but a corrupt cache entry
     * mustn't end up in the filter-set importer. */
    if (nostr_event_get_kind(ev) != 30000) {
      nostr_event_free(ev);
      continue;
    }
    const char *author = nostr_event_get_pubkey(ev);
    if (!author || g_ascii_strcasecmp(author, ctx->pubkey_hex) != 0) {
      nostr_event_free(ev);
      continue;
    }

    const char *d_tag = event_get_d_tag(ev);
    if (!d_tag || !*d_tag) {
      /* No d-tag — technically not a valid addressable list.
       * Ignore. */
      nostr_event_free(ev);
      continue;
    }
    int64_t created_at = nostr_event_get_created_at(ev);

    DedupSlot *existing = g_hash_table_lookup(winners, d_tag);
    if (existing && existing->created_at >= created_at) {
      /* Older duplicate — drop. */
      nostr_event_free(ev);
      continue;
    }

    /* We're the new winner for this d-tag. */
    DedupSlot *slot = g_new0(DedupSlot, 1);
    slot->event = ev;
    slot->created_at = created_at;
    g_hash_table_replace(winners, g_strdup(d_tag), slot);
  }

  storage_ndb_free_results(results, count);
  storage_ndb_end_query(txn);

  /* Parse each winner and drop lists without any p-tag entries. */
  GnostrNip51UserLists *out = user_lists_new();

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, winners);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    DedupSlot *slot = value;
    if (!slot || !slot->event) continue;

    NostrList *list = nostr_nip51_parse_list(slot->event, NULL);
    if (!list) continue;

    if (!list_has_p_entry(list)) {
      nostr_nip51_list_free(list);
      continue;
    }

    g_ptr_array_add(out->lists, list);
  }

  /* winners destroys slots, which free the events. */
  g_hash_table_destroy(winners);

  g_debug("nip51-loader: loaded %u list(s) for %.8s...",
          out->lists->len, ctx->pubkey_hex);

  g_task_return_pointer(task, out,
                        (GDestroyNotify)gnostr_nip51_user_lists_free);
}

/* ------------------------------------------------------------------------
 * Public async API
 * ------------------------------------------------------------------------ */

void
gnostr_nip51_load_user_lists_async(const gchar *pubkey_hex,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
  g_return_if_fail(callback != NULL);

  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gnostr_nip51_load_user_lists_async);

  if (!pubkey_hex || strlen(pubkey_hex) != 64) {
    g_task_return_new_error(task,
                            G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "nip51-loader: pubkey_hex must be 64 hex chars");
    g_object_unref(task);
    return;
  }

  Nip51LoadCtx *ctx = g_new0(Nip51LoadCtx, 1);
  ctx->pubkey_hex = g_ascii_strdown(pubkey_hex, -1);
  g_task_set_task_data(task, ctx, nip51_load_ctx_free);

  g_task_run_in_thread(task, nip51_load_thread);
  g_object_unref(task);
}

GnostrNip51UserLists *
gnostr_nip51_load_user_lists_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  g_return_val_if_fail(
      g_task_is_valid(result, NULL) ||
          g_async_result_is_tagged(result, gnostr_nip51_load_user_lists_async),
      NULL);

  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ------------------------------------------------------------------------
 * List → FilterSet converter
 * ------------------------------------------------------------------------ */

GnostrFilterSet *
gnostr_nip51_list_to_filter_set(const void *nostr_list,
                                 const gchar *proposed_name)
{
  if (!nostr_list) return NULL;
  const NostrList *list = (const NostrList *)nostr_list;

  /* Collect normalized hex pubkeys from p-tag entries. */
  GPtrArray *authors = g_ptr_array_new_with_free_func(g_free);
  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (size_t i = 0; i < list->count; i++) {
    const NostrListEntry *entry = list->entries[i];
    if (!entry || !entry->tag_name || !entry->value) continue;
    if (strcmp(entry->tag_name, "p") != 0) continue;

    gchar *hex = gnostr_ensure_hex_pubkey(entry->value);
    if (!hex || !*hex) { g_free(hex); continue; }

    /* Dedup case-insensitively by lowering before lookup. */
    gchar *lower = g_ascii_strdown(hex, -1);
    if (g_hash_table_contains(seen, lower)) {
      g_free(lower);
      g_free(hex);
      continue;
    }
    g_hash_table_insert(seen, lower, GINT_TO_POINTER(1));
    /* `seen` owns `lower`; we've stored `hex` separately in authors. */
    g_ptr_array_add(authors, hex);
  }
  g_hash_table_destroy(seen);

  if (authors->len == 0) {
    g_ptr_array_unref(authors);
    return NULL;
  }

  /* Build the FilterSet. */
  GnostrFilterSet *fs = gnostr_filter_set_new();
  gnostr_filter_set_set_source(fs, GNOSTR_FILTER_SET_SOURCE_CUSTOM);

  /* Name resolution: proposed > list title > list identifier >
   * generic fallback. */
  const gchar *name = NULL;
  if (proposed_name && *proposed_name)
    name = proposed_name;
  else if (list->title && *list->title)
    name = list->title;
  else if (list->identifier && *list->identifier)
    name = list->identifier;
  else
    name = "NIP-51 List";
  gnostr_filter_set_set_name(fs, name);

  if (list->description && *list->description)
    gnostr_filter_set_set_description(fs, list->description);

  /* Terminate with a NULL so set_authors sees a proper GStrv. */
  g_ptr_array_add(authors, NULL);
  gnostr_filter_set_set_authors(
      fs, (const gchar * const *)authors->pdata);
  /* authors still owns every non-NULL entry; unref frees them. */
  g_ptr_array_unref(authors);

  return fs;
}
