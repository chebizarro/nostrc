#include "gh-relay-scope.h"

#include <gio/gio.h>
#include <nostr-event.h>

#define GH_MAX_RELAYS 16
#define GH_SEEN_LIMIT 4096

typedef struct {
  gchar *url;
  gpointer handle;
  gboolean eose;
  gboolean opened;
} GhEndpoint;

struct _GhRelayScope {
  gint refs;
  guint64 generation;
  NostrFilters *filters;
  GhRelayTransport transport;
  gpointer transport_data;
  GhRelayScopeFunc callback;
  gpointer user_data;
  GHashTable *endpoints;
  GHashTable *seen;
  GQueue seen_order;
  gboolean started;
  gboolean cancelled;
};

extern const GhRelayTransport gh_relay_gnostr_transport;

static void
endpoint_free(gpointer data)
{
  GhEndpoint *endpoint = data;
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
  return scope;
}

GhRelayScope *
gh_relay_scope_new(guint64 generation, NostrFilters *filters,
                   GhRelayScopeFunc callback, gpointer user_data)
{
  return gh_relay_scope_new_with_transport(generation, filters,
                                            &gh_relay_gnostr_transport, NULL,
                                            callback, user_data);
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
  g_hash_table_unref(scope->endpoints);
  g_hash_table_unref(scope->seen);
  g_queue_clear_full(&scope->seen_order, g_free);
  nostr_filters_free(scope->filters);
  g_free(scope);
}

guint64
gh_relay_scope_get_generation(const GhRelayScope *scope)
{
  return scope ? scope->generation : 0;
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
  nostr_event_free(event);
  if (!valid || g_hash_table_contains(scope->seen, id))
    return;
  g_hash_table_add(scope->seen, g_strdup(id));
  g_queue_push_tail(&scope->seen_order, g_strdup(id));
  if (g_queue_get_length(&scope->seen_order) > GH_SEEN_LIMIT) {
    gchar *oldest = g_queue_pop_head(&scope->seen_order);
    g_hash_table_remove(scope->seen, oldest);
    g_free(oldest);
  }
  GhRelayUpdate update = { .notice = GH_RELAY_NOTICE_EVENT, .url = url,
                           .event_json = event_json, .event_id = id,
                           .backfill = !endpoint->eose };
  emit_update(scope, &update);
}

void
gh_relay_scope_eose(GhRelayScope *scope, const gchar *url)
{
  GhEndpoint *endpoint = active_endpoint(scope, url);
  if (!endpoint || endpoint->eose)
    return;
  endpoint->eose = TRUE;
  GhRelayUpdate update = { .notice = GH_RELAY_NOTICE_EOSE, .url = url };
  emit_update(scope, &update);
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
  if (notice == GH_RELAY_NOTICE_DISCONNECTED)
    endpoint->eose = FALSE;
  GhRelayUpdate update = { .notice = notice, .url = url,
                           .event_id = event_id, .detail = detail,
                           .accepted = accepted };
  emit_update(scope, &update);
}
