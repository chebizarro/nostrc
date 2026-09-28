#include "gh-inbox-lookup.h"

#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>

#define KIND_DM_INBOX 10050 /* NIP-17 */
#define MAX_SOURCES 16      /* the relay scope's own bound */
#define MAX_INBOX_RELAYS 16 /* the relay publish's own bound */
#define MAX_EVENTS 64       /* candidate lists examined per lookup */
#define FUTURE_SKEW_SECONDS (15 * 60)
#define DEFAULT_DEADLINE_SECONDS 15
#define MAX_DEADLINE_SECONDS 120
#define CACHE_TTL_USEC (5 * 60 * G_USEC_PER_SEC)
#define CACHE_MAX 128

enum { SOURCE_PENDING, SOURCE_EOSE, SOURCE_FAILED };

typedef struct {
  guint64 generation;
  gint64 stored_at; /* monotonic */
  GhInboxResult *result;
} CacheEntry;

struct _GhInboxLookup {
  GObject parent_instance;
  GhAccountController *accounts; /* NULL once disposed */
  GSettings *settings;
  GhRelayTransport transport;
  gpointer transport_data;
  gboolean custom_transport;
  guint deadline_seconds;
  GHashTable *cache;   /* recipient hex -> CacheEntry */
  GPtrArray *inflight; /* borrowed Lookup, each pinned by its task */
};

static void gh_inbox_lookup_resolver_init(GhInboxResolverInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(GhInboxLookup, gh_inbox_lookup, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GH_TYPE_INBOX_RESOLVER,
                                                    gh_inbox_lookup_resolver_init))

typedef struct {
  GhInboxLookup *owner; /* the task's source object */
  GTask *task;          /* NULL once returned */
  GMainContext *context;
  guint64 generation;
  gchar *recipient;
  GSource *caller_cancel;
  GSource *generation_cancel;
  GSource *deadline;
  GSource *completion;
  GhRelayScope *scope;
  GHashTable *sources; /* url -> SOURCE_* */
  guint events;
  gchar *best_id; /* NULL until a valid list is admitted */
  gint64 best_created_at;
  GStrv best_relays;
  gboolean best_truncated;
} Lookup;

static void
cache_entry_free(gpointer data)
{
  CacheEntry *entry = data;
  gh_inbox_result_free(entry->result);
  g_free(entry);
}

static gboolean
hex64(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isxdigit(*p))
      return FALSE;
  return TRUE;
}

static void
destroy_source(GSource **source)
{
  if (!*source)
    return;
  g_source_destroy(*source);
  g_clear_pointer(source, g_source_unref);
}

static void
lookup_free(gpointer data)
{
  Lookup *lookup = data;
  g_warn_if_fail(lookup->task == NULL && lookup->scope == NULL);
  g_main_context_unref(lookup->context);
  g_free(lookup->recipient);
  if (lookup->sources)
    g_hash_table_unref(lookup->sources);
  g_free(lookup->best_id);
  g_strfreev(lookup->best_relays);
  g_free(lookup);
}

/* Releases every hook the lookup holds; idempotent. */
static void
lookup_detach(Lookup *lookup)
{
  destroy_source(&lookup->caller_cancel);
  destroy_source(&lookup->generation_cancel);
  destroy_source(&lookup->deadline);
  destroy_source(&lookup->completion);
  if (lookup->scope) {
    gh_relay_scope_cancel(lookup->scope);
    g_clear_pointer(&lookup->scope, gh_relay_scope_unref);
  }
  g_ptr_array_remove_fast(lookup->owner->inflight, lookup);
}

static gboolean
lookup_current(Lookup *lookup)
{
  GhInboxLookup *self = lookup->owner;
  GCancellable *cancellable = lookup->task ? g_task_get_cancellable(lookup->task) : NULL;
  return self->accounts &&
         gh_account_controller_is_current(self->accounts, lookup->generation) &&
         (!cancellable || !g_cancellable_is_cancelled(cancellable));
}

/* Returns the task exactly once; a stale lookup always reports cancellation. */
static void
lookup_return(Lookup *lookup, GhInboxResult *result)
{
  GTask *task = lookup->task;
  if (!task) {
    gh_inbox_result_free(result);
    return;
  }
  gboolean current = lookup_current(lookup); /* consults the task's cancellable */
  lookup->task = NULL;
  lookup_detach(lookup);
  if (result && current) {
    g_task_return_pointer(task, result, (GDestroyNotify)gh_inbox_result_free);
  } else {
    gh_inbox_result_free(result);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Inbox lookup cancelled or the account changed");
  }
  g_object_unref(task);
}

static gboolean
on_cancelled(GCancellable *cancellable, gpointer data)
{
  (void)cancellable;
  lookup_return(data, NULL);
  return G_SOURCE_REMOVE;
}

static GSource *
watch_cancellable(Lookup *lookup, GCancellable *cancellable)
{
  GSource *source = g_cancellable_source_new(cancellable);
  g_source_set_callback(source, G_SOURCE_FUNC(on_cancelled), lookup, NULL);
  g_source_attach(source, lookup->context);
  return source;
}

static GhInboxResult *
build_result(Lookup *lookup)
{
  GhInboxResult *result = g_new0(GhInboxResult, 1);
  result->recipient = g_strdup(lookup->recipient);
  if (lookup->sources) {
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, lookup->sources);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      result->sources++;
      if (GPOINTER_TO_UINT(value) == SOURCE_EOSE)
        result->answered++;
      else
        result->failed++;
    }
  }
  if (lookup->best_id) {
    result->event_id = g_strdup(lookup->best_id);
    result->created_at = lookup->best_created_at;
    result->truncated = lookup->best_truncated;
    if (lookup->best_relays && lookup->best_relays[0]) {
      result->status = GH_INBOX_FOUND;
      result->relays = g_strdupv(lookup->best_relays);
    } else {
      result->status = GH_INBOX_EMPTY;
    }
  } else if (result->sources == 0) {
    result->status = GH_INBOX_NO_SOURCES;
  } else {
    result->status = result->answered ? GH_INBOX_NOT_FOUND
                                      : GH_INBOX_UNREACHABLE;
  }
  return result;
}

static void
cache_store(GhInboxLookup *self, guint64 generation, const GhInboxResult *result)
{
  if (result->status != GH_INBOX_FOUND)
    return; /* absence is re-checked every time: the user may publish one */
  if (g_hash_table_size(self->cache) >= CACHE_MAX)
    g_hash_table_remove_all(self->cache);
  CacheEntry *entry = g_new0(CacheEntry, 1);
  entry->generation = generation;
  entry->stored_at = g_get_monotonic_time();
  entry->result = gh_inbox_result_copy(result);
  g_hash_table_replace(self->cache, g_strdup(result->recipient), entry);
}

static gboolean
complete_now(gpointer data)
{
  Lookup *lookup = data;
  g_clear_pointer(&lookup->completion, g_source_unref);
  if (!lookup->task)
    return G_SOURCE_REMOVE;
  GhInboxResult *result = build_result(lookup);
  if (lookup_current(lookup))
    cache_store(lookup->owner, lookup->generation, result);
  lookup_return(lookup, result);
  return G_SOURCE_REMOVE;
}

/* Completion is deferred to the owning context so the scope is never torn
 * down from inside one of its own transport callbacks, and runs at low
 * priority: the gnostr transport dispatches EOSE at default priority but
 * stored EVENTs from a default-idle queue, so an EOSE can overtake events that
 * were received before it. Those drain first; a cross-thread window remains
 * until the transport orders them (nostrc-qp24.10.6). */
static void
schedule_completion(Lookup *lookup)
{
  if (lookup->completion || !lookup->task)
    return;
  lookup->completion = g_idle_source_new();
  g_source_set_priority(lookup->completion, G_PRIORITY_LOW);
  g_source_set_callback(lookup->completion, complete_now, lookup, NULL);
  g_source_attach(lookup->completion, lookup->context);
}

static gboolean
all_settled(Lookup *lookup)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, lookup->sources);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (GPOINTER_TO_UINT(value) == SOURCE_PENDING)
      return FALSE;
  return TRUE;
}

/* Usable relay tags of an admitted list: ws(s), deduplicated, first 16. */
static GStrv
inbox_relays(NostrEvent *event, gboolean *truncated)
{
  GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
  NostrTags *tags = nostr_event_get_tags(event);
  gsize count = tags ? nostr_tags_size(tags) : 0;
  *truncated = FALSE;
  for (gsize i = 0; i < count; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    const gchar *url = tag && nostr_tag_size(tag) >= 2 ? nostr_tag_get(tag, 1) : NULL;
    if (!tag || g_strcmp0(nostr_tag_get_key(tag), "relay") != 0 ||
        !gh_relay_url_validate(url, NULL) ||
        g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL))
      continue;
    if (urls->len >= MAX_INBOX_RELAYS) {
      *truncated = TRUE;
      break;
    }
    g_ptr_array_add(urls, g_strdup(url));
  }
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(urls, FALSE);
}

static void
admit_event(Lookup *lookup, const GhRelayUpdate *update)
{
  if (++lookup->events > MAX_EVENTS)
    return;
  NostrEvent *event = nostr_event_new();
  if (!event)
    return;
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  if (nostr_event_deserialize_signed(event, update->event_json, NULL) !=
        NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_get_kind(event) != KIND_DM_INBOX ||
      g_ascii_strcasecmp(nostr_event_get_pubkey(event) ? nostr_event_get_pubkey(event) : "",
                         lookup->recipient) != 0 ||
      nostr_event_get_created_at(event) > now + FUTURE_SKEW_SECONDS) {
    nostr_event_free(event);
    return; /* someone else's list, another kind, or a pinned future date */
  }
  gint64 created_at = nostr_event_get_created_at(event);
  if (lookup->best_id &&
      (created_at < lookup->best_created_at ||
       (created_at == lookup->best_created_at &&
        g_strcmp0(update->event_id, lookup->best_id) >= 0))) {
    nostr_event_free(event);
    return;
  }
  gboolean truncated = FALSE;
  GStrv relays = inbox_relays(event, &truncated);
  nostr_event_free(event);
  g_free(lookup->best_id);
  g_strfreev(lookup->best_relays);
  lookup->best_id = g_strdup(update->event_id);
  lookup->best_created_at = created_at;
  lookup->best_relays = relays;
  lookup->best_truncated = truncated;
}

static void
on_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  Lookup *lookup = data;
  if (scope != lookup->scope || !lookup->task || lookup->completion ||
      !lookup_current(lookup))
    return;
  guint status = GPOINTER_TO_UINT(g_hash_table_lookup(lookup->sources, update->url));
  if (!g_hash_table_contains(lookup->sources, update->url))
    return;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    admit_event(lookup, update);
    return;
  case GH_RELAY_NOTICE_EOSE:
    if (status == SOURCE_PENDING)
      status = SOURCE_EOSE;
    break;
  case GH_RELAY_NOTICE_ERROR:
  case GH_RELAY_NOTICE_CLOSED:
    /* A connect failure, or the relay ended the REQ (e.g. auth-required:)
     * before answering. The lookup does not wait for a redial. */
    if (status == SOURCE_PENDING)
      status = SOURCE_FAILED;
    break;
  default:
    return;
  }
  g_hash_table_insert(lookup->sources, g_strdup(update->url), GUINT_TO_POINTER(status));
  if (all_settled(lookup))
    schedule_completion(lookup);
}

static gboolean
deadline_expired(gpointer data)
{
  Lookup *lookup = data;
  g_clear_pointer(&lookup->deadline, g_source_unref);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, lookup->sources);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (GPOINTER_TO_UINT(value) == SOURCE_PENDING)
      g_hash_table_iter_replace(&iter, GUINT_TO_POINTER(SOURCE_FAILED));
  schedule_completion(lookup);
  return G_SOURCE_REMOVE;
}

static void
add_source(GPtrArray *urls, const gchar *url)
{
  if (urls->len < MAX_SOURCES && gh_relay_url_validate(url, NULL) &&
      !g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL))
    g_ptr_array_add(urls, g_strdup(url));
}

static void
add_sources(GPtrArray *urls, const gchar *const *list)
{
  for (guint i = 0; list && list[i]; i++)
    add_source(urls, list[i]);
}

static NostrFilters *
inbox_filters(const gchar *pubkey_hex)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  if (!filters || !filter) {
    if (filters)
      nostr_filters_free(filters);
    if (filter)
      nostr_filter_free(filter);
    return NULL;
  }
  const int kinds[] = { KIND_DM_INBOX };
  const char *const authors[] = { pubkey_hex };
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  nostr_filter_set_authors(filter, authors, G_N_ELEMENTS(authors));
  gboolean added = nostr_filters_add(filters, filter);
  nostr_filter_free(filter); /* contents moved into the vector */
  if (!added) {
    nostr_filters_free(filters);
    return NULL;
  }
  return filters;
}

/* Only discovery-relays: never the account's own relays, which would learn
 * whom it is about to message (charter §4.3). */
static void
start_req(Lookup *lookup)
{
  GhInboxLookup *self = lookup->owner;
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  g_auto(GStrv) discovery = g_settings_get_strv(self->settings, "discovery-relays");
  add_sources(urls, (const gchar *const *)discovery);
  lookup->sources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  NostrFilters *filters = urls->len ? inbox_filters(lookup->recipient) : NULL;
  if (!filters) {
    schedule_completion(lookup); /* NO_SOURCES */
    return;
  }
  lookup->scope = self->custom_transport
    ? gh_relay_scope_new_with_transport(lookup->generation, filters, &self->transport,
                                        self->transport_data, on_scope_update, lookup)
    : gh_relay_scope_new(lookup->generation, filters, on_scope_update, lookup);
  for (guint i = 0; i < urls->len; i++) {
    const gchar *url = g_ptr_array_index(urls, i);
    if (gh_relay_scope_add_url(lookup->scope, url, NULL))
      g_hash_table_insert(lookup->sources, g_strdup(url), GUINT_TO_POINTER(SOURCE_PENDING));
  }
  lookup->deadline = g_timeout_source_new_seconds(self->deadline_seconds);
  g_source_set_callback(lookup->deadline, deadline_expired, lookup, NULL);
  g_source_attach(lookup->deadline, lookup->context);
  gh_relay_scope_start(lookup->scope);
  if (lookup->scope && all_settled(lookup))
    schedule_completion(lookup);
}

static void
lookup_resolve_async(GhInboxResolver *resolver, const gchar *pubkey_hex,
                     GCancellable *cancellable, GAsyncReadyCallback callback,
                     gpointer user_data)
{
  GhInboxLookup *self = GH_INBOX_LOOKUP(resolver);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, lookup_resolve_async);
  Lookup *lookup = g_new0(Lookup, 1);
  lookup->owner = self;
  lookup->context = g_main_context_ref_thread_default();
  g_task_set_task_data(task, lookup, lookup_free);
  if (g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
    return;
  }
  if (!hex64(pubkey_hex)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A 64-character hex pubkey is required");
    g_object_unref(task);
    return;
  }
  GCancellable *generation_cancel =
    self->accounts ? gh_account_controller_get_cancellable(self->accounts) : NULL;
  if (!self->accounts ||
      gh_account_controller_get_state(self->accounts) != GH_ACCOUNT_STATE_ACTIVE ||
      !generation_cancel) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                            "No active Groundhog account");
    g_object_unref(task);
    return;
  }
  lookup->generation = gh_account_controller_get_generation(self->accounts);
  lookup->recipient = g_ascii_strdown(pubkey_hex, -1);

  CacheEntry *entry = g_hash_table_lookup(self->cache, lookup->recipient);
  if (entry && (entry->generation != lookup->generation ||
                g_get_monotonic_time() - entry->stored_at > CACHE_TTL_USEC)) {
    g_hash_table_remove(self->cache, lookup->recipient);
    entry = NULL;
  }
  if (entry) {
    GhInboxResult *result = gh_inbox_result_copy(entry->result);
    result->cached = TRUE;
    g_task_return_pointer(task, result, (GDestroyNotify)gh_inbox_result_free);
    g_object_unref(task);
    return;
  }

  lookup->task = task; /* owned until lookup_return */
  g_ptr_array_add(self->inflight, lookup);
  if (cancellable)
    lookup->caller_cancel = watch_cancellable(lookup, cancellable);
  lookup->generation_cancel = watch_cancellable(lookup, generation_cancel);
  start_req(lookup);
}

static GhInboxResult *
lookup_resolve_finish(GhInboxResolver *resolver, GAsyncResult *result, GError **error)
{
  GhInboxLookup *self = GH_INBOX_LOOKUP(resolver);
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == lookup_resolve_async,
                       NULL);
  GhInboxResult *value = g_task_propagate_pointer(G_TASK(result), error);
  Lookup *lookup = g_task_get_task_data(G_TASK(result));
  if (value && (!self->accounts ||
                !gh_account_controller_is_current(self->accounts, lookup->generation))) {
    gh_inbox_result_free(value);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "Inbox lookup cancelled or the account changed");
    return NULL;
  }
  return value;
}

void
gh_inbox_lookup_set_deadline(GhInboxLookup *self, guint seconds)
{
  g_return_if_fail(GH_IS_INBOX_LOOKUP(self));
  self->deadline_seconds = CLAMP(seconds, 1, MAX_DEADLINE_SECONDS);
}

static void
lookup_forget(GhInboxResolver *resolver, const gchar *pubkey_hex)
{
  GhInboxLookup *self = GH_INBOX_LOOKUP(resolver);
  if (!pubkey_hex)
    return;
  g_autofree gchar *key = g_ascii_strdown(pubkey_hex, -1);
  g_hash_table_remove(self->cache, key);
}

static void
on_accounts_changed(GhInboxLookup *self)
{
  /* Entries are generation-keyed anyway; drop them as soon as it moves. */
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->cache);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (!gh_account_controller_is_current(self->accounts,
                                          ((CacheEntry *)value)->generation))
      g_hash_table_iter_remove(&iter);
}

GhInboxLookup *
gh_inbox_lookup_new(GhAccountController *accounts, GSettings *settings,
                    const GhRelayTransport *transport, gpointer transport_data)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  g_return_val_if_fail(G_IS_SETTINGS(settings), NULL);
  g_return_val_if_fail(!transport || (transport->open && transport->close), NULL);
  GhInboxLookup *self = g_object_new(GH_TYPE_INBOX_LOOKUP, NULL);
  self->accounts = g_object_ref(accounts);
  self->settings = g_object_ref(settings);
  if (transport) {
    self->transport = *transport;
    self->transport_data = transport_data;
    self->custom_transport = TRUE;
  }
  g_signal_connect_object(accounts, "changed", G_CALLBACK(on_accounts_changed), self,
                          G_CONNECT_SWAPPED);
  return self;
}

static void
gh_inbox_lookup_dispose(GObject *object)
{
  GhInboxLookup *self = GH_INBOX_LOOKUP(object);
  /* Each in-flight task pins self, so this only runs via run_dispose. */
  while (self->inflight->len)
    lookup_return(g_ptr_array_index(self->inflight, 0), NULL);
  if (self->accounts)
    g_signal_handlers_disconnect_by_data(self->accounts, self);
  g_hash_table_remove_all(self->cache);
  g_clear_object(&self->accounts);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(gh_inbox_lookup_parent_class)->dispose(object);
}

static void
gh_inbox_lookup_finalize(GObject *object)
{
  GhInboxLookup *self = GH_INBOX_LOOKUP(object);
  g_hash_table_unref(self->cache);
  g_ptr_array_unref(self->inflight);
  G_OBJECT_CLASS(gh_inbox_lookup_parent_class)->finalize(object);
}

static void
gh_inbox_lookup_class_init(GhInboxLookupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_inbox_lookup_dispose;
  object_class->finalize = gh_inbox_lookup_finalize;
}

static void
gh_inbox_lookup_resolver_init(GhInboxResolverInterface *iface)
{
  iface->resolve_async = lookup_resolve_async;
  iface->resolve_finish = lookup_resolve_finish;
  iface->forget = lookup_forget;
}

static void
gh_inbox_lookup_init(GhInboxLookup *self)
{
  self->deadline_seconds = DEFAULT_DEADLINE_SECONDS;
  self->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cache_entry_free);
  self->inflight = g_ptr_array_new();
}
