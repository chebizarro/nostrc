#include <stdlib.h>
#include "gh-mls-key-packages.h"

#include "gh-auth-policy.h"
#include "gh-relay-scope.h"

#include <marmot/marmot.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-tag.h>
#include <nostr-utils.h>
#include <string.h>

#define DEFAULT_DEADLINE 15
#define MAX_SOURCES 16
/* Candidates kept per lookup (a relay could flood the answer). */
#define MAX_CANDIDATES 64
#define MAX_EVENT_JSON (64 * 1024)

typedef struct {
  GhAccountController *accounts;
  guint64 generation;
  gchar *pubkey;
  guint deadline;
  GHashTable *asked;      /* every URL either phase sent a REQ to */
  GHashTable *pending;    /* URLs of the current phase without EOSE or failure */
  GhRelayScope *scope;
  guint timer;
  guint phase;            /* 1: discovery relays, 2: the person's write relays */
  gboolean evidence;      /* a verification lookup: kind 443 too, every candidate back */
  GHashTable *excluded;   /* relay keys (gh_mls_relay_key()) never asked, both phases */
  GPtrArray *candidates;  /* kind-30443 (and, for evidence, 443) JSON */
  GHashTable *candidate_ids;
  gchar *relay_list;      /* the newest kind 10002 by the person */
  gint64 relay_list_at;
  gchar *relay_list_id;
  guint sources;
  guint answered;
  GCancellable *cancellable;
  gulong cancel_handler;
  gboolean done;
} Lookup;

void
gh_mls_key_package_free(GhMlsKeyPackage *key_package)
{
  if (!key_package)
    return;
  g_free(key_package->pubkey);
  g_free(key_package->event_json);
  g_free(key_package->event_id);
  g_free(key_package);
}

static void
lookup_stop(Lookup *lookup)
{
  if (lookup->timer) {
    g_source_remove(lookup->timer);
    lookup->timer = 0;
  }
  if (lookup->scope) {
    gh_relay_scope_cancel(lookup->scope);
    g_clear_pointer(&lookup->scope, gh_relay_scope_unref);
  }
}

static void
lookup_free(gpointer data)
{
  Lookup *lookup = data;
  lookup_stop(lookup);
  if (lookup->cancel_handler)
    g_cancellable_disconnect(lookup->cancellable, lookup->cancel_handler);
  g_clear_object(&lookup->cancellable);
  g_clear_object(&lookup->accounts);
  g_free(lookup->pubkey);
  g_hash_table_unref(lookup->asked);
  if (lookup->excluded)
    g_hash_table_unref(lookup->excluded);
  g_hash_table_unref(lookup->pending);
  g_ptr_array_unref(lookup->candidates);
  g_hash_table_unref(lookup->candidate_ids);
  g_free(lookup->relay_list);
  g_free(lookup->relay_list_id);
  g_free(lookup);
}

gchar *
gh_mls_relay_key(const gchar *url)
{
  g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  if (!uri || !g_uri_get_host(uri))
    return url ? g_ascii_strdown(url, -1) : NULL;
  g_autofree gchar *scheme = g_ascii_strdown(g_uri_get_scheme(uri), -1);
  g_autofree gchar *host = g_ascii_strdown(g_uri_get_host(uri), -1);
  gint port = g_uri_get_port(uri);
  if ((port == 443 && g_str_equal(scheme, "wss")) || (port == 80 && g_str_equal(scheme, "ws")))
    port = -1;
  g_autofree gchar *path = g_strdup(g_uri_get_path(uri));
  gsize n = strlen(path);
  while (n > 0 && path[n - 1] == '/')
    path[--n] = '\0';
  const gchar *query = g_uri_get_query(uri);
  g_autofree gchar *port_text = port > 0 ? g_strdup_printf(":%d", port) : g_strdup("");
  return g_strconcat(scheme, "://", host, port_text, path, query ? "?" : "", query ? query : "",
                     NULL);
}

static gboolean
excluded(Lookup *lookup, const gchar *url)
{
  if (!lookup->excluded || !url)
    return FALSE;
  g_autofree gchar *key = gh_mls_relay_key(url);
  return key && g_hash_table_contains(lookup->excluded, key);
}

static gboolean
lower_hex64(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isdigit(*p) && (*p < 'a' || *p > 'f'))
      return FALSE;
  return TRUE;
}

static gboolean
still_current(GTask *task)
{
  Lookup *lookup = g_task_get_task_data(task);
  GCancellable *cancellable = g_task_get_cancellable(task);
  return (!cancellable || !g_cancellable_is_cancelled(cancellable)) &&
         gh_account_controller_is_current(lookup->accounts, lookup->generation);
}

static void start_phase(GTask *task, const gchar *const *urls, gint kinds_mask);

/* No callback of this lookup runs any more (never called from the cancel
 * handler itself, where disconnecting would deadlock). */
static void
lookup_done(Lookup *lookup)
{
  lookup->done = TRUE;
  lookup_stop(lookup);
  if (lookup->cancel_handler) {
    g_cancellable_disconnect(lookup->cancellable, lookup->cancel_handler);
    lookup->cancel_handler = 0;
  }
}

static void
fail(GTask *task, GIOErrorEnum code, const gchar *message)
{
  Lookup *lookup = g_task_get_task_data(task);
  lookup_done(lookup);
  g_task_return_new_error(task, G_IO_ERROR, code, "%s", message);
  g_object_unref(task);
}

/* Everything collected: the libmarmot slot-aware choice. */
static void
finish(GTask *task)
{
  Lookup *lookup = g_task_get_task_data(task);
  lookup_stop(lookup);
  if (lookup->done)
    return;
  if (!still_current(task)) {
    fail(task, G_IO_ERROR_CANCELLED, "The KeyPackage lookup was cancelled");
    return;
  }
  if (lookup->evidence) {
    /* Nobody answered: no verdict either way (offline is not "not found"). */
    if (lookup->answered == 0 && lookup->candidates->len == 0) {
      fail(task, G_IO_ERROR_HOST_UNREACHABLE, "No relay answered the KeyPackage lookup");
      return;
    }
    GPtrArray *found = g_ptr_array_ref(lookup->candidates);
    lookup_done(lookup);
    g_task_return_pointer(task, found, (GDestroyNotify)g_ptr_array_unref);
    g_object_unref(task);
    return;
  }
  if (lookup->candidates->len == 0) {
    if (lookup->answered == 0)
      fail(task, G_IO_ERROR_HOST_UNREACHABLE, "No relay answered the KeyPackage lookup");
    else
      fail(task, G_IO_ERROR_NOT_FOUND, "This person hasn't set up encrypted groups");
    return;
  }
  guint8 owner[32];
  size_t index = 0;
  gboolean ok = nostr_hex2bin(owner, lookup->pubkey, sizeof owner) &&
                marmot_select_key_package_event((const char **)lookup->candidates->pdata,
                                                lookup->candidates->len, owner,
                                                &index) == MARMOT_OK &&
                index < lookup->candidates->len;
  if (!ok) {
    fail(task, G_IO_ERROR_NOT_FOUND, "This person has no valid KeyPackage");
    return;
  }
  GhMlsKeyPackage *result = g_new0(GhMlsKeyPackage, 1);
  result->pubkey = g_strdup(lookup->pubkey);
  result->event_json = g_strdup(g_ptr_array_index(lookup->candidates, index));
  NostrEvent *event = nostr_event_new();
  if (event && nostr_event_deserialize_compact(event, result->event_json, NULL) == 1)
    {
      char *raw_id = nostr_event_get_id(event); /* malloc'd; nostrc-kdxe */
      result->event_id = raw_id ? g_strdup(raw_id) : NULL;
      free(raw_id);
    }
  if (event)
    nostr_event_free(event);
  result->sources = lookup->sources;
  result->answered = lookup->answered;
  lookup_done(lookup);
  g_task_return_pointer(task, result, (GDestroyNotify)gh_mls_key_package_free);
  g_object_unref(task);
}

/* The person's kind-10002 write relays not asked yet (NIP-65: "write" or no
 * marker), valid ws(s) URLs only. */
static GStrv
write_relays(Lookup *lookup)
{
  g_autoptr(GStrvBuilder) out = g_strv_builder_new();
  if (!lookup->relay_list)
    return g_strv_builder_end(out);
  NostrEvent *event = nostr_event_new();
  if (!event || nostr_event_deserialize_compact(event, lookup->relay_list, NULL) != 1) {
    if (event)
      nostr_event_free(event);
    return g_strv_builder_end(out);
  }
  NostrTags *tags = nostr_event_get_tags(event);
  g_autoptr(GHashTable) added = g_hash_table_new(g_str_hash, g_str_equal);
  guint n = 0;
  for (size_t i = 0; tags && i < nostr_tags_size(tags) && n < GH_MLS_KEY_PACKAGE_MAX_WRITE_RELAYS;
       i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2 || g_strcmp0(nostr_tag_get(tag, 0), "r") != 0)
      continue;
    const gchar *url = nostr_tag_get(tag, 1);
    const gchar *marker = nostr_tag_size(tag) >= 3 ? nostr_tag_get(tag, 2) : NULL;
    if ((marker && *marker && g_strcmp0(marker, "write") != 0) ||
        !gh_relay_url_validate(url, NULL) || g_hash_table_contains(lookup->asked, url) ||
        g_hash_table_contains(added, url))
      continue;
    g_strv_builder_add(out, url);
    g_hash_table_add(added, (gpointer)url);
    n++;
  }
  GStrv result = g_strv_builder_end(out);
  nostr_event_free(event);
  return result;
}

static void
phase_done(GTask *task)
{
  Lookup *lookup = g_task_get_task_data(task);
  lookup_stop(lookup);
  if (lookup->phase == 1 && still_current(task)) {
    g_auto(GStrv) urls = write_relays(lookup);
    if (urls[0]) {
      start_phase(task, (const gchar *const *)urls, 2);
      return;
    }
  }
  finish(task);
}

static void
url_settled(GTask *task, const gchar *url, gboolean answered)
{
  Lookup *lookup = g_task_get_task_data(task);
  if (!url || !g_hash_table_remove(lookup->pending, url))
    return;
  if (answered)
    lookup->answered++;
  if (g_hash_table_size(lookup->pending) == 0)
    phase_done(task);
}

static void
keep_event(Lookup *lookup, const gchar *json, const gchar *id)
{
  if (!json || strnlen(json, MAX_EVENT_JSON + 1) > MAX_EVENT_JSON)
    return;
  NostrEvent *event = nostr_event_new();
  if (!event || nostr_event_deserialize_compact(event, json, NULL) != 1 ||
      g_strcmp0(nostr_event_get_pubkey(event), lookup->pubkey) != 0) {
    if (event)
      nostr_event_free(event);
    return;
  }
  gint kind = nostr_event_get_kind(event);
  gint64 created_at = nostr_event_get_created_at(event);
  nostr_event_free(event);
  if (kind == MARMOT_KIND_KEY_PACKAGE || (lookup->evidence && kind == GH_MLS_KIND_LEGACY_KEY_PACKAGE)) {
    if (lookup->candidates->len >= MAX_CANDIDATES || !id ||
        g_hash_table_contains(lookup->candidate_ids, id))
      return;
    g_hash_table_add(lookup->candidate_ids, g_strdup(id));
    g_ptr_array_add(lookup->candidates, g_strdup(json));
  } else if (kind == 10002 && lookup->phase == 1) {
    /* NIP-01: the newest replaceable event wins, then the lower id. */
    if (lookup->relay_list && (created_at < lookup->relay_list_at ||
                               (created_at == lookup->relay_list_at &&
                                g_strcmp0(id, lookup->relay_list_id) >= 0)))
      return;
    g_free(lookup->relay_list);
    g_free(lookup->relay_list_id);
    lookup->relay_list = g_strdup(json);
    lookup->relay_list_id = g_strdup(id);
    lookup->relay_list_at = created_at;
  }
}

static void
on_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GTask *task = data;
  Lookup *lookup = g_task_get_task_data(task);
  if (scope != lookup->scope)
    return;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    keep_event(lookup, update->event_json, update->event_id);
    break;
  case GH_RELAY_NOTICE_EOSE:
    url_settled(task, update->url, TRUE);
    break;
  case GH_RELAY_NOTICE_CLOSED:
  case GH_RELAY_NOTICE_DISCONNECTED:
  case GH_RELAY_NOTICE_ERROR:
    url_settled(task, update->url, FALSE);
    break;
  default:
    break;
  }
}

static gboolean
deadline_hit(gpointer data)
{
  GTask *task = data;
  Lookup *lookup = g_task_get_task_data(task);
  lookup->timer = 0;
  g_hash_table_remove_all(lookup->pending);
  phase_done(task);
  return G_SOURCE_REMOVE;
}

static void
start_phase(GTask *task, const gchar *const *urls, gint phase)
{
  Lookup *lookup = g_task_get_task_data(task);
  lookup->phase = (guint)phase;
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  int both[] = { 10002, MARMOT_KIND_KEY_PACKAGE };
  int only[] = { MARMOT_KIND_KEY_PACKAGE };
  /* Verification also reads the older kind 443 (never kind 10051: the
   * adopted spec dropped it). */
  int both_evidence[] = { 10002, MARMOT_KIND_KEY_PACKAGE, GH_MLS_KIND_LEGACY_KEY_PACKAGE };
  int only_evidence[] = { MARMOT_KIND_KEY_PACKAGE, GH_MLS_KIND_LEGACY_KEY_PACKAGE };
  if (phase == 1 && lookup->evidence)
    nostr_filter_set_kinds(filter, both_evidence, G_N_ELEMENTS(both_evidence));
  else if (phase == 1)
    nostr_filter_set_kinds(filter, both, G_N_ELEMENTS(both));
  else if (lookup->evidence)
    nostr_filter_set_kinds(filter, only_evidence, G_N_ELEMENTS(only_evidence));
  else
    nostr_filter_set_kinds(filter, only, G_N_ELEMENTS(only));
  const char *authors[] = { lookup->pubkey };
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter);
  GhRelayScope *scope = gh_relay_scope_new(lookup->generation, filters, on_update, task);
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(lookup->accounts);
  for (guint i = 0; urls[i]; i++) {
    g_autoptr(GError) error = NULL;
    if (g_hash_table_size(lookup->pending) >= MAX_SOURCES ||
        /* Never an excluded relay, in either phase (W24 review A1). */
        g_hash_table_contains(lookup->pending, urls[i]) || excluded(lookup, urls[i]) ||
        !gh_relay_scope_add_url(scope, urls[i], &error))
      continue;
    /* §4.3: a lookup relay learns whom you look up, never who you are. */
    if (!gh_auth_policy_apply_scope(policy, scope, GH_AUTH_PURPOSE_CONTACT_DIRECTORY, urls[i],
                                    &error))
      g_debug("Groundhog will not sign in to a KeyPackage source: %s", error->message);
    g_hash_table_add(lookup->pending, g_strdup(urls[i]));
    g_hash_table_add(lookup->asked, g_strdup(urls[i]));
    lookup->sources++;
  }
  if (g_hash_table_size(lookup->pending) == 0) {
    gh_relay_scope_unref(scope);
    if (phase == 1) {
      fail(task, G_IO_ERROR_INVALID_ARGUMENT, "No usable relay to look KeyPackages up on");
    } else {
      finish(task);
    }
    return;
  }
  lookup->scope = scope;
  lookup->timer = g_timeout_add_seconds(lookup->deadline, deadline_hit, task);
  gh_relay_scope_start(scope);
}

/* An account switch or the caller: close the sockets now, from an idle
 * (the handler may run inside g_cancellable_cancel()). */
static gboolean
cancel_idle(gpointer data)
{
  GTask *task = data;
  Lookup *lookup = g_task_get_task_data(task);
  if (!lookup->done)   /* the idle holds its own reference */
    fail(task, G_IO_ERROR_CANCELLED, "The KeyPackage lookup was cancelled");
  return G_SOURCE_REMOVE;
}

static void
on_cancelled(GCancellable *cancellable, gpointer data)
{
  (void)cancellable;
  g_idle_add_full(G_PRIORITY_DEFAULT, cancel_idle, g_object_ref(data), g_object_unref);
}

static void
lookup_start(GTask *task, GhAccountController *accounts, const gchar *const *discovery_relays,
             const gchar *const *exclude, const gchar *pubkey, guint deadline,
             gboolean evidence, GCancellable *cancellable)
{
  g_autofree gchar *lower = pubkey ? g_ascii_strdown(pubkey, -1) : NULL;
  if (!lower_hex64(lower)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "A 64-character hex public key is required");
    g_object_unref(task);
    return;
  }
  Lookup *lookup = g_new0(Lookup, 1);
  lookup->evidence = evidence;
  for (guint i = 0; exclude && exclude[i]; i++) {
    if (!lookup->excluded)
      lookup->excluded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gchar *key = gh_mls_relay_key(exclude[i]);
    if (key)
      g_hash_table_add(lookup->excluded, key);
  }
  lookup->accounts = g_object_ref(accounts);
  lookup->generation = gh_account_controller_get_generation(accounts);
  lookup->pubkey = g_steal_pointer(&lower);
  lookup->deadline = deadline ? MIN(deadline, 120) : DEFAULT_DEADLINE;
  lookup->asked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  lookup->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  lookup->candidates = g_ptr_array_new_with_free_func(g_free);
  lookup->candidate_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_task_set_task_data(task, lookup, lookup_free);
  if (!discovery_relays || !discovery_relays[0]) {
    fail(task, G_IO_ERROR_INVALID_ARGUMENT, "No discovery relay to look KeyPackages up on");
    return;
  }
  if (cancellable) {
    lookup->cancellable = g_object_ref(cancellable);
    lookup->cancel_handler = g_cancellable_connect(cancellable, G_CALLBACK(on_cancelled),
                                                   task, NULL);
  }
  start_phase(task, discovery_relays, 1);
}

void
gh_mls_key_package_lookup_async(GhAccountController *accounts,
                                const gchar *const *discovery_relays, const gchar *pubkey,
                                guint deadline, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_mls_key_package_lookup_async);
  lookup_start(task, accounts, discovery_relays, NULL, pubkey, deadline, FALSE, cancellable);
}

void
gh_mls_key_package_evidence_lookup_async(GhAccountController *accounts,
                                         const gchar *const *relays,
                                         const gchar *const *exclude, const gchar *pubkey,
                                         guint deadline, GCancellable *cancellable,
                                         GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts));
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_mls_key_package_evidence_lookup_async);
  lookup_start(task, accounts, relays, exclude, pubkey, deadline, TRUE, cancellable);
}

GPtrArray *
gh_mls_key_package_evidence_lookup_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

GhMlsKeyPackage *
gh_mls_key_package_lookup_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
