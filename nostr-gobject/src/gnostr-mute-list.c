#include "gnostr-mute-list.h"
#include "gnostr-list-private.h"
#include "gnostr-list-gnostr-adapter.h"
#include "gnostr-relays.h"
#include "nostr-kinds.h"
#include <string.h>

/* The ABI remains an opaque singleton. Lookup strings are interned for its
 * lifetime because the legacy accessors return borrowed strings in an owned
 * container. All lookup/projection changes are serialized by lock. */
struct _GNostrMuteList {
  gatomicrefcount refs;
  GMutex lock;
  GMainContext *context;
  GThread *thread;
  gboolean closed, dirty;
  guint64 generation;
  guint pending;
  gint64 last_time;
  char *account;
  char **relays;
  NostrListDocument *projection;
  GHashTable *interned;
  GHashTable *index[4];
  GNostrListService *service;
  gpointer fetch;
};
static GNostrMuteList *singleton;
static GMutex singleton_lock;
static const char *tag_names[] = {"p", "word", "t", "e"};
static const char *local_account = "0000000000000000000000000000000000000000000000000000000000000000";
static GNostrMuteList *facade_ref(GNostrMuteList *s) { g_atomic_ref_count_inc(&s->refs); return s; }
static void facade_unref(gpointer data) {
  GNostrMuteList *s = data; if (!g_atomic_ref_count_dec(&s->refs)) return;
  g_clear_object(&s->service); nostr_nip51_document_free(s->projection);
  for (guint i = 0; i < 4; i++) g_hash_table_unref(s->index[i]);
  g_hash_table_unref(s->interned); g_free(s->account); g_strfreev(s->relays);
  g_main_context_unref(s->context); g_mutex_clear(&s->lock); g_free(s);
}
static void post(GNostrMuteList *s, GSourceFunc fn, gpointer data, GDestroyNotify destroy) {
  GSource *source = g_idle_source_new(); g_source_set_callback(source, fn, data, destroy);
  g_source_attach(source, s->context); g_source_unref(source);
}
static char *normalized(guint index, const char *value) {
  if (!value || !g_utf8_validate(value, -1, NULL)) return NULL;
  if (index == 2 && *value == '#') value++;
  return index == 0 || index == 3 ? g_ascii_strdown(value, -1) : g_utf8_strdown(value, -1);
}
static void rebuild_locked(GNostrMuteList *s, const NostrListDocument *codec) {
  NostrListDocument *copy = list_codec_copy(codec); if (!copy) return;
  nostr_nip51_document_free(s->projection); s->projection = copy;
  for (guint i = 0; i < 4; i++) g_hash_table_remove_all(s->index[i]);
  for (int p = 0; p < 2; p++) for (size_t i = 0; i < nostr_nip51_document_tag_count(codec, p); i++) {
    const NostrListTag *tag = nostr_nip51_document_tag(codec, p, i);
    for (guint k = 0; k < 4; k++) if (!g_strcmp0(tag_names[k], nostr_nip51_tag_field(tag, 0))) {
      char *key = normalized(k, nostr_nip51_tag_field(tag, 1)); if (!key || !*key) { g_free(key); continue; }
      gpointer stable = g_hash_table_lookup(s->interned, key);
      if (!stable) { stable = key; g_hash_table_add(s->interned, key); } else g_free(key);
      g_hash_table_insert(s->index[k], stable, GINT_TO_POINTER(p + 1));
    }
  }
}
static GPtrArray *replacement_operations(const NostrListDocument *from, const NostrListDocument *to) {
  GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  for (int p = 0; p < 2; p++) {
    for (size_t i = 0; i < nostr_nip51_document_tag_count(from, p); i++)
      g_ptr_array_add(ops, gnostr_list_operation_remove(p, nostr_nip51_document_tag(from, p, i), G_MAXUINT));
    for (size_t i = 0; i < nostr_nip51_document_tag_count(to, p); i++)
      g_ptr_array_add(ops, gnostr_list_operation_append(p, nostr_nip51_document_tag(to, p, i)));
  }
  return ops;
}
static gboolean replace_service(GNostrListService *service, const NostrListDocument *to) {
  GNostrListDocument *current = gnostr_list_service_lookup(service, NOSTR_KIND_MUTE_LIST, "");
  if (!current) return FALSE;
  GPtrArray *ops = replacement_operations(current->codec, to);
  GNostrListDocument *saved = gnostr_list_service_apply(service, NOSTR_KIND_MUTE_LIST, "", current->revision, ops, NULL);
  gboolean ok = saved != NULL; g_clear_object(&saved); g_object_unref(current); g_ptr_array_unref(ops); return ok;
}
static void on_document(GNostrListService *service, GNostrListDocument *doc, GNostrMuteList *s) {
  g_mutex_lock(&s->lock);
  if (!s->closed && s->service == service && !s->pending && !s->fetch) {
    rebuild_locked(s, doc->codec); s->dirty = doc->dirty; s->last_time = MAX(s->last_time, doc->created_at);
  }
  g_mutex_unlock(&s->lock);
}
static gboolean create_service(GNostrMuteList *s, const char *account, const char *const *relays) {
  GNostrListService *next = list_gnostr_service_new(account, s->generation);
  GNostrListReadPlanEntry plan = {.kind = NOSTR_KIND_MUTE_LIST, .identifier = "", .sources = relays, .targets = relays,
    .read_purpose = "mute-list-read", .publish_purpose = "mute-list-publish",
    .publish_policy = GNOSTR_LIST_EXPLICIT_ONLY, .required_sources = GNOSTR_LIST_SOURCES_ALL};
  if (!next || !gnostr_list_service_start(next, &plan, 1, NULL)) {
    if (next) { gnostr_list_service_close(next); g_object_unref(next); } return FALSE;
  }
  g_mutex_lock(&s->lock); GNostrListService *old = s->service; s->service = next; g_mutex_unlock(&s->lock);
  if (old) { g_signal_handlers_disconnect_by_data(old, s); gnostr_list_service_close(old); g_object_unref(old); }
  g_signal_connect(next, "document-changed", G_CALLBACK(on_document), s); return TRUE;
}
GNostrMuteList *gnostr_mute_list_get_default(void) {
  g_mutex_lock(&singleton_lock);
  if (!singleton) {
    GNostrMuteList *s = g_new0(GNostrMuteList, 1); g_atomic_ref_count_init(&s->refs); g_mutex_init(&s->lock);
    s->context = g_main_context_ref_thread_default(); s->thread = g_thread_self(); s->generation = 1;
    s->projection = nostr_nip51_document_new(NOSTR_KIND_MUTE_LIST, NULL);
    s->interned = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < 4; i++) s->index[i] = g_hash_table_new(g_str_hash, g_str_equal);
    singleton = s;
    create_service(s, local_account, NULL);
  }
  GNostrMuteList *result = singleton; g_mutex_unlock(&singleton_lock); return result;
}
static gboolean close_dispatch(gpointer data);
static void fetch_check(GNostrMuteList *s);
void gnostr_mute_list_shutdown(void) {
  g_mutex_lock(&singleton_lock); GNostrMuteList *s = singleton; singleton = NULL;
  if (s) {
    g_mutex_lock(&s->lock); s->closed = TRUE; s->generation++; g_mutex_unlock(&s->lock);
    if (g_thread_self() == s->thread) close_dispatch(s);
    else post(s, close_dispatch, facade_ref(s), facade_unref);
    facade_unref(s);
  }
  g_mutex_unlock(&singleton_lock);
}
typedef struct { GNostrMuteList *s; guint64 generation; GPtrArray *ops; NostrListDocument *replace; gint64 imported_time; } Mutation;
static void mutation_free(gpointer data) {
  Mutation *m = data; g_clear_pointer(&m->ops, g_ptr_array_unref); nostr_nip51_document_free(m->replace); facade_unref(m->s); g_free(m);
}
static gboolean mutation_dispatch(gpointer data) {
  Mutation *m = data; GNostrMuteList *s = m->s;
  g_mutex_lock(&s->lock); gboolean active = !s->closed && s->generation == m->generation;
  GNostrListService *service = active ? g_object_ref(s->service) : NULL; g_mutex_unlock(&s->lock);
  if (!service) return G_SOURCE_REMOVE;
  gboolean ok;
  if (m->replace) ok = replace_service(service, m->replace);
  else {
    GNostrListDocument *doc = gnostr_list_service_lookup(service, NOSTR_KIND_MUTE_LIST, "");
    GNostrListDocument *saved = doc ? gnostr_list_service_apply(service, NOSTR_KIND_MUTE_LIST, "", doc->revision, m->ops, NULL) : NULL;
    ok = saved != NULL; g_clear_object(&saved); g_clear_object(&doc);
  }
  g_mutex_lock(&s->lock); if (s->pending) s->pending--; g_mutex_unlock(&s->lock);
  GNostrListDocument *doc = gnostr_list_service_lookup(service, NOSTR_KIND_MUTE_LIST, "");
  if (doc) { on_document(service, doc, s); g_object_unref(doc); }
  if (m->replace && ok) { g_mutex_lock(&s->lock); if (!s->pending) s->dirty = FALSE; s->last_time = m->imported_time; g_mutex_unlock(&s->lock); }
  fetch_check(s);
  if (!ok) g_warning("Mute list local edit could not be committed");
  g_object_unref(service); return G_SOURCE_REMOVE;
}
static void queue_mutation(GNostrMuteList *s, Mutation *m) {
  m->s = facade_ref(s); m->generation = s->generation; s->pending++;
  /* Attach while locked so worker mutations cannot reorder the service edits. */
  post(s, mutation_dispatch, m, mutation_free);
}
gboolean gnostr_mute_list_load_from_json(GNostrMuteList *s, const char *json) {
  if (!s || !json || strlen(json) > NOSTR_LIST_MAX_EVENT_BYTES) return FALSE;
  NostrEvent *ev = nostr_event_new();
  gboolean ok = nostr_event_deserialize_compact(ev, json, NULL) == 1 && nostr_event_get_kind(ev) == NOSTR_KIND_MUTE_LIST;
  NostrListDocument *codec = ok ? nostr_nip51_document_parse_event(ev) : NULL;
  ok = codec && !nostr_nip51_document_validate(codec) &&
    nostr_nip51_document_private_state(codec) == NOSTR_LIST_PRIVATE_EMPTY;
  if (ok) {
    Mutation *m = g_new0(Mutation, 1); m->replace = codec; m->imported_time = nostr_event_get_created_at(ev); codec = NULL;
    g_mutex_lock(&s->lock);
    if (!s->closed) { rebuild_locked(s, m->replace); s->dirty = FALSE; s->last_time = nostr_event_get_created_at(ev); queue_mutation(s, m); }
    else { nostr_nip51_document_free(m->replace); g_free(m); ok = FALSE; }
    g_mutex_unlock(&s->lock);
  }
  nostr_nip51_document_free(codec); nostr_event_free(ev); return ok;
}
static void mutate(GNostrMuteList *s, guint k, const char *value, gboolean private_tag, gboolean remove) {
  if (!s) return;
  char *key = normalized(k, value); if (!key || !*key) { g_free(key); return; }
  const char *fields[] = {tag_names[k], key}; NostrListTag *tag = nostr_nip51_tag_new(fields, 2);
  if (!tag || nostr_nip51_validate_item(nostr_nip51_schema_lookup(NOSTR_KIND_MUTE_LIST), tag) == NOSTR_LIST_ITEM_INVALID) { nostr_nip51_tag_free(tag); g_free(key); return; }
  g_mutex_lock(&s->lock);
  if (s->closed || (!remove && g_hash_table_contains(s->index[k], key)) || (remove && !g_hash_table_contains(s->index[k], key))) goto done;
  Mutation *m = g_new0(Mutation, 1); m->ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  if (!remove) g_ptr_array_add(m->ops, gnostr_list_operation_append(private_tag, tag));
  else for (int p = 0; p < 2; p++) for (size_t i = 0; i < nostr_nip51_document_tag_count(s->projection, p); i++) {
    const NostrListTag *t = nostr_nip51_document_tag(s->projection, p, i);
    char *candidate = normalized(k, nostr_nip51_tag_field(t, 1));
    if (!g_strcmp0(nostr_nip51_tag_field(t, 0), tag_names[k]) && !g_strcmp0(candidate, key))
      g_ptr_array_add(m->ops, gnostr_list_operation_remove(p, t, G_MAXUINT));
    g_free(candidate);
  }
  NostrListDocument *next = list_codec_copy(s->projection);
  gboolean ok = next != NULL;
  for (guint i = 0; ok && i < m->ops->len; i++) ok = list_operation_apply(next, m->ops->pdata[i], NULL);
  if (ok) { rebuild_locked(s, next); s->dirty = TRUE; queue_mutation(s, m); }
  else { g_ptr_array_unref(m->ops); g_free(m); }
  nostr_nip51_document_free(next);
done:
  g_mutex_unlock(&s->lock); nostr_nip51_tag_free(tag); g_free(key);
}
#define MUTATORS(name, k) \
void gnostr_mute_list_add_##name(GNostrMuteList *s, const char *v, gboolean p) { mutate(s, k, v, p, FALSE); } \
void gnostr_mute_list_remove_##name(GNostrMuteList *s, const char *v) { mutate(s, k, v, FALSE, TRUE); }
MUTATORS(pubkey, 0)
MUTATORS(word, 1)
MUTATORS(hashtag, 2)
MUTATORS(event, 3)
static gboolean query(GNostrMuteList *s, guint k, const char *v) {
  if (!s) return FALSE; char *key = normalized(k, v); if (!key) return FALSE;
  g_mutex_lock(&s->lock); gboolean result = !s->closed && g_hash_table_contains(s->index[k], key); g_mutex_unlock(&s->lock); g_free(key); return result;
}
gboolean gnostr_mute_list_is_pubkey_muted(GNostrMuteList *s, const char *v) { return query(s, 0, v); }
gboolean gnostr_mute_list_is_hashtag_muted(GNostrMuteList *s, const char *v) { return query(s, 2, v); }
gboolean gnostr_mute_list_is_event_muted(GNostrMuteList *s, const char *v) { return query(s, 3, v); }
gboolean gnostr_mute_list_contains_muted_word(GNostrMuteList *s, const char *content) {
  if (!s || !content || !g_utf8_validate(content, -1, NULL)) return FALSE;
  char *lower = g_utf8_strdown(content, -1); gboolean result = FALSE;
  g_mutex_lock(&s->lock); GHashTableIter it; gpointer word; g_hash_table_iter_init(&it, s->index[1]);
  while (!s->closed && g_hash_table_iter_next(&it, &word, NULL)) if (strstr(lower, word)) { result = TRUE; break; }
  g_mutex_unlock(&s->lock); g_free(lower); return result;
}
gboolean gnostr_mute_list_should_hide_event(GNostrMuteList *s, const char *json) {
  if (!s || !json) return FALSE;
  json_t *j = json_loads(json, JSON_REJECT_DUPLICATES, NULL);
  char *author = normalized(0, json_string_value(json_object_get(j, "pubkey")));
  char *id = normalized(3, json_string_value(json_object_get(j, "id")));
  char *content = normalized(1, json_string_value(json_object_get(j, "content")));
  g_mutex_lock(&s->lock);
  gboolean hide = !s->closed && ((author && g_hash_table_contains(s->index[0], author)) ||
    (id && g_hash_table_contains(s->index[3], id)));
  GHashTableIter it; gpointer word; g_hash_table_iter_init(&it, s->index[1]);
  while (!s->closed && !hide && content && g_hash_table_iter_next(&it, &word, NULL)) hide = strstr(content, word) != NULL;
  json_t *tags = json_object_get(j, "tags");
  for (size_t i = 0; !s->closed && !hide && i < json_array_size(tags); i++) {
    json_t *t = json_array_get(tags, i);
    if (!g_strcmp0(json_string_value(json_array_get(t, 0)), "t")) {
      char *tag = normalized(2, json_string_value(json_array_get(t, 1)));
      hide = tag && g_hash_table_contains(s->index[2], tag); g_free(tag);
    }
  }
  g_mutex_unlock(&s->lock); g_free(author); g_free(id); g_free(content);
  json_decref(j); return hide;
}
static const char **values(GNostrMuteList *s, guint k, size_t *count) {
  if (count) *count = 0; if (!s) return NULL;
  g_mutex_lock(&s->lock); guint n = s->closed ? 0 : g_hash_table_size(s->index[k]);
  const char **v = g_new0(const char *, n + 1); GHashTableIter it; gpointer key; guint i = 0;
  g_hash_table_iter_init(&it, s->index[k]); while (i < n && g_hash_table_iter_next(&it, &key, NULL)) v[i++] = key;
  if (count) *count = n; g_mutex_unlock(&s->lock); return v;
}
const char **gnostr_mute_list_get_pubkeys(GNostrMuteList *s, size_t *n) { return values(s, 0, n); }
const char **gnostr_mute_list_get_words(GNostrMuteList *s, size_t *n) { return values(s, 1, n); }
const char **gnostr_mute_list_get_hashtags(GNostrMuteList *s, size_t *n) { return values(s, 2, n); }
const char **gnostr_mute_list_get_events(GNostrMuteList *s, size_t *n) { return values(s, 3, n); }
gboolean gnostr_mute_list_is_dirty(GNostrMuteList *s) { if (!s) return FALSE; g_mutex_lock(&s->lock); gboolean dirty = s->dirty; g_mutex_unlock(&s->lock); return dirty; }
gint64 gnostr_mute_list_get_last_event_time(GNostrMuteList *s) { if (!s) return 0; g_mutex_lock(&s->lock); gint64 at = s->last_time; g_mutex_unlock(&s->lock); return at; }
GNostrListService *gnostr_mute_list_get_service(GNostrMuteList *s) {
  if (!s) return NULL; g_mutex_lock(&s->lock); GNostrListService *service = s->service ? g_object_ref(s->service) : NULL; g_mutex_unlock(&s->lock); return service;
}

typedef struct {
  GNostrMuteList *s;
  char *account;
  char **relays;
  GNostrMuteListMergeStrategy strategy;
  GNostrMuteListFetchCallback callback;
  gpointer user_data;
  NostrListDocument *local;
  gint64 local_time;
  guint64 generation;
  gulong handler, cancel_handler;
  GCancellable *cancel;
} Fetch;
static void fetch_complete(Fetch *f, gboolean success) {
  GNostrMuteList *s = f->s;
  if (f->handler) g_signal_handler_disconnect(s->service, f->handler);
  if (f->cancel_handler) g_cancellable_disconnect(f->cancel, f->cancel_handler);
  g_clear_object(&f->cancel);
  g_mutex_lock(&s->lock); if (s->fetch == f) s->fetch = NULL; g_mutex_unlock(&s->lock);
  if (f->callback) f->callback(s, success, f->user_data);
  nostr_nip51_document_free(f->local); g_free(f->account); g_strfreev(f->relays); facade_unref(s); g_free(f);
}
static gboolean tag_present(const NostrListDocument *d, int p, const NostrListTag *t) {
  for (size_t i = 0; i < nostr_nip51_document_tag_count(d, p); i++) {
    const NostrListTag *other = nostr_nip51_document_tag(d, p, i);
    if (nostr_nip51_tag_size(t) != nostr_nip51_tag_size(other)) continue;
    gboolean equal = TRUE;
    for (size_t j = 0; j < nostr_nip51_tag_size(t); j++) equal &= !g_strcmp0(nostr_nip51_tag_field(t, j), nostr_nip51_tag_field(other, j));
    if (equal) return TRUE;
  }
  return FALSE;
}
static void fetch_state(GNostrListService *service, Fetch *f) {
  GNostrMuteList *s = f->s;
  if (s->closed || f->generation != s->generation) { fetch_complete(f, FALSE); return; }
  g_mutex_lock(&s->lock); gboolean pending = s->pending > 0; g_mutex_unlock(&s->lock); if (pending) return;
  GNostrListDocument *doc = gnostr_list_service_lookup(service, NOSTR_KIND_MUTE_LIST, ""); if (!doc) return;
  json_t *sources = json_loads(doc->sources, 0, NULL); guint eose = 0, terminal = 0;
  for (guint i = 0; f->relays && f->relays[i]; i++) {
    int state = json_integer_value(json_object_get(sources, f->relays[i])) - 1;
    if (state == GNOSTR_LIST_EOSE) eose++;
    if (state == GNOSTR_LIST_EOSE || state == GNOSTR_LIST_CLOSED || state == GNOSTR_LIST_AUTH_REQUIRED || state == GNOSTR_LIST_ERROR || state == GNOSTR_LIST_DISCONNECTED) terminal++;
  }
  json_decref(sources);
  GNostrListState state = gnostr_list_service_get_state(service, NOSTR_KIND_MUTE_LIST, "");
  if (terminal != g_strv_length(f->relays) || state == GNOSTR_LIST_READING) { g_object_unref(doc); return; }
  gboolean success = eose > 0 && state != GNOSTR_LIST_UNREADABLE && doc->event_id != NULL;
  if (eose && !doc->event_id && state != GNOSTR_LIST_UNREADABLE) {
    g_mutex_lock(&s->lock); NostrListDocument *local = list_codec_copy(s->projection); g_mutex_unlock(&s->lock);
    g_signal_handler_disconnect(service, f->handler); f->handler = 0;
    replace_service(service, local); nostr_nip51_document_free(local);
  }
  if (success) {
    g_mutex_lock(&s->lock); nostr_nip51_document_free(f->local); f->local = list_codec_copy(s->projection); f->local_time = s->last_time; g_mutex_unlock(&s->lock);
    NostrListDocument *chosen = NULL;
    gboolean keep = (f->strategy == GNOSTR_MUTE_LIST_MERGE_LOCAL_WINS &&
      (f->local_time > 0 || nostr_nip51_document_tag_count(f->local, FALSE) || nostr_nip51_document_tag_count(f->local, TRUE))) ||
      (f->strategy == GNOSTR_MUTE_LIST_MERGE_LATEST && f->local_time >= doc->created_at);
    if (keep) chosen = list_codec_copy(f->local);
    else if (f->strategy == GNOSTR_MUTE_LIST_MERGE_UNION) {
      chosen = list_codec_copy(doc->codec);
      for (int p = 0; p < 2; p++) for (size_t i = 0; i < nostr_nip51_document_tag_count(f->local, p); i++) {
        const NostrListTag *t = nostr_nip51_document_tag(f->local, p, i);
        if (!tag_present(chosen, p, t) && nostr_nip51_document_append(chosen, p, t)) success = FALSE;
      }
    }
    /* Disconnect before applying compatibility edits: apply emits state-changed. */
    g_signal_handler_disconnect(service, f->handler); f->handler = 0;
    if (chosen && success) success = replace_service(service, chosen);
    nostr_nip51_document_free(chosen);
    g_object_unref(doc); doc = gnostr_list_service_lookup(service, NOSTR_KIND_MUTE_LIST, "");
    g_mutex_lock(&s->lock); if (success) { rebuild_locked(s, doc->codec); s->dirty = doc->dirty; s->last_time = keep ? f->local_time : doc->created_at; } g_mutex_unlock(&s->lock);
  }
  g_object_unref(doc); fetch_complete(f, success);
}
static void fetch_check(GNostrMuteList *s) { if (s->fetch && ((Fetch *)s->fetch)->handler) fetch_state(s->service, s->fetch); }
typedef struct { GNostrMuteList *s; guint64 generation; } FetchCancel;
static void fetch_cancel_free(gpointer data) { FetchCancel *c = data; facade_unref(c->s); g_free(c); }
static gboolean fetch_cancel_dispatch(gpointer data) {
  FetchCancel *c = data; GNostrMuteList *s = c->s; Fetch *f = s->fetch;
  if (!f || f->generation != c->generation) return G_SOURCE_REMOVE;
  if (f->handler) { g_signal_handler_disconnect(s->service, f->handler); f->handler = 0; }
  GNostrListReadPlanEntry plan = {.kind = NOSTR_KIND_MUTE_LIST, .identifier = "", .targets = (const char *const *)s->relays,
    .publish_policy = GNOSTR_LIST_EXPLICIT_ONLY, .required_sources = GNOSTR_LIST_SOURCES_ALL};
  gnostr_list_service_update_read_plan(s->service, &plan, 1, NULL);
  g_mutex_lock(&s->lock); s->generation++; s->pending = 0; NostrListDocument *local = list_codec_copy(s->projection); g_mutex_unlock(&s->lock);
  replace_service(s->service, local); nostr_nip51_document_free(local);
  fetch_complete(f, FALSE); return G_SOURCE_REMOVE;
}
static void fetch_cancelled(GCancellable *cancel, gpointer data) {
  (void)cancel; FetchCancel *c = data; FetchCancel *copy = g_new0(FetchCancel, 1);
  copy->s = facade_ref(c->s); copy->generation = c->generation;
  post(c->s, fetch_cancel_dispatch, copy, fetch_cancel_free);
}
static gboolean fetch_dispatch(gpointer data) {
  Fetch *f = data; GNostrMuteList *s = f->s;
  if (s->fetch) fetch_complete(s->fetch, FALSE);
  g_mutex_lock(&s->lock);
  if (s->closed || (f->cancel && g_cancellable_is_cancelled(f->cancel))) { g_mutex_unlock(&s->lock); fetch_complete(f, FALSE); return G_SOURCE_REMOVE; }
  f->local = list_codec_copy(s->projection); f->local_time = s->last_time;
  /* A different account never inherits a previous account's session edits. */
  if (s->account && g_strcmp0(s->account, f->account)) {
    nostr_nip51_document_free(f->local); f->local = nostr_nip51_document_new(NOSTR_KIND_MUTE_LIST, NULL); f->local_time = 0;
    rebuild_locked(s, f->local); s->dirty = FALSE; s->last_time = 0;
  }
  s->generation++; f->generation = s->generation; s->pending = 0; s->fetch = f;
  g_mutex_unlock(&s->lock);
  if (!create_service(s, f->account, (const char *const *)f->relays)) { fetch_complete(f, FALSE); return G_SOURCE_REMOVE; }
  g_mutex_lock(&s->lock); g_free(s->account); s->account = g_strdup(f->account); g_strfreev(s->relays); s->relays = g_strdupv(f->relays); g_mutex_unlock(&s->lock);
  f->handler = g_signal_connect(s->service, "state-changed", G_CALLBACK(fetch_state), f);
  if (f->cancel) {
    FetchCancel *c = g_new0(FetchCancel, 1); c->s = facade_ref(s); c->generation = f->generation;
    f->cancel_handler = g_cancellable_connect(f->cancel, G_CALLBACK(fetch_cancelled), c, fetch_cancel_free);
  }
  if (!f->relays || !f->relays[0]) fetch_complete(f, FALSE);
  return G_SOURCE_REMOVE;
}
static void fetch_start(GNostrMuteList *s, const char *account, const char *const *relays,
  GNostrMuteListMergeStrategy strategy, GCancellable *cancel, GNostrMuteListFetchCallback cb, gpointer data) {
  gboolean valid_account = account && strlen(account) == 64;
  for (guint i = 0; valid_account && i < 64; i++) valid_account = g_ascii_isxdigit(account[i]);
  if (!s || !valid_account || strategy < GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS || strategy > GNOSTR_MUTE_LIST_MERGE_LATEST) { if (cb) cb(s, FALSE, data); return; }
  Fetch *f = g_new0(Fetch, 1); f->s = facade_ref(s); f->account = g_ascii_strdown(account, -1); f->strategy = strategy; f->callback = cb; f->user_data = data; f->cancel = cancel ? g_object_ref(cancel) : NULL;
  if (relays) f->relays = g_strdupv((char **)relays);
  else { GPtrArray *v = g_ptr_array_new(); gnostr_load_relays_into(v); g_ptr_array_add(v, NULL); f->relays = (char **)g_ptr_array_free(v, FALSE); }
  gboolean valid_relays = g_strv_length(f->relays) <= 16;
  for (guint i = 0; valid_relays && f->relays[i]; i++) {
    valid_relays = *f->relays[i] && strlen(f->relays[i]) <= 4096;
    for (guint j = 0; valid_relays && j < i; j++) valid_relays = strcmp(f->relays[i], f->relays[j]) != 0;
  }
  if (!valid_relays) { fetch_complete(f, FALSE); return; }
  post(s, fetch_dispatch, f, NULL);
}
void gnostr_mute_list_fetch_with_strategy_async(GNostrMuteList *s, const char *account, const char *const *relays,
  GNostrMuteListMergeStrategy strategy, GNostrMuteListFetchCallback cb, gpointer data) { fetch_start(s, account, relays, strategy, NULL, cb, data); }
void gnostr_mute_list_fetch_async(GNostrMuteList *s, const char *a, const char *const *r, GNostrMuteListFetchCallback cb, gpointer data) { gnostr_mute_list_fetch_with_strategy_async(s, a, r, GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS, cb, data); }
static gboolean close_dispatch(gpointer data) {
  GNostrMuteList *s = data;
  if (s->fetch) fetch_complete(s->fetch, FALSE);
  if (s->service) { g_signal_handlers_disconnect_by_data(s->service, s); gnostr_list_service_close(s->service); }
  return G_SOURCE_REMOVE;
}
typedef struct { GNostrMuteList *s; GNostrMuteListSaveCallback cb; gpointer data; GTask *task; guint64 generation; } Save;
static void save_complete(Save *save, gboolean success, const char *message) {
  if (save->cb) save->cb(save->s, success, message, save->data);
  if (save->task) { if (success) g_task_return_boolean(save->task, TRUE); else g_task_return_new_error(save->task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", message ? message : "Mute list publication failed"); g_object_unref(save->task); }
  facade_unref(save->s); g_free(save);
}
static void saved(GObject *source, GAsyncResult *res, gpointer data) {
  Save *save = data; GError *error = NULL; GVariant *outcomes = gnostr_list_service_publish_finish(GNOSTR_LIST_SERVICE(source), res, &error);
  gboolean accepted = FALSE;
  if (outcomes) { GVariantIter it; const char *target; gint outcome; g_variant_iter_init(&it, outcomes); while (g_variant_iter_next(&it, "{&si}", &target, &outcome)) accepted |= outcome == GNOSTR_LIST_ACCEPTED || outcome == GNOSTR_LIST_DUPLICATE; }
  g_mutex_lock(&save->s->lock); gboolean current = !save->s->closed && save->generation == save->s->generation; g_mutex_unlock(&save->s->lock);
  save_complete(save, current && accepted, error ? "Mute list publication failed" : (accepted ? NULL : "No relay accepted the mute list"));
  g_clear_error(&error); g_clear_pointer(&outcomes, g_variant_unref);
}
static gboolean save_dispatch(gpointer data) {
  Save *save = data; GNostrMuteList *s = save->s;
  g_mutex_lock(&s->lock); gboolean active = !s->closed && s->account && s->relays && s->relays[0]; save->generation = s->generation; g_mutex_unlock(&s->lock);
  if (!active) { save_complete(save, FALSE, "Fetch and bind an account with relay destinations before saving"); return G_SOURCE_REMOVE; }
  gnostr_list_service_publish_async(s->service, NOSTR_KIND_MUTE_LIST, "",
    GNOSTR_LIST_APPROVE_PRIVATE | GNOSTR_LIST_APPROVE_PUBLIC | GNOSTR_LIST_APPROVE_PARTIAL,
    save->task ? g_task_get_cancellable(save->task) : NULL, saved, save);
  return G_SOURCE_REMOVE;
}
void gnostr_mute_list_save_async(GNostrMuteList *s, GNostrMuteListSaveCallback cb, gpointer data) {
  if (!s) { if (cb) cb(s, FALSE, "Invalid mute list", data); return; }
  Save *save = g_new0(Save, 1); save->s = facade_ref(s); save->cb = cb; save->data = data; post(s, save_dispatch, save, NULL);
}
static void fetched_task(GNostrMuteList *s, gboolean ok, gpointer data) { (void)s; GTask *task = data; if (ok) g_task_return_boolean(task, TRUE); else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Mute list discovery incomplete"); g_object_unref(task); }
void gnostr_mute_list_fetch_gtask_async(GNostrMuteList *s, const char *account, const char *const *relays, GCancellable *cancel, GAsyncReadyCallback cb, gpointer data) {
  GTask *task = g_task_new(NULL, cancel, cb, data); fetch_start(s, account, relays, GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS, cancel, fetched_task, task);
}
gboolean gnostr_mute_list_fetch_gtask_finish(GNostrMuteList *s, GAsyncResult *result, GError **error) { (void)s; return g_task_propagate_boolean(G_TASK(result), error); }
void gnostr_mute_list_save_gtask_async(GNostrMuteList *s, GCancellable *cancel, GAsyncReadyCallback cb, gpointer data) {
  GTask *task = g_task_new(NULL, cancel, cb, data);
  if (!s) { g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid mute list"); g_object_unref(task); return; }
  Save *save = g_new0(Save, 1); save->s = facade_ref(s); save->task = task; post(s, save_dispatch, save, NULL);
}
gboolean gnostr_mute_list_save_gtask_finish(GNostrMuteList *s, GAsyncResult *result, GError **error) { (void)s; return g_task_propagate_boolean(G_TASK(result), error); }
