#include "gh-relay-scope.h"

#include <gio/gio.h>
#include <nostr-event.h>
#include <string.h>

#define GH_MAX_RELAYS 16
#define GH_SEEN_LIMIT 4096

/* One filter's part of the current answer, while paging (nostrc-cpwf). */
typedef struct {
  guint count;                 /* events of the answer matching it */
  gint64 oldest;               /* their oldest created_at; G_MAXINT64: none */
  gint64 until;                /* the answer's until; G_MAXINT64: the live REQ */
  guint max_count;             /* most events one answer of this round brought: a
                                * lower bound on the relay's cap (review B2) */
  gboolean open;               /* older events may remain: page it */
} PageFilter;

typedef struct {
  GhRelayScope *scope;         /* owner; endpoints never outlive it */
  gchar *url;
  gpointer handle;
  gboolean eose;
  gboolean opened;
  GhRelayAuthMode auth_mode;   /* caller's identity choice; NONE default */
  /* NIP-42 state of the current connection. */
  gchar *challenge;            /* latest challenge */
  gchar *auth_challenge;       /* challenge an AUTH was attempted for */
  GhRelayAuthAttempt *attempt; /* signing in flight */
  gchar auth_event_id[65];     /* AUTH sent, its OK pending; "" otherwise */
  gchar *held_closed;          /* auth-required CLOSED held during AUTH */
  gboolean auth_needed;        /* the REQ awaits an authenticated retry */
  /* Overflow recovery (nostrc-5rfp), per connection. */
  guint overflow_retries;
  guint recover_source;        /* idle re-issuing the REQ */
  /* Backfill paging (nostrc-cpwf), per round (one live REQ's answer). */
  gboolean live_eose;          /* the live REQ's own EOSE arrived */
  PageFilter *pf;              /* per scope filter; NULL until needed */
  GhRelayScope *page;          /* the older page in flight */
  guint *page_map;             /* page filter index -> scope filter index */
  guint pages;                 /* older pages of this round */
  gboolean page_ended;         /* its EOSE or failure seen; judged by the idle */
  gboolean page_failed;
  gboolean incomplete;         /* older events of this round could not be fetched */
  guint settle_source;         /* idle judging an answer */
} GhEndpoint;

struct _GhRelayScope {
  gint refs;
  guint64 generation;
  NostrFilters *filters;
  GhRelayTransport transport;
  GhRelayAuthTransport auth_transport;
  gpointer transport_data;
  GhRelayScopeFunc callback;
  gpointer user_data;
  GHashTable *endpoints;
  GHashTable *seen;
  GQueue seen_order;
  GhRelayAuthSigner *signer;   /* account signer, for ACCOUNT URLs only */
  gchar *isolation;            /* Tor stream isolation label */
  gboolean started;
  gboolean cancelled;
  /* Backfill paging (nostrc-cpwf); page_limit 0: off. */
  guint page_limit;
  guint max_pages;
  gboolean *pageable;          /* per filter: no limit of its own */
};

/* gh_relay_scope_new()'s transport (gh_relay_scope_set_default_transport()). */
static GMutex default_lock;
static GhRelayTransport default_transport;
static GhRelayAuthTransport default_auth;
static gpointer default_data;
static gboolean default_set;

/* Forgets the connection's NIP-42 state and drops a pending AUTH; a held
 * CLOSED is superseded by whatever ended the connection. */
static void
reset_auth(GhEndpoint *endpoint)
{
  g_clear_pointer(&endpoint->attempt, gh_relay_auth_attempt_drop);
  g_clear_pointer(&endpoint->challenge, g_free);
  g_clear_pointer(&endpoint->auth_challenge, g_free);
  g_clear_pointer(&endpoint->held_closed, g_free);
  endpoint->auth_event_id[0] = '\0';
  endpoint->auth_needed = FALSE;
}

static void
stop_recovery(GhEndpoint *endpoint)
{
  if (endpoint->recover_source) {
    g_source_remove(endpoint->recover_source);
    endpoint->recover_source = 0;
  }
}

/* Ends this round's paging: the page in flight is cancelled, the answer
 * forgotten. The next live REQ's EOSE starts a new round. */
static void
reset_paging(GhEndpoint *endpoint)
{
  if (endpoint->settle_source) {
    g_source_remove(endpoint->settle_source);
    endpoint->settle_source = 0;
  }
  if (endpoint->page) {
    GhRelayScope *page = g_steal_pointer(&endpoint->page);
    gh_relay_scope_cancel(page);
    gh_relay_scope_unref(page);
  }
  g_clear_pointer(&endpoint->page_map, g_free);
  endpoint->live_eose = FALSE;
  endpoint->pages = 0;
  endpoint->page_ended = FALSE;
  endpoint->page_failed = FALSE;
  endpoint->incomplete = FALSE;
  if (endpoint->scope->pageable) {
    for (size_t i = 0; endpoint->pf && i < endpoint->scope->filters->count; i++) {
      endpoint->pf[i].count = 0;
      endpoint->pf[i].oldest = G_MAXINT64;
      endpoint->pf[i].until = G_MAXINT64;
      endpoint->pf[i].max_count = 0;
      endpoint->pf[i].open = FALSE;
    }
  }
}

static void
endpoint_free(gpointer data)
{
  GhEndpoint *endpoint = data;
  stop_recovery(endpoint);
  reset_paging(endpoint);
  reset_auth(endpoint);
  g_free(endpoint->pf);
  g_free(endpoint->url);
  g_free(endpoint);
}

static void
emit_update(GhRelayScope *scope, const GhRelayUpdate *update)
{
  if (!scope->cancelled && scope->callback) {
    gh_relay_scope_ref(scope);
    scope->callback(scope, update, scope->user_data);
    gh_relay_scope_unref(scope);
  }
}

GhRelayScope *
gh_relay_scope_new_with_transport(guint64 generation, NostrFilters *filters,
                                  const GhRelayTransport *transport,
                                  gpointer transport_data, GhRelayScopeFunc callback,
                                  gpointer user_data)
{
  g_return_val_if_fail(filters != NULL, NULL);
  g_return_val_if_fail(transport && transport->open && transport->close, NULL);
  GhRelayScope *scope = g_new0(GhRelayScope, 1);
  scope->refs = 1;
  scope->generation = generation;
  scope->filters = filters;
  scope->transport = *transport;
  scope->transport_data = transport_data;
  scope->callback = callback;
  scope->user_data = user_data;
  scope->endpoints = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                            endpoint_free);
  scope->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_queue_init(&scope->seen_order);
  scope->isolation = g_uuid_string_random();
  return scope;
}

void
gh_relay_scope_set_default_transport(const GhRelayTransport *transport,
                                     const GhRelayAuthTransport *auth,
                                     gpointer transport_data)
{
  g_return_if_fail(!transport || (transport->open && transport->close));
  g_return_if_fail(!auth || (auth->send_auth && auth->resubscribe));
  g_mutex_lock(&default_lock);
  default_set = transport != NULL;
  memset(&default_transport, 0, sizeof default_transport);
  memset(&default_auth, 0, sizeof default_auth);
  default_data = NULL;
  if (transport) {
    default_transport = *transport;
    if (auth)
      default_auth = *auth;
    default_data = transport_data;
  }
  g_mutex_unlock(&default_lock);
}

GhRelayScope *
gh_relay_scope_new(guint64 generation, NostrFilters *filters,
                   GhRelayScopeFunc callback, gpointer user_data)
{
  GhRelayTransport transport = gh_relay_gnostr_transport;
  GhRelayAuthTransport auth = gh_relay_gnostr_auth_transport;
  gpointer data = NULL;
  g_mutex_lock(&default_lock);
  if (default_set) {
    transport = default_transport;
    auth = default_auth;
    data = default_data;
  }
  g_mutex_unlock(&default_lock);
  GhRelayScope *scope = gh_relay_scope_new_with_transport(generation, filters, &transport,
                                                          data, callback, user_data);
  if (scope && auth.send_auth)
    scope->auth_transport = auth;
  return scope;
}

void
gh_relay_scope_set_isolation(GhRelayScope *scope, const gchar *isolation)
{
  g_return_if_fail(scope != NULL && !scope->started);
  g_return_if_fail(isolation != NULL && *isolation);
  g_free(scope->isolation);
  scope->isolation = g_strdup(isolation);
}

const gchar *
gh_relay_scope_get_isolation(const GhRelayScope *scope)
{
  g_return_val_if_fail(scope != NULL, NULL);
  return scope->isolation;
}

void
gh_relay_scope_set_auth_transport(GhRelayScope *scope,
                                  const GhRelayAuthTransport *auth)
{
  g_return_if_fail(scope != NULL && !scope->started);
  g_return_if_fail(!auth || (auth->send_auth && auth->resubscribe));
  if (auth)
    scope->auth_transport = *auth;
  else
    memset(&scope->auth_transport, 0, sizeof scope->auth_transport);
}

GhRelayScope *
gh_relay_scope_ref(GhRelayScope *scope)
{
  g_return_val_if_fail(scope != NULL, NULL);
  g_atomic_int_inc(&scope->refs);
  return scope;
}

void
gh_relay_scope_cancel(GhRelayScope *scope)
{
  g_return_if_fail(scope != NULL);
  if (scope->cancelled)
    return;
  scope->cancelled = TRUE;
  scope->generation++;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, scope->endpoints);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    GhEndpoint *endpoint = value;
    stop_recovery(endpoint);
    reset_paging(endpoint);
    reset_auth(endpoint); /* the revoked generation signs nothing more */
    if (endpoint->opened) {
      endpoint->opened = FALSE;
      scope->transport.close(endpoint->handle, scope->transport_data);
      endpoint->handle = NULL;
    }
  }
}

void
gh_relay_scope_unref(GhRelayScope *scope)
{
  if (!scope || !g_atomic_int_dec_and_test(&scope->refs))
    return;
  gh_relay_scope_cancel(scope);
  gh_relay_auth_signer_unref(scope->signer);
  g_hash_table_unref(scope->endpoints);
  g_hash_table_unref(scope->seen);
  g_queue_clear_full(&scope->seen_order, g_free);
  nostr_filters_free(scope->filters);
  g_free(scope->pageable);
  g_free(scope->isolation);
  g_free(scope);
}

void
gh_relay_scope_set_backfill_paging(GhRelayScope *scope, guint limit, guint max_pages)
{
  g_return_if_fail(scope != NULL && !scope->started);
  g_return_if_fail(limit >= 1 && limit <= G_MAXINT && max_pages >= 1);
  scope->page_limit = limit;
  scope->max_pages = max_pages;
  g_free(scope->pageable);
  scope->pageable = g_new0(gboolean, scope->filters->count + 1);
  for (size_t i = 0; i < scope->filters->count; i++) {
    NostrFilter *filter = &scope->filters->filters[i];
    if (nostr_filter_get_limit_zero(filter) || nostr_filter_get_limit(filter) > 0)
      continue;   /* "only the newest N": never paged */
    scope->pageable[i] = TRUE;
    nostr_filter_set_limit(filter, (int)limit);
  }
}

guint64
gh_relay_scope_get_generation(const GhRelayScope *scope)
{
  return scope ? scope->generation : 0;
}

gboolean
gh_relay_scope_set_account_signer(GhRelayScope *scope, GhRelayAuthSigner *signer,
                                  GError **error)
{
  g_return_val_if_fail(scope != NULL, FALSE);
  if (scope->cancelled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "relay scope cancelled");
    return FALSE;
  }
  if (scope->started) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "set the AUTH signer before the scope starts");
    return FALSE;
  }
  if (signer && (gh_relay_auth_signer_is_revoked(signer) ||
                 gh_relay_auth_signer_get_generation(signer) != scope->generation)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "AUTH signer does not belong to this account generation");
    return FALSE;
  }
  if (!signer) {
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, scope->endpoints);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      if (((GhEndpoint *)value)->auth_mode == GH_RELAY_AUTH_ACCOUNT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                            "a URL still authenticates as the account");
        return FALSE;
      }
    }
  }
  if (signer)
    gh_relay_auth_signer_ref(signer);
  gh_relay_auth_signer_unref(scope->signer);
  scope->signer = signer;
  return TRUE;
}

static void
open_endpoint(GhRelayScope *scope, GhEndpoint *endpoint)
{
  g_autoptr(GError) error = NULL;
  gh_relay_scope_ref(scope);
  gpointer handle = scope->transport.open(scope, endpoint->url, scope->filters,
                                           scope->transport_data, &error);
  if (scope->cancelled) {
    if (handle)
      scope->transport.close(handle, scope->transport_data);
  } else if (handle) {
    endpoint->handle = handle;
    endpoint->opened = TRUE;
  } else {
    GhRelayUpdate update = { .notice = GH_RELAY_NOTICE_ERROR,
                             .url = endpoint->url,
                             .detail = error ? error->message : "relay open failed" };
    emit_update(scope, &update);
  }
  gh_relay_scope_unref(scope);
}

gboolean
gh_relay_url_validate(const gchar *url, GError **error)
{
  g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  if (!host || !*host ||
      (g_strcmp0(scheme, "ws") != 0 && g_strcmp0(scheme, "wss") != 0) ||
      g_uri_get_userinfo(uri) != NULL) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "relay URL must be ws(s) with a host and no credentials");
    return FALSE;
  }
  return TRUE;
}

gboolean
gh_relay_scope_add_url(GhRelayScope *scope, const gchar *url, GError **error)
{
  g_return_val_if_fail(scope != NULL, FALSE);
  if (scope->cancelled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "relay scope cancelled");
    return FALSE;
  }
  if (!gh_relay_url_validate(url, error))
    return FALSE;
  if (g_hash_table_contains(scope->endpoints, url))
    return TRUE;
  if (g_hash_table_size(scope->endpoints) >= GH_MAX_RELAYS) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        "relay scope exceeds 16 URLs");
    return FALSE;
  }
  GhEndpoint *endpoint = g_new0(GhEndpoint, 1);
  endpoint->scope = scope;
  endpoint->url = g_strdup(url);
  g_hash_table_insert(scope->endpoints, endpoint->url, endpoint);
  if (scope->started)
    open_endpoint(scope, endpoint);
  return TRUE;
}

void
gh_relay_scope_start(GhRelayScope *scope)
{
  g_return_if_fail(scope != NULL);
  if (scope->started || scope->cancelled)
    return;
  scope->started = TRUE;
  gh_relay_scope_ref(scope);
  GList *endpoints = g_hash_table_get_values(scope->endpoints);
  for (GList *item = endpoints; item && !scope->cancelled; item = item->next)
    open_endpoint(scope, item->data);
  g_list_free(endpoints);
  gh_relay_scope_unref(scope);
}

static GhEndpoint *
active_endpoint(GhRelayScope *scope, const gchar *url)
{
  if (!scope || scope->cancelled || !scope->started || !url)
    return NULL;
  GhEndpoint *endpoint = g_hash_table_lookup(scope->endpoints, url);
  return endpoint && endpoint->opened ? endpoint : NULL;
}

/* Counts @event into this round's answer for every pageable scope filter
 * that @filters (the scope's own, or a page's, through @map) matches. */
static void
ensure_page_filters(GhRelayScope *scope, GhEndpoint *endpoint)
{
  if (endpoint->pf)
    return;
  endpoint->pf = g_new0(PageFilter, scope->filters->count + 1);
  for (size_t i = 0; i < scope->filters->count; i++) {
    endpoint->pf[i].oldest = G_MAXINT64;
    endpoint->pf[i].until = G_MAXINT64;
  }
}

static void
count_answer(GhRelayScope *scope, GhEndpoint *endpoint, NostrEvent *event,
             NostrFilters *filters, const guint *map)
{
  ensure_page_filters(scope, endpoint);
  gint64 created_at = nostr_event_get_created_at(event);
  for (size_t j = 0; j < filters->count; j++) {
    size_t i = map ? map[j] : j;
    if (!scope->pageable[i] || !nostr_filter_matches(&filters->filters[j], event))
      continue;
    endpoint->pf[i].count++;
    endpoint->pf[i].oldest = MIN(endpoint->pf[i].oldest, created_at);
  }
}

/* A verified event of @url's answer: once per id, to the caller. */
static void
deliver_event(GhRelayScope *scope, GhEndpoint *endpoint, const gchar *event_json,
              const gchar *id)
{
  if (g_hash_table_contains(scope->seen, id))
    return;
  g_hash_table_add(scope->seen, g_strdup(id));
  g_queue_push_tail(&scope->seen_order, g_strdup(id));
  if (g_queue_get_length(&scope->seen_order) > GH_SEEN_LIMIT) {
    gchar *oldest = g_queue_pop_head(&scope->seen_order);
    g_hash_table_remove(scope->seen, oldest);
    g_free(oldest);
  }
  GhRelayUpdate update = { .notice = GH_RELAY_NOTICE_EVENT, .url = endpoint->url,
                           .event_json = event_json, .event_id = id,
                           .backfill = !endpoint->eose };
  emit_update(scope, &update);
}

void
gh_relay_scope_event(GhRelayScope *scope, const gchar *url,
                     const gchar *event_json)
{
  GhEndpoint *endpoint = active_endpoint(scope, url);
  if (!endpoint || !event_json)
    return;
  NostrEvent *event = nostr_event_new();
  if (!event)
    return;
  char id[65];
  gboolean valid = nostr_event_deserialize_signed(event, event_json, NULL) ==
                     NOSTR_EVENT_VALIDATION_OK &&
                   nostr_event_validate(event, id) == NOSTR_EVENT_VALIDATION_OK;
  /* The live REQ's stored answer, counted before deduplication: another
   * relay's copy does not make this relay's answer shorter. */
  if (valid && scope->pageable && !endpoint->live_eose)
    count_answer(scope, endpoint, event, scope->filters, NULL);
  nostr_event_free(event);
  if (valid)
    deliver_event(scope, endpoint, event_json, id);
}

static void
finish_backfill(GhRelayScope *scope, GhEndpoint *endpoint)
{
  endpoint->eose = TRUE;
  if (endpoint->incomplete)
    g_debug("relay scope: %s's backfill is incomplete after %u older pages",
            endpoint->url, endpoint->pages);
  GhRelayUpdate update = { .notice = GH_RELAY_NOTICE_EOSE, .url = endpoint->url,
                           .incomplete = endpoint->incomplete };
  emit_update(scope, &update);
}

static gboolean settle_now(gpointer data);

/* Answers are judged from an idle: never inside a transport callback, where
 * a page's own connection cannot be torn down. */
static void
schedule_settle(GhEndpoint *endpoint)
{
  if (!endpoint->settle_source)
    endpoint->settle_source = g_idle_add(settle_now, endpoint);
}

/* The answer for scope filter @i has ended: may older events remain? */
static void
judge_filter(GhRelayScope *scope, GhEndpoint *endpoint, size_t i)
{
  PageFilter *p = &endpoint->pf[i];
  p->open = FALSE;
  /* Perhaps cut short by the relay: at least the threshold, and no fewer
   * than any earlier answer of this round brought (review B2). The relay
   * returned max_count events at once before, so its cap is at least that:
   * a page with fewer holds everything the relay has there. Without this,
   * 20 or more events in the window's oldest second looked like a cut page
   * forever (the step page returns them again, and nothing older exists). */
  gboolean cut = p->count >= gh_relay_page_threshold(scope->page_limit) &&
                 p->count >= p->max_count;
  p->max_count = MAX(p->max_count, p->count);
  if (cut) {
    /* until is inclusive, so the next page repeats the boundary second (the
     * relay may have cut inside it); a page that got no older holds more
     * than a page in that one second: step over it, and the answer is
     * incomplete. */
    gint64 next = p->oldest;
    if (next >= p->until) {
      next = p->until - 1;
      endpoint->incomplete = TRUE;
    }
    gint64 since = nostr_filter_get_since_i64(&scope->filters->filters[i]);
    if (next >= 0 && next >= since) {
      p->until = next;
      p->open = TRUE;
    }
  }
  p->count = 0;
  p->oldest = G_MAXINT64;
}

static void on_page_update(GhRelayScope *page, const GhRelayUpdate *update, gpointer data);

/* One older page for every open filter, on a fresh connection to the same
 * URL with the same AUTH identity and Tor isolation. */
static gboolean
start_page(GhRelayScope *scope, GhEndpoint *endpoint)
{
  NostrFilters *filters = nostr_filters_new();
  if (!filters)
    return FALSE;
  g_clear_pointer(&endpoint->page_map, g_free);
  endpoint->page_map = g_new0(guint, scope->filters->count + 1);
  guint n = 0;
  for (size_t i = 0; i < scope->filters->count; i++) {
    if (!scope->pageable[i] || !endpoint->pf[i].open)
      continue;
    NostrFilter *filter = nostr_filter_copy(&scope->filters->filters[i]);
    if (!filter) {
      nostr_filters_free(filters);
      return FALSE;
    }
    nostr_filter_set_until_i64(filter, endpoint->pf[i].until);
    nostr_filter_set_limit(filter, (int)scope->page_limit);
    gboolean added = nostr_filters_add(filters, filter);
    nostr_filter_free(filter); /* contents moved into the vector */
    if (!added) {
      nostr_filters_free(filters);
      return FALSE;
    }
    endpoint->page_map[n++] = (guint)i;
  }
  GhRelayScope *page = gh_relay_scope_new_with_transport(scope->generation, filters,
                                                         &scope->transport,
                                                         scope->transport_data,
                                                         on_page_update, endpoint);
  page->auth_transport = scope->auth_transport;
  g_free(page->isolation);
  page->isolation = g_strdup(scope->isolation);
  if (scope->signer)
    page->signer = gh_relay_auth_signer_ref(scope->signer);
  if (!gh_relay_scope_add_url(page, endpoint->url, NULL) ||
      !gh_relay_scope_set_url_auth(page, endpoint->url, endpoint->auth_mode, NULL)) {
    gh_relay_scope_unref(page);
    return FALSE;
  }
  endpoint->page = page;
  endpoint->page_ended = FALSE;
  endpoint->page_failed = FALSE;
  endpoint->pages++;
  gh_relay_scope_start(page);
  return TRUE;
}

static gboolean
settle_now(gpointer data)
{
  GhEndpoint *endpoint = data;
  GhRelayScope *scope = endpoint->scope;
  endpoint->settle_source = 0;
  if (scope->cancelled || !endpoint->opened || endpoint->eose || !endpoint->live_eose)
    return G_SOURCE_REMOVE;
  gh_relay_scope_ref(scope);
  if (endpoint->page) {
    if (!endpoint->page_ended)
      goto out;
    GhRelayScope *page = g_steal_pointer(&endpoint->page);
    gh_relay_scope_cancel(page);
    for (size_t j = 0; !endpoint->page_failed && j < page->filters->count; j++)
      judge_filter(scope, endpoint, endpoint->page_map[j]);
    gh_relay_scope_unref(page);
    if (endpoint->page_failed) {
      endpoint->incomplete = TRUE;
      finish_backfill(scope, endpoint);
      goto out;
    }
  } else {
    ensure_page_filters(scope, endpoint);   /* the live answer may have been empty */
    for (size_t i = 0; i < scope->filters->count; i++)
      if (scope->pageable[i])
        judge_filter(scope, endpoint, i);
  }
  gboolean open = FALSE;
  for (size_t i = 0; i < scope->filters->count; i++)
    open |= scope->pageable[i] && endpoint->pf[i].open;
  if (!open) {
    finish_backfill(scope, endpoint);
  } else if (endpoint->pages >= scope->max_pages || !start_page(scope, endpoint)) {
    endpoint->incomplete = TRUE;   /* the page budget ran out, or no page could start */
    finish_backfill(scope, endpoint);
  }
out:
  gh_relay_scope_unref(scope);
  return G_SOURCE_REMOVE;
}

/* The older page's answer, as the URL's own backfill. */
static void
on_page_update(GhRelayScope *page, const GhRelayUpdate *update, gpointer data)
{
  GhEndpoint *endpoint = data;
  GhRelayScope *scope = endpoint->scope;
  if (page != endpoint->page || endpoint->page_ended || scope->cancelled || !endpoint->opened)
    return;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT: {
    /* The page verified it; count it against the page's filters. */
    NostrEvent *event = nostr_event_new();
    if (event && nostr_event_deserialize_compact(event, update->event_json, NULL) == 1)
      count_answer(scope, endpoint, event, page->filters, endpoint->page_map);
    if (event)
      nostr_event_free(event);
    deliver_event(scope, endpoint, update->event_json, update->event_id);
    break;
  }
  case GH_RELAY_NOTICE_EOSE:
    endpoint->page_ended = TRUE;
    schedule_settle(endpoint);
    break;
  case GH_RELAY_NOTICE_CLOSED:
  case GH_RELAY_NOTICE_ERROR:
  case GH_RELAY_NOTICE_DISCONNECTED:
    g_debug("relay scope: an older page from %s failed: %s", endpoint->url,
            update->detail ? update->detail : "connection lost");
    endpoint->page_ended = TRUE;
    endpoint->page_failed = TRUE;
    schedule_settle(endpoint);
    break;
  case GH_RELAY_NOTICE_AUTH:
  case GH_RELAY_NOTICE_OK:
    break;   /* the page's own NIP-42; the live REQ reports AUTH */
  }
}

void
gh_relay_scope_eose(GhRelayScope *scope, const gchar *url)
{
  GhEndpoint *endpoint = active_endpoint(scope, url);
  if (!endpoint || endpoint->eose)
    return;
  if (scope->pageable) {
    /* Paging: the URL's EOSE waits until its backfill has been paged. */
    if (endpoint->live_eose)
      return;
    endpoint->live_eose = TRUE;
    schedule_settle(endpoint);
    return;
  }
  endpoint->eose = TRUE;
  GhRelayUpdate update = { .notice = GH_RELAY_NOTICE_EOSE, .url = url };
  emit_update(scope, &update);
}

static gboolean
auth_enabled(const GhRelayScope *scope, const GhEndpoint *endpoint)
{
  if (!scope->auth_transport.send_auth || !scope->auth_transport.resubscribe)
    return FALSE;
  switch (endpoint->auth_mode) {
  case GH_RELAY_AUTH_EPHEMERAL: return TRUE;
  case GH_RELAY_AUTH_ACCOUNT: return scope->signer != NULL;
  case GH_RELAY_AUTH_NONE: return FALSE;
  }
  return FALSE;
}

static gboolean
auth_in_flight(const GhEndpoint *endpoint)
{
  return endpoint->attempt || endpoint->auth_event_id[0];
}

/* The authenticated retry will not happen: report the held CLOSED. */
static void
fail_auth(GhRelayScope *scope, GhEndpoint *endpoint, const gchar *why)
{
  g_debug("relay scope: AUTH for %s not completed: %s", endpoint->url,
          why ? why : "unknown");
  g_clear_pointer(&endpoint->attempt, gh_relay_auth_attempt_drop);
  endpoint->auth_event_id[0] = '\0';
  endpoint->auth_needed = FALSE;
  g_autofree gchar *reason = g_steal_pointer(&endpoint->held_closed);
  if (reason) {
    GhRelayUpdate update = { .notice = GH_RELAY_NOTICE_CLOSED,
                             .url = endpoint->url, .detail = reason };
    emit_update(scope, &update);
  }
}

static void
on_auth_signed(gpointer owner, const gchar *signed_json, const gchar *event_id,
               const GError *error)
{
  GhEndpoint *endpoint = owner;
  GhRelayScope *scope = endpoint->scope;
  endpoint->attempt = NULL; /* the attempt released itself */
  if (scope->cancelled || !endpoint->opened)
    return;
  if (error) {
    fail_auth(scope, endpoint, error->message);
    return;
  }
  g_autoptr(GError) send_error = NULL;
  g_strlcpy(endpoint->auth_event_id, event_id, sizeof endpoint->auth_event_id);
  if (!scope->auth_transport.send_auth(endpoint->handle, signed_json,
                                       scope->transport_data, &send_error))
    fail_auth(scope, endpoint, send_error ? send_error->message : "AUTH not sent");
}

/* One AUTH per challenge, only once the relay has asked for it. */
static void
maybe_auth(GhRelayScope *scope, GhEndpoint *endpoint)
{
  if (!auth_enabled(scope, endpoint) || !endpoint->auth_needed ||
      !endpoint->challenge || auth_in_flight(endpoint))
    return;
  if (g_strcmp0(endpoint->auth_challenge, endpoint->challenge) == 0) {
    fail_auth(scope, endpoint, "AUTH already attempted for this challenge");
    return;
  }
  g_free(endpoint->auth_challenge);
  endpoint->auth_challenge = g_strdup(endpoint->challenge);
  g_autoptr(GError) error = NULL;
  endpoint->attempt = gh_relay_auth_attempt_start(endpoint->auth_mode, scope->signer,
                                                  scope->generation, endpoint->url,
                                                  endpoint->challenge, on_auth_signed,
                                                  endpoint, &error);
  if (!endpoint->attempt)
    fail_auth(scope, endpoint, error ? error->message : "AUTH not started");
}

/* TRUE when the notice was consumed by NIP-42 handling. */
static gboolean
auth_notice(GhRelayScope *scope, GhEndpoint *endpoint, GhRelayNotice notice,
            const gchar *event_id, gboolean accepted, const gchar *detail)
{
  switch (notice) {
  case GH_RELAY_NOTICE_OK:
    if (!endpoint->auth_event_id[0] ||
        g_strcmp0(event_id, endpoint->auth_event_id) != 0)
      return FALSE;
    endpoint->auth_event_id[0] = '\0';
    if (!accepted) {
      fail_auth(scope, endpoint, detail ? detail : "relay refused AUTH");
      return TRUE;
    }
    endpoint->auth_needed = FALSE;
    g_clear_pointer(&endpoint->held_closed, g_free);
    endpoint->eose = FALSE;
    reset_paging(endpoint);
    scope->auth_transport.resubscribe(endpoint->handle, scope->transport_data);
    return TRUE;
  case GH_RELAY_NOTICE_CLOSED:
    if (!auth_enabled(scope, endpoint) || !gh_relay_auth_is_required(detail))
      return FALSE;
    /* The retry after this challenge's AUTH was refused again: report. */
    if (endpoint->challenge &&
        g_strcmp0(endpoint->auth_challenge, endpoint->challenge) == 0 &&
        !auth_in_flight(endpoint))
      return FALSE;
    endpoint->auth_needed = TRUE;
    if (!endpoint->challenge)
      return FALSE; /* report now; a later challenge still retries once */
    g_free(endpoint->held_closed);
    endpoint->held_closed = g_strdup(detail);
    maybe_auth(scope, endpoint);
    return TRUE;
  case GH_RELAY_NOTICE_ERROR:
    if (auth_in_flight(endpoint))
      fail_auth(scope, endpoint, detail);
    return FALSE;
  case GH_RELAY_NOTICE_DISCONNECTED:
    reset_auth(endpoint);
    return FALSE;
  case GH_RELAY_NOTICE_EVENT:
  case GH_RELAY_NOTICE_EOSE:
  case GH_RELAY_NOTICE_AUTH:
    return FALSE;
  }
  return FALSE;
}

gboolean
gh_relay_closed_is_overflow(const gchar *detail)
{
  return detail && g_str_has_prefix(detail, GH_RELAY_CLOSED_OVERFLOW_PREFIX);
}

/* The overflow retry: a fresh REQ on the same connection. From an idle, never
 * from inside the transport's own CLOSED callback. */
static gboolean
recover_now(gpointer data)
{
  GhEndpoint *endpoint = data;
  GhRelayScope *scope = endpoint->scope;
  endpoint->recover_source = 0;
  if (!scope->cancelled && endpoint->opened && scope->auth_transport.resubscribe) {
    g_debug("relay scope: re-issuing the REQ to %s after an overflow", endpoint->url);
    endpoint->eose = FALSE;
    reset_paging(endpoint);
    scope->auth_transport.resubscribe(endpoint->handle, scope->transport_data);
  }
  return G_SOURCE_REMOVE;
}

static void
maybe_recover(GhRelayScope *scope, GhEndpoint *endpoint, const gchar *detail)
{
  if (scope->cancelled || !endpoint->opened || !gh_relay_closed_is_overflow(detail) ||
      !scope->auth_transport.resubscribe || endpoint->recover_source ||
      endpoint->overflow_retries >= GH_RELAY_SCOPE_OVERFLOW_RETRIES)
    return;
  endpoint->overflow_retries++;
  endpoint->recover_source = g_idle_add(recover_now, endpoint);
}

void
gh_relay_scope_notice(GhRelayScope *scope, const gchar *url,
                      GhRelayNotice notice, const gchar *event_id,
                      gboolean accepted, const gchar *detail)
{
  GhEndpoint *endpoint = active_endpoint(scope, url);
  if (!endpoint || notice == GH_RELAY_NOTICE_EVENT ||
      notice == GH_RELAY_NOTICE_EOSE)
    return;
  if (notice == GH_RELAY_NOTICE_DISCONNECTED) {
    endpoint->eose = FALSE;
    endpoint->overflow_retries = 0;   /* the next connection's REQ is new anyway */
    stop_recovery(endpoint);
    reset_paging(endpoint);           /* the next REQ's answer starts a new round */
  }
  gh_relay_scope_ref(scope);
  if (!auth_notice(scope, endpoint, notice, event_id, accepted, detail) &&
      !scope->cancelled) {
    GhRelayUpdate update = { .notice = notice, .url = url,
                             .event_id = event_id, .detail = detail,
                             .accepted = accepted };
    if (notice == GH_RELAY_NOTICE_CLOSED && !endpoint->eose)
      reset_paging(endpoint);         /* no EOSE follows for this REQ */
    emit_update(scope, &update);
    if (notice == GH_RELAY_NOTICE_CLOSED)
      maybe_recover(scope, endpoint, detail);
  }
  gh_relay_scope_unref(scope);
}

gboolean
gh_relay_scope_set_url_auth(GhRelayScope *scope, const gchar *url,
                            GhRelayAuthMode mode, GError **error)
{
  g_return_val_if_fail(scope != NULL, FALSE);
  if (scope->cancelled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                        "relay scope cancelled");
    return FALSE;
  }
  GhEndpoint *endpoint = url ? g_hash_table_lookup(scope->endpoints, url) : NULL;
  if (!endpoint) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "relay URL is not in this scope");
    return FALSE;
  }
  if (mode == GH_RELAY_AUTH_ACCOUNT && !scope->signer) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "account AUTH needs this generation's account signer");
    return FALSE;
  }
  if (endpoint->auth_mode == mode)
    return TRUE;
  endpoint->auth_mode = mode;
  /* An AUTH under the previous identity choice must not complete. */
  if (auth_in_flight(endpoint)) {
    gh_relay_scope_ref(scope);
    fail_auth(scope, endpoint, "AUTH identity changed");
    gh_relay_scope_unref(scope);
  }
  return TRUE;
}

void
gh_relay_scope_auth_challenge(GhRelayScope *scope, const gchar *url,
                              const gchar *challenge)
{
  GhEndpoint *endpoint = active_endpoint(scope, url);
  if (!endpoint || !challenge || !*challenge)
    return;
  g_free(endpoint->challenge);
  endpoint->challenge = g_strdup(challenge);
  gh_relay_scope_ref(scope);
  GhRelayUpdate update = { .notice = GH_RELAY_NOTICE_AUTH, .url = url,
                           .detail = "relay authentication requested" };
  emit_update(scope, &update);
  if (!scope->cancelled && endpoint->opened)
    maybe_auth(scope, endpoint);
  gh_relay_scope_unref(scope);
}
