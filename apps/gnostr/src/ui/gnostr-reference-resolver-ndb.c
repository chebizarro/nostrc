/* gnostr NDB reference resolver (nostrc-8xfib.6). Ported from the timeline
 * factory's NIP-18 block (storage_ndb note + profile lookups) and the
 * relay query of gnostr-timeline-embed.c. Fetching stays gnostr policy:
 * the factory asks for it; the shared card never does on bind. */
#include "gnostr-reference-resolver-ndb.h"
#include "gnostr-timeline-embed-private.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include "nostr-filter.h"
#include <stdlib.h>
#include <string.h>

struct _GnostrReferenceResolverNdb {
  GObject parent_instance;
  GNostrPool *pool;
};

static void resolver_iface_init(GnNostrReferenceResolverInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(GnostrReferenceResolverNdb, gnostr_reference_resolver_ndb,
  G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_NOSTR_REFERENCE_RESOLVER, resolver_iface_init))

static gchar *lookup_local(GnNostrReferenceResolver *resolver, const GnNostrReference *reference) {
  (void)resolver;
  if (reference->type != GN_NOSTR_REFERENCE_EVENT || !reference->id ||
      strlen(reference->id) != 64) return NULL;
  char *json = NULL;
  int len = 0;
  if (storage_ndb_get_note_by_id_nontxn(reference->id, &json, &len) != 0 || !json)
    return NULL;
  gchar *copy = g_strndup(json, (gsize)len);
  free(json);
  return copy;
}

gchar *gnostr_reference_resolver_ndb_profile_field(const gchar *pubkey_hex, const gchar *field) {
  unsigned char pk[32];
  if (!pubkey_hex || !gnostr_timeline_embed_hex32_from_string(pubkey_hex, pk)) return NULL;
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) return NULL;
  gchar *value = NULL;
  char *profile_json = NULL; /* borrowed from the transaction */
  int profile_len = 0;
  if (storage_ndb_get_profile_by_pubkey(txn, pk, &profile_json, &profile_len, NULL) == 0 &&
      profile_json && gnostr_json_is_valid(profile_json)) {
    g_autofree gchar *content = gnostr_json_get_string(profile_json, "content", NULL);
    if (content && gnostr_json_is_valid(content)) {
      value = gnostr_json_get_string(content, field, NULL);
      if (!value || !*value) {
        g_clear_pointer(&value, g_free);
        if (g_str_equal(field, "display_name"))
          value = gnostr_json_get_string(content, "name", NULL);
      }
      if (value && !*value) g_clear_pointer(&value, g_free);
    }
  }
  storage_ndb_end_query(txn);
  return value;
}

static gchar *display_name(GnNostrReferenceResolver *resolver, const gchar *pubkey_hex) {
  (void)resolver;
  return gnostr_reference_resolver_ndb_profile_field(pubkey_hex, "display_name");
}

static gboolean can_fetch(GnNostrReferenceResolver *resolver) {
  (void)resolver;
  return TRUE;
}

static void on_query_done(GObject *source, GAsyncResult *result, gpointer user_data) {
  g_autoptr(GTask) task = user_data;
  const GnNostrReference *reference = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events = gnostr_pool_query_finish(GNOSTR_POOL(source), result, &error);
  for (guint i = 0; events && i < events->len; i++) {
    const gchar *json = g_ptr_array_index(events, i);
    if (!json || !gn_nostr_reference_matches_event(reference, json)) continue;
    GPtrArray *ingest = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ingest, g_strdup(json));
    storage_ndb_ingest_events_async(ingest);
    g_task_return_pointer(task, g_strdup(json), g_free);
    return;
  }
  if (error) g_task_return_error(task, g_steal_pointer(&error));
  else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "No matching signed note on the read relays");
}

static void reference_free(gpointer data) { gn_nostr_reference_free(data); }

static void fetch_async(GnNostrReferenceResolver *resolver, const GnNostrReference *reference,
                        GCancellable *cancellable, GAsyncReadyCallback callback,
                        gpointer user_data) {
  GnostrReferenceResolverNdb *self = GNOSTR_REFERENCE_RESOLVER_NDB(resolver);
  g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, fetch_async);
  g_task_set_task_data(task, gn_nostr_reference_parse(reference->uri), reference_free);
  if (!g_task_get_task_data(task) || !reference->id ||
      reference->type == GN_NOSTR_REFERENCE_PERSON) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Not a note reference");
    return;
  }
  g_autoptr(GPtrArray) urls = gnostr_get_read_relay_urls();
  if (!urls || urls->len == 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No read relays");
    return;
  }
  NostrFilter *filter = nostr_filter_new();
  if (reference->type == GN_NOSTR_REFERENCE_EVENT) {
    const char *ids[] = { reference->id };
    nostr_filter_set_ids(filter, ids, 1);
  } else {
    const char *authors[] = { reference->author };
    const int kinds[] = { reference->kind };
    nostr_filter_set_authors(filter, authors, 1);
    nostr_filter_set_kinds(filter, kinds, 1);
    nostr_filter_tags_append(filter, "d", reference->id, NULL);
    nostr_filter_set_limit(filter, 20);
  }
  NostrFilters *filters = nostr_filters_new();
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter);
  gnostr_pool_query_urls_async(self->pool, (const gchar **)urls->pdata, urls->len,
                               filters, cancellable, on_query_done, g_steal_pointer(&task));
}

static gchar *fetch_finish(GnNostrReferenceResolver *resolver, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, resolver), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void resolver_iface_init(GnNostrReferenceResolverInterface *iface) {
  iface->lookup_local = lookup_local;
  iface->display_name = display_name;
  iface->can_fetch = can_fetch;
  iface->fetch_async = fetch_async;
  iface->fetch_finish = fetch_finish;
}

static void gnostr_reference_resolver_ndb_dispose(GObject *object) {
  g_clear_object(&GNOSTR_REFERENCE_RESOLVER_NDB(object)->pool);
  G_OBJECT_CLASS(gnostr_reference_resolver_ndb_parent_class)->dispose(object);
}

static void gnostr_reference_resolver_ndb_class_init(GnostrReferenceResolverNdbClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = gnostr_reference_resolver_ndb_dispose;
}

static void gnostr_reference_resolver_ndb_init(GnostrReferenceResolverNdb *self) {
  self->pool = gnostr_pool_new();
}

GnNostrReferenceResolver *gnostr_reference_resolver_ndb_get_default(void) {
  static GnNostrReferenceResolver *resolver; /* GTK thread only */
  if (!resolver) resolver = g_object_new(GNOSTR_TYPE_REFERENCE_RESOLVER_NDB, NULL);
  return resolver;
}
