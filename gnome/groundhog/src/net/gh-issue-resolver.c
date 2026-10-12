/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-issue-resolver.h"
#include "gh-auth-policy.h"
#include "gh-relay-scope.h"

#include <nip34.h>
#include <nostr-filter.h>

#define MAX_RELAYS 16 /* the relay scope's own bound */
#define MAX_EVENTS 16
#define DEFAULT_DEADLINE_SECONDS 15
#define MAX_DEADLINE_SECONDS 120

enum { RELAY_PENDING, RELAY_EOSE, RELAY_FAILED };

struct _GhIssueResolver {
  GObject parent_instance;
  GhAccountController *accounts;
  GStrv relays;
  guint deadline_seconds;
};

static void resolver_iface_init(GnIssueRepoResolverInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(GhIssueResolver, gh_issue_resolver, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_ISSUE_REPO_RESOLVER, resolver_iface_init))

typedef struct {
  GTask *task; /* NULL once returned */
  GMainContext *context;
  guint64 generation;
  GnIssueTarget *hint;
  GhRelayScope *scope;
  GHashTable *relays; /* url -> RELAY_* */
  GPtrArray *events;  /* verified event JSON */
  GSource *caller_cancel;
  GSource *generation_cancel;
  GSource *deadline;
  GSource *completion;
} Lookup;

static void
destroy_source(GSource **source)
{
  if (!*source)
    return;
  g_source_destroy(*source);
  g_clear_pointer(source, g_source_unref);
}

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
}

static void
lookup_free(gpointer data)
{
  Lookup *lookup = data;
  lookup_detach(lookup);
  g_main_context_unref(lookup->context);
  gn_issue_target_free(lookup->hint);
  g_clear_pointer(&lookup->relays, g_hash_table_unref);
  g_ptr_array_unref(lookup->events);
  g_free(lookup);
}

static gboolean
lookup_current(Lookup *lookup)
{
  GhIssueResolver *self = g_task_get_source_object(lookup->task);
  GCancellable *cancellable = g_task_get_cancellable(lookup->task);
  return self->accounts && gh_account_controller_is_current(self->accounts, lookup->generation) &&
         (!cancellable || !g_cancellable_is_cancelled(cancellable));
}

/* Returns the task exactly once; a stale lookup reports cancellation. */
static void
lookup_return(Lookup *lookup)
{
  GTask *task = lookup->task;
  if (!task)
    return;
  gboolean current = lookup_current(lookup);
  lookup->task = NULL;
  lookup_detach(lookup);
  if (current) {
    GnIssueTarget *target = lookup->events->len
      ? gn_issue_target_from_announcements(lookup->hint, (const gchar *const *)lookup->events->pdata,
                                           (gssize)lookup->events->len)
      : NULL;
    g_task_return_pointer(task, target, (GDestroyNotify)gn_issue_target_free);
  } else {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                            "Repository lookup cancelled or the account changed");
  }
  g_object_unref(task);
}

static gboolean
complete_now(gpointer data)
{
  Lookup *lookup = data;
  g_clear_pointer(&lookup->completion, g_source_unref);
  lookup_return(lookup);
  return G_SOURCE_REMOVE;
}

/* Deferred to the owning context so that the scope is never torn down from
 * inside one of its own callbacks. */
static void
schedule_completion(Lookup *lookup)
{
  if (lookup->completion || !lookup->task)
    return;
  lookup->completion = g_idle_source_new();
  g_source_set_callback(lookup->completion, complete_now, lookup, NULL);
  g_source_attach(lookup->completion, lookup->context);
}

static gboolean
on_cancelled(GCancellable *cancellable, gpointer data)
{
  (void)cancellable;
  lookup_return(data);
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

static gboolean
all_settled(Lookup *lookup)
{
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, lookup->relays);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    if (GPOINTER_TO_UINT(value) == RELAY_PENDING)
      return FALSE;
  return TRUE;
}

static void
on_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  Lookup *lookup = data;
  if (scope != lookup->scope || !lookup->task || lookup->completion ||
      !g_hash_table_contains(lookup->relays, update->url))
    return;
  guint status = GPOINTER_TO_UINT(g_hash_table_lookup(lookup->relays, update->url));
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    /* The scope admitted only a validly signed event; the owner, kind and
     * repository id are checked by gn_issue_target_from_announcements(). */
    if (lookup->events->len < MAX_EVENTS)
      g_ptr_array_add(lookup->events, g_strdup(update->event_json));
    return;
  case GH_RELAY_NOTICE_EOSE:
    if (status == RELAY_PENDING)
      status = RELAY_EOSE;
    break;
  case GH_RELAY_NOTICE_ERROR:
  case GH_RELAY_NOTICE_CLOSED:
    if (status == RELAY_PENDING)
      status = RELAY_FAILED;
    break;
  default:
    return;
  }
  g_hash_table_insert(lookup->relays, g_strdup(update->url), GUINT_TO_POINTER(status));
  if (all_settled(lookup))
    schedule_completion(lookup);
}

static gboolean
deadline_expired(gpointer data)
{
  Lookup *lookup = data;
  g_clear_pointer(&lookup->deadline, g_source_unref);
  schedule_completion(lookup);
  return G_SOURCE_REMOVE;
}

static NostrFilters *
announcement_filters(const GnIssueTarget *hint)
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
  const int kinds[] = { NIP34_KIND_REPOSITORY };
  const char *const authors[] = { hint->owner_hex };
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  nostr_filter_set_authors(filter, authors, G_N_ELEMENTS(authors));
  nostr_filter_tags_append(filter, "d", hint->repo_id, NULL);
  nostr_filter_set_limit(filter, 5);
  gboolean added = nostr_filters_add(filters, filter);
  nostr_filter_free(filter); /* contents moved into the vector */
  if (!added) {
    nostr_filters_free(filters);
    return NULL;
  }
  return filters;
}

static void
resolve_async(GnIssueRepoResolver *resolver, const GnIssueTarget *hint,
              GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  GhIssueResolver *self = GH_ISSUE_RESOLVER(resolver);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, resolve_async);
  Lookup *lookup = g_new0(Lookup, 1);
  lookup->context = g_main_context_ref_thread_default();
  lookup->hint = gn_issue_target_copy(hint);
  lookup->events = g_ptr_array_new_with_free_func(g_free);
  lookup->relays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_task_set_task_data(task, lookup, lookup_free);
  if (g_task_return_error_if_cancelled(task)) {
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
  NostrFilters *filters = announcement_filters(hint);
  if (!filters) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The repository lookup could not be prepared");
    g_object_unref(task);
    return;
  }
  lookup->task = task; /* owned until lookup_return() */
  lookup->generation = gh_account_controller_get_generation(self->accounts);
  if (cancellable)
    lookup->caller_cancel = watch_cancellable(lookup, cancellable);
  lookup->generation_cancel = watch_cancellable(lookup, generation_cancel);
  /* The application transport: GhNet's network-mode dispatcher. */
  lookup->scope = gh_relay_scope_new(lookup->generation, filters, on_scope_update, lookup);
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->accounts);
  for (guint i = 0; self->relays[i]; i++) {
    const gchar *url = self->relays[i];
    if (!gh_relay_scope_add_url(lookup->scope, url, NULL))
      continue;
    /* A public lookup: an ephemeral key if the relay demands AUTH, never
     * the account. */
    gh_auth_policy_apply_scope(policy, lookup->scope, GH_AUTH_PURPOSE_CONTACT_DIRECTORY, url,
                               NULL);
    g_hash_table_insert(lookup->relays, g_strdup(url), GUINT_TO_POINTER(RELAY_PENDING));
  }
  lookup->deadline = g_timeout_source_new_seconds(self->deadline_seconds);
  g_source_set_callback(lookup->deadline, deadline_expired, lookup, NULL);
  g_source_attach(lookup->deadline, lookup->context);
  gh_relay_scope_start(lookup->scope);
  if (all_settled(lookup))
    schedule_completion(lookup);
}

static GnIssueTarget *
resolve_finish(GnIssueRepoResolver *resolver, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, resolver), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
resolver_iface_init(GnIssueRepoResolverInterface *iface)
{
  iface->resolve_async = resolve_async;
  iface->resolve_finish = resolve_finish;
}

static void
gh_issue_resolver_dispose(GObject *object)
{
  /* Each lookup's task holds a reference: none is in flight here. */
  g_clear_object(&GH_ISSUE_RESOLVER(object)->accounts);
  G_OBJECT_CLASS(gh_issue_resolver_parent_class)->dispose(object);
}

static void
gh_issue_resolver_finalize(GObject *object)
{
  g_strfreev(GH_ISSUE_RESOLVER(object)->relays);
  G_OBJECT_CLASS(gh_issue_resolver_parent_class)->finalize(object);
}

static void
gh_issue_resolver_class_init(GhIssueResolverClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_issue_resolver_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_issue_resolver_finalize;
}

static void
gh_issue_resolver_init(GhIssueResolver *self)
{
  self->deadline_seconds = DEFAULT_DEADLINE_SECONDS;
}

GnIssueRepoResolver *
gh_issue_resolver_new(GhAccountController *accounts, GSettings *settings)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  if (!settings)
    return NULL;
  g_auto(GStrv) discovery = g_settings_get_strv(settings, "discovery-relays");
  g_autoptr(GPtrArray) relays = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; discovery[i] && relays->len < MAX_RELAYS; i++) {
    g_autofree gchar *url = g_strstrip(g_strdup(discovery[i]));
    if (gh_relay_url_validate(url, NULL) &&
        !g_ptr_array_find_with_equal_func(relays, url, g_str_equal, NULL))
      g_ptr_array_add(relays, g_steal_pointer(&url));
  }
  if (!relays->len)
    return NULL;
  g_ptr_array_add(relays, NULL);
  GhIssueResolver *self = g_object_new(GH_TYPE_ISSUE_RESOLVER, NULL);
  self->accounts = g_object_ref(accounts);
  self->relays = (GStrv)g_ptr_array_free(g_steal_pointer(&relays), FALSE);
  return GN_ISSUE_REPO_RESOLVER(self);
}

const gchar *const *
gh_issue_resolver_get_relays(GhIssueResolver *self)
{
  g_return_val_if_fail(GH_IS_ISSUE_RESOLVER(self), NULL);
  return (const gchar *const *)self->relays;
}

void
gh_issue_resolver_set_deadline(GhIssueResolver *self, guint seconds)
{
  g_return_if_fail(GH_IS_ISSUE_RESOLVER(self));
  self->deadline_seconds = CLAMP(seconds, 1, MAX_DEADLINE_SECONDS);
}
